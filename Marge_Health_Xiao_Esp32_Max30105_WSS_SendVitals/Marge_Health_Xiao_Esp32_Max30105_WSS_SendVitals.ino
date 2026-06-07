#include <WiFi.h>
#include <WebSocketsClient.h>
#include <Wire.h>
#include "MAX30105.h"
#include "spo2_algorithm.h"
#include <ArduinoJson.h>

#if __has_include("local_secrets.h")
#include "local_secrets.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID "HeySalad_02"
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "GutenTag%800"
#endif

#ifndef WS_HOST
#define WS_HOST "192.168.0.83"
#endif

#ifndef WS_PORT
#define WS_PORT 3015
#endif

#ifndef WS_PATH
#define WS_PATH "/"
#endif

#ifndef WS_USE_SSL
#define WS_USE_SSL 0
#endif

#ifndef MONITORING_AUTO_START
#define MONITORING_AUTO_START 1
#endif

// WiFi credentials
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// WebSocket settings
const char* wsHost = WS_HOST;
const int wsPort = WS_PORT;
const char* wsPath = WS_PATH;

WebSocketsClient webSocket;
MAX30105 particleSensor;
bool webSocketConfigured = false;

// Sensor data buffers and variables
#define BUFFER_LENGTH 100
uint32_t irBuffer[BUFFER_LENGTH];
uint32_t redBuffer[BUFFER_LENGTH];
const byte RATE_SIZE = 10;  
byte rates[RATE_SIZE];
byte rateSpot = 0;
byte rateCount = 0;
long lastBeat = 0;
float beatsPerMinute;
int beatAvg;
const int THRESHOLD = 500;
const int MIN_DISTANCE = 300;
long lastPeak = 0;
long recentMin = 0;
long recentMax = 0;
bool lookingForMax = true;
int32_t spo2;
int8_t validSPO2;
int32_t heartRate;
int8_t validHeartRate;
byte readIndex = 0;
byte sampleCount = 0;

bool isMonitoring = MONITORING_AUTO_START;

void webSocketEvent(WStype_t type, uint8_t* payload, size_t length);

void scanForConfiguredNetwork() {
    Serial.println("Scanning for configured WiFi network...");
    int networkCount = WiFi.scanNetworks();
    bool foundTarget = false;

    for (int i = 0; i < networkCount; i++) {
        if (WiFi.SSID(i) == ssid) {
            foundTarget = true;
            Serial.printf("Found configured SSID, RSSI: %d dBm, channel: %d\n", WiFi.RSSI(i), WiFi.channel(i));
            break;
        }
    }

    if (!foundTarget) {
        Serial.println("Configured SSID not found in scan results");
    }

    WiFi.scanDelete();
}

bool connectToWiFi(unsigned long timeoutMs = 12000) {
    WiFi.mode(WIFI_STA);

    if (WiFi.status() == WL_CONNECTED) {
        return true;
    }

    WiFi.disconnect(true);
    delay(250);

    scanForConfiguredNetwork();

    Serial.print("Connecting to WiFi SSID: ");
    Serial.println(ssid);
    WiFi.begin(ssid, password);

    unsigned long startedAt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startedAt < timeoutMs) {
        delay(500);
        Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
        return true;
    }

    Serial.printf("\nWiFi connection failed, status: %d\n", WiFi.status());
    Serial.println("Continuing in USB serial mode. Check hotspot visibility and 2.4 GHz compatibility for relay mode.");
    return false;
}

void configureWebSocket() {
    if (webSocketConfigured || WiFi.status() != WL_CONNECTED) {
        return;
    }

    if (WS_USE_SSL) {
        webSocket.beginSSL(wsHost, wsPort, wsPath);
    } else {
        webSocket.begin(wsHost, wsPort, wsPath);
    }
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(5000);
    webSocketConfigured = true;
    Serial.println("WebSocket client configured.");
}

