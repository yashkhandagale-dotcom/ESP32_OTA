#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>

const char* OTA_HOSTNAME = "ESP32-CNC02";
const char* OTA_PASSWORD = "flash1234";

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n=== ESP32 Fresh Setup ===");

    // ❌ DO NOT add WiFi.disconnect here now

    WiFiManager wm;

    // Optional timeout (3 mins)
    wm.setConfigPortalTimeout(180);

    // 🔥 This will open hotspot if no WiFi is saved
    if (!wm.autoConnect("ESP32-SETUP")) {
        Serial.println("Failed to connect. Restarting...");
        delay(3000);
        ESP.restart();
    }

    Serial.println("\n✅ WiFi Connected!");
    Serial.print("SSID: ");
    Serial.println(WiFi.SSID());

    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    // 🔹 OTA Setup
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);

    ArduinoOTA.onStart([]() {
        Serial.println("\n[OTA] Start");
    });

    ArduinoOTA.onEnd([]() {
        Serial.println("\n[OTA] End");
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("[OTA] %u%%\r", (progress * 100) / total);
    });

    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("[OTA] Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
        else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
        else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
        else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });

    ArduinoOTA.begin();

    Serial.println("[OTA] Ready");
    Serial.println("=== Setup Complete ===");
}

void loop() {
    ArduinoOTA.handle();
}