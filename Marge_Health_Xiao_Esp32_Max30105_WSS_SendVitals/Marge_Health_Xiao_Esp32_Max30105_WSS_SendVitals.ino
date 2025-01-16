#include <WiFi.h>
#include <WebSocketsClient.h>
#include <Wire.h>
#include "MAX30105.h"
#include "spo2_algorithm.h"
#include <ArduinoJson.h>

// WiFi credentials
const char* ssid = "HeySalad_02";
const char* password = "GutenTag%800";

// WebSocket settings
const char* wsHost = "192.168.1.114";  // Your Mac/Server IP
const int wsPort = 3015;

WebSocketsClient webSocket;
MAX30105 particleSensor;

// Sensor data buffers and variables
#define BUFFER_LENGTH 100
uint32_t irBuffer[BUFFER_LENGTH];
uint32_t redBuffer[BUFFER_LENGTH];
const byte RATE_SIZE = 10;  
byte rates[RATE_SIZE];
byte rateSpot = 0;
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

bool isMonitoring = false;

void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch(type) {
        case WStype_CONNECTED: {
            Serial.printf("[WSc] Connected to: %s\n", payload);

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
            isMonitoring = false;
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

    StaticJsonDocument<200> doc;
    doc["bpm"] = beatsPerMinute;
    doc["avgBpm"] = beatAvg;
    doc["spo2"] = (validSPO2 && spo2 != -999) ? spo2 : 0;
    doc["ir"] = particleSensor.getIR();
    doc["red"] = particleSensor.getRed();
    doc["validReading"] = (particleSensor.getIR() > 50000);

    String jsonString;
    serializeJson(doc, jsonString);
    
    if(webSocket.isConnected()) {
        webSocket.sendTXT(jsonString);
        Serial.println("Sent: " + jsonString); // Debug print
    } else {
        Serial.println("WebSocket not connected, can't send data");
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println("\nStarting Marge Health Vitals Monitor...");

    // Connect to WiFi
    Serial.println("Connecting to WiFi...");
    WiFi.begin(ssid, password);

    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    
    Serial.println("\nWiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    // Configure WebSocket
    webSocket.begin(wsHost, wsPort, "/");
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(5000);

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
    
    Serial.println("Setup complete!");
}

void loop() {
    // Continuously handle WebSocket events
    webSocket.loop();

    // Simple debug every 5 seconds
    static unsigned long lastCheck = 0;
    if (millis() - lastCheck > 5000) {
        Serial.printf("WebSocket Connected: %s, Monitoring: %s\n",
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
                        
                        int sum = 0;
                        for (byte x = 0; x < RATE_SIZE; x++) {
                            sum += rates[x];
                        }
                        beatAvg = sum / RATE_SIZE;
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
        recentMax = 0;
        recentMin = 0;
        lookingForMax = true;
    }

    // Move to the next sample
    particleSensor.nextSample();

    // Quick delay to reduce loop speed (and CPU usage)
    delay(50);
}