void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch(type) {
        case WStype_CONNECTED: {
            Serial.printf("[WSc] Connected to: %s\n", payload);
            if (isMonitoring) {
                Serial.println("Monitoring auto-start enabled; vitals will send when sensor signal is valid.");
            }

            // Send JSON message upon connection
            StaticJsonDocument<200> doc;
            doc["type"] = "connection";
            doc["message"] = "Hello from ESP32 (JSON)";
            String connectionMsg;
            serializeJson(doc, connectionMsg);
            
            webSocket.sendTXT(connectionMsg);
            break;
        }
        
        case WStype_DISCONNECTED: {
            Serial.println("[WSc] Disconnected!");
            break;
        }
        
        case WStype_TEXT: {
            // Received text (should be JSON)
            Serial.printf("[WSc] Got text: %s\n", payload);

            StaticJsonDocument<200> doc;
            DeserializationError error = deserializeJson(doc, payload);

            if (!error) {
                if (doc.containsKey("command")) {
                    String command = doc["command"].as<String>();
                    if(command == "start") {
                        isMonitoring = true;
                        Serial.println("Monitoring started by JSON command");
                    } else if(command == "stop") {
                        isMonitoring = false;
                        Serial.println("Monitoring stopped by JSON command");
                    }
                }
            } else {
                Serial.println("Failed to parse received JSON");
            }
            break;
        }
        
        case WStype_BIN: {
            Serial.printf("[WSc] Got binary data, length: %u\n", length);
            break;
        }
        
        case WStype_ERROR: {
            Serial.printf("[WSc] Error\n");
            break;
        }
        
        case WStype_PING: {
            Serial.println("[WSc] Got ping");
            break;
        }
        
        case WStype_PONG: {
            Serial.println("[WSc] Got pong");
            break;
        }
        
        default: {
            break;
        }
    }
}

// Helper to send sensor data as JSON
void sendSensorData() {
    // Only send if we're actively monitoring
    if (!isMonitoring) {
        // Debug
        // Serial.println("Not monitoring, skipping send");
        return;
    }

    uint32_t currentIR = particleSensor.getIR();
    uint32_t currentRed = particleSensor.getRed();
    bool signalValid = currentIR > 50000;
    bool spo2Ready = validSPO2 && spo2 > 0 && spo2 <= 100;
    bool heartRateReady = validHeartRate && heartRate > 0;
    float spo2Estimate = 0;
    bool hasSpo2Estimate = false;

    if (signalValid && currentIR > 0 && currentRed > 0) {
        float redIrRatio = (float)currentRed / (float)currentIR;
        spo2Estimate = 110.0 - (25.0 * redIrRatio);
        spo2Estimate = constrain(spo2Estimate, 70.0, 100.0);
        hasSpo2Estimate = true;
    }

    StaticJsonDocument<384> doc;
    doc["bpm"] = beatsPerMinute;
    doc["avgBpm"] = beatAvg;
    doc["spo2Valid"] = spo2Ready;
    if (spo2Ready) {
        doc["spo2"] = spo2;
    } else {
        doc["spo2"].set(nullptr);
    }
    if (!spo2Ready && hasSpo2Estimate) {
        doc["spo2Estimate"] = round(spo2Estimate * 10.0) / 10.0;
    }
    doc["spo2Status"] = spo2Ready ? "valid" : (sampleCount < BUFFER_LENGTH ? "calibrating" : "estimate_only");
    if (heartRateReady) {
        doc["algorithmHeartRate"] = heartRate;
    } else {
        doc["algorithmHeartRate"].set(nullptr);
    }
    doc["sampleProgress"] = sampleCount;
    doc["ir"] = currentIR;
    doc["red"] = currentRed;
    doc["validReading"] = signalValid;

    String jsonString;
    serializeJson(doc, jsonString);
    
    Serial.println("Sent: " + jsonString); // USB serial dashboard output

    if(webSocket.isConnected()) {
        webSocket.sendTXT(jsonString);
    } else {
        Serial.println("WebSocket not connected; emitted data on USB serial only");
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println("\nStarting Marge Health Vitals Monitor...");
    Serial.print("WebSocket target: ");
    Serial.print(WS_USE_SSL ? "wss://" : "ws://");
    Serial.print(wsHost);
    Serial.print(":");
    Serial.print(wsPort);
    Serial.println(wsPath);
    Serial.printf("Monitoring auto-start: %s\n", isMonitoring ? "true" : "false");

    // Initialize sensor
    if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
        Serial.println("MAX30105 not found - check wiring");
        while (1) {
            delay(100);
        }
    }
    Serial.println("MAX30105 found!");

    // Configure sensor
    byte ledBrightness = 60;
    byte sampleAverage = 4;
    byte ledMode = 2;
    int sampleRate = 100;
    int pulseWidth = 411;
    int adcRange = 4096;
    
    particleSensor.setup(ledBrightness, sampleAverage, ledMode, sampleRate, pulseWidth, adcRange);

    // Initialize buffers
    for (byte i = 0; i < BUFFER_LENGTH; i++) {
        redBuffer[i] = 0;
        irBuffer[i] = 0;
    }
    
    if (connectToWiFi()) {
        Serial.println("\nWiFi connected!");
        Serial.print("IP address: ");
        Serial.println(WiFi.localIP());
        configureWebSocket();
    } else {
        Serial.println("\nWiFi offline. USB serial vitals are still available.");
    }

    Serial.println("Setup complete!");
}

