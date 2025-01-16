// Marge Health BPM detection Algorithim with XIAO ESP32C3 + MAX30105
// By Peter Machona - 12.01.2025
// Potential Use case - Health monitoring


#include <Wire.h>
#include "MAX30105.h"

MAX30105 particleSensor;

// Circular buffer for readings
const int BUFFER_SIZE = 100;
long readings[BUFFER_SIZE];
int readIndex = 0;

// Variables for BPM calculation
const byte RATE_SIZE = 10;  
byte rates[RATE_SIZE];      
byte rateSpot = 0;
long lastBeat = 0;
float beatsPerMinute;
int beatAvg;

// Variables for peak detection
const int THRESHOLD = 500;  // Minimum rise for a peak
const int MIN_DISTANCE = 300; // Minimum ms between peaks (200 BPM max)
long lastPeak = 0;
long recentMin = 0;
long recentMax = 0;
bool lookingForMax = true;

void setup() {
  Serial.begin(115200);
  
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("MAX30105 not found - check wiring");
    while (1);
  }

  // Configure sensor
  particleSensor.setup(0x3F, 4, 2, 400, 411, 4096);
  particleSensor.setPulseAmplitudeRed(0x0A);
  particleSensor.setPulseAmplitudeIR(0x3F); // Increased IR LED power

  // Initialize buffer
  for (int i = 0; i < BUFFER_SIZE; i++) {
    readings[i] = 0;
  }
}

void loop() {
  // Read sensor
  long irValue = particleSensor.getIR();
  
  if (irValue > 50000) {
    // Store reading in circular buffer
    readings[readIndex] = irValue;
    readIndex = (readIndex + 1) % BUFFER_SIZE;
    
    // Find local min/max for peak detection
    if (lookingForMax) {
      if (irValue > recentMax) {
        recentMax = irValue;
      } else if ((recentMax - irValue) > THRESHOLD) {
        // Found a peak
        unsigned long now = millis();
        if ((now - lastPeak) > MIN_DISTANCE) {
          // Calculate BPM
          float delta = (now - lastPeak) / 1000.0;
          float bpm = 60.0 / delta;
          
          if (bpm >= 40 && bpm <= 180) {
            beatsPerMinute = bpm;
            rates[rateSpot++] = (byte)bpm;
            rateSpot %= RATE_SIZE;

            // Calculate average
            beatAvg = 0;
            for (byte x = 0; x < RATE_SIZE; x++) {
              beatAvg += rates[x];
            }
            beatAvg /= RATE_SIZE;
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

    // Print results
    Serial.print("IR=");
    Serial.print(irValue);
    Serial.print(", BPM=");
    Serial.print(beatsPerMinute, 1);
    Serial.print(", Avg BPM=");
    Serial.print(beatAvg);
    Serial.print(", Delta=");
    Serial.println(recentMax - recentMin);

  } else {
    Serial.println("No finger detected");
    beatsPerMinute = 0;
    beatAvg = 0;
    recentMax = 0;
    recentMin = 0;
    lookingForMax = true;
  }

  delay(50);
}