void loop() {
    if (WiFi.status() == WL_CONNECTED) {
        configureWebSocket();
        webSocket.loop();
    } else {
        static unsigned long lastWifiRetry = 0;
        if (millis() - lastWifiRetry > 30000) {
            Serial.println("Retrying WiFi for relay mode...");
            if (connectToWiFi(6000)) {
                Serial.println("\nWiFi connected!");
                Serial.print("IP address: ");
                Serial.println(WiFi.localIP());
                configureWebSocket();
            }
            lastWifiRetry = millis();
        }
    }

    // Simple debug every 5 seconds
    static unsigned long lastCheck = 0;
    if (millis() - lastCheck > 5000) {
        Serial.printf("WiFi: %s, WebSocket Connected: %s, Monitoring: %s\n",
            WiFi.status() == WL_CONNECTED ? "connected" : "offline",
            webSocket.isConnected() ? "true" : "false",
            isMonitoring ? "true" : "false");
        lastCheck = millis();
    }

    // Keep checking the sensor
    while (particleSensor.available() == false) {
        particleSensor.check();  
    }

    uint32_t irValue = particleSensor.getIR();
    uint32_t redValue = particleSensor.getRed();

    // If a finger is detected (IR above 50k)
    if (irValue > 50000) {
        // Store samples
        irBuffer[readIndex] = irValue;
        redBuffer[readIndex] = redValue;
        
        // Basic peak detection
        if (lookingForMax) {
            if (irValue > recentMax) {
                recentMax = irValue;
            } else if ((recentMax - irValue) > THRESHOLD) {
                unsigned long now = millis();
                if ((now - lastPeak) > MIN_DISTANCE) {
                    float delta = (now - lastPeak) / 1000.0;
                    float bpm = 60.0 / delta;
                    
                    if (bpm >= 40 && bpm <= 180) {
                        beatsPerMinute = bpm;
                        rates[rateSpot++] = (byte)bpm;
                        rateSpot %= RATE_SIZE;
                        if (rateCount < RATE_SIZE) {
                            rateCount++;
                        }
                        
                        int sum = 0;
                        for (byte x = 0; x < rateCount; x++) {
                            sum += rates[x];
                        }
                        beatAvg = sum / rateCount;
                    }
                    lastPeak = now;
                }
                lookingForMax = false;
                recentMin = irValue;
            }
        } else {
            if (irValue < recentMin) {
                recentMin = irValue;
            } else if ((irValue - recentMin) > THRESHOLD) {
                lookingForMax = true;
                recentMax = irValue;
            }
        }

        readIndex++;
        if (sampleCount < BUFFER_LENGTH) {
            sampleCount++;
        }
        if (readIndex == BUFFER_LENGTH) {
            maxim_heart_rate_and_oxygen_saturation(
                irBuffer, BUFFER_LENGTH, 
                redBuffer, 
                &spo2, &validSPO2, 
                &heartRate, &validHeartRate
            );
            readIndex = 0;
        }

        // Send data if monitoring is active, every 100ms
        static unsigned long lastSend = 0;
        if (isMonitoring && (millis() - lastSend >= 100)) {
            Serial.println("Attempting to send data...");
            sendSensorData();
            lastSend = millis();
        }
    } else {
        // No finger detected, reset
        if(isMonitoring) {
            Serial.println("No finger detected");
        }
        beatsPerMinute = 0;
        beatAvg = 0;
        spo2 = 0;
        validSPO2 = 0;
        validHeartRate = 0;
        heartRate = 0;
        rateSpot = 0;
        rateCount = 0;
        sampleCount = 0;
        recentMax = 0;
        recentMin = 0;
        lookingForMax = true;

        static unsigned long lastNoFingerSend = 0;
        if (isMonitoring && (millis() - lastNoFingerSend >= 1000)) {
            sendSensorData();
            lastNoFingerSend = millis();
        }
    }

    // Move to the next sample
    particleSensor.nextSample();

    // Quick delay to reduce loop speed (and CPU usage)
    delay(50);
}
