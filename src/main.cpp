#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ModbusIP_ESP8266.h>
#include <ModbusRTU.h>
#include <ArduinoOTA.h>              // ← OTA added

// ─────────────── CONFIG ───────────────
const char* WIFI_SSID     = "WB-310";
const char* WIFI_PASSWORD = "@Ur&@81$%G$";

// ─────────────── OTA CONFIG ───────────────
const char* OTA_HOSTNAME  = "ESP32-CNC02";
const char* OTA_PASSWORD  = "flash1234";

WiFiUDP udp;
const uint16_t DISCOVERY_PORT = 8888;

#define MAX_GATEWAYS 5
struct Gateway { String ip; uint16_t port; };
Gateway gateways[MAX_GATEWAYS];
int gatewayCount = 0;
WiFiClient opcClients[MAX_GATEWAYS];
bool opcConnected[MAX_GATEWAYS] = {};

ModbusIP mbTcp;
ModbusRTU mbRtu;

const uint8_t  RTU_SLAVE_ID = 1;
const uint32_t RTU_BAUD     = 9600;
const uint8_t  PIN_RS485_DE = 4;

const uint16_t REG_VOLTAGE     = 0;
const uint16_t REG_TEMPERATURE = 4;

#define TEMP_PIN 14
#define ROT_CLK  18
#define ROT_DT   19

OneWire oneWire(TEMP_PIN);
DallasTemperature sensors(&oneWire);

volatile int encoderCount = 0;
volatile int lastEncoded  = 0;

float g_voltage     = 24.0f;
float g_temperature = 25.0f;

// ───────── NON-BLOCKING DISCOVERY STATE ─────────
enum DiscoveryState { DISC_IDLE, DISC_SEND, DISC_WAIT };
DiscoveryState discState = DISC_IDLE;

unsigned long discLastAction = 0;
int discAttempt = 0;

const unsigned long DISC_INTERVAL  = 10000;
const unsigned long DISC_WAIT_TIME = 1000;
const int DISC_MAX_ATTEMPTS        = 5;

// ─────────────── HELPERS ───────────────
void IRAM_ATTR readEncoder() {
    int MSB = digitalRead(ROT_CLK);
    int LSB = digitalRead(ROT_DT);
    int encoded = (MSB << 1) | LSB;
    int sum = (lastEncoded << 2) | encoded;
    if (sum == 0b1101 || sum == 0b0100 || sum == 0b0010 || sum == 0b1011) encoderCount++;
    if (sum == 0b1110 || sum == 0b0111 || sum == 0b0001 || sum == 0b1000) encoderCount--;
    lastEncoded = encoded;
}

union FloatRegs { float f; uint16_t w[2]; };

void writeFloat(uint16_t startReg, float value) {
    FloatRegs d; d.f = value;
    mbTcp.Hreg(startReg,   d.w[1]); mbTcp.Hreg(startReg+1, d.w[0]);
    mbRtu.Hreg(startReg,   d.w[1]); mbRtu.Hreg(startReg+1, d.w[0]);
}

// ─────────────── WIFI ───────────────
void connectToWiFi() {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(200);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected → " + WiFi.localIP().toString());
}

// ─────────────── OTA SETUP ───────────────
void setupOTA() {
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);

    ArduinoOTA.onStart([]() {
        String type = (ArduinoOTA.getCommand() == U_FLASH) ? "firmware" : "filesystem";
        digitalWrite(PIN_RS485_DE, LOW);   // safe the RS485 bus before flashing
        Serial.println("\n[OTA] Starting — " + type);
    });

    ArduinoOTA.onEnd([]() {
        Serial.println("\n[OTA] Done — rebooting...");
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        static uint8_t lastPct = 255;
        uint8_t pct = (progress * 100) / total;
        if (pct != lastPct) {
            Serial.printf("[OTA] %d%%\r", pct);
            lastPct = pct;
        }
    });

    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("\n[OTA] Error[%u]: ", error);
        if      (error == OTA_AUTH_ERROR)    Serial.println("Auth failed");
        else if (error == OTA_BEGIN_ERROR)   Serial.println("Begin failed");
        else if (error == OTA_CONNECT_ERROR) Serial.println("Connect failed");
        else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive failed");
        else if (error == OTA_END_ERROR)     Serial.println("End failed");
    });

    ArduinoOTA.begin();
    Serial.println("[OTA] Ready → " + String(OTA_HOSTNAME) + ".local");
}

// ─────────────── NON-BLOCKING DISCOVERY ───────────────
void handleDiscovery() {
    switch (discState) {
        case DISC_IDLE:
            if (gatewayCount == 0 && millis() - discLastAction > DISC_INTERVAL) {
                Serial.println("\n[DISCOVERY] Starting...");
                udp.begin(DISCOVERY_PORT);
                discAttempt = 0;
                discState = DISC_SEND;
            }
            break;

        case DISC_SEND:
            if (discAttempt < DISC_MAX_ATTEMPTS) {
                udp.beginPacket("255.255.255.255", DISCOVERY_PORT);
                udp.print("WHO_IS_GATEWAY?");
                udp.endPacket();
                Serial.printf("[DISCOVERY] Broadcast %d\n", discAttempt + 1);
                discLastAction = millis();
                discState = DISC_WAIT;
            } else {
                Serial.println("[DISCOVERY] No gateways found");
                discState = DISC_IDLE;
                discLastAction = millis();
            }
            break;

        case DISC_WAIT:
            if (udp.parsePacket()) {
                char buf[128];
                int len = udp.read(buf, sizeof(buf) - 1);
                buf[len] = 0;
                String reply(buf);
                Serial.print("[DISCOVERY] Reply: ");
                Serial.println(reply);
                if (reply.startsWith("I_AM_GATEWAY")) {
                    int p1 = reply.indexOf(":");
                    int p2 = reply.lastIndexOf(":");
                    if (p1 > 0 && p2 > p1) {
                        String ip   = reply.substring(p1 + 1, p2);
                        uint16_t port = reply.substring(p2 + 1).toInt();
                        gateways[0] = { ip, port };
                        gatewayCount = 1;
                        Serial.println("[DISCOVERY] Gateway stored");
                    }
                }
                discState = DISC_IDLE;
                discLastAction = millis();
                break;
            }
            if (millis() - discLastAction > DISC_WAIT_TIME) {
                discAttempt++;
                discState = DISC_SEND;
            }
            break;
    }
}

// ─────────────── OPC CONNECTION ───────────────
void connectToOpcServers() {
    handleDiscovery();
    if (gatewayCount == 0) return;
    for (int i = 0; i < gatewayCount; i++) {
        if (opcClients[i].connected()) { opcConnected[i] = true; continue; }
        opcConnected[i] = opcClients[i].connect(gateways[i].ip.c_str(), gateways[i].port);
    }
}

// ─────────────── OPC JSON SEND ───────────────
void sendOpcData(float voltage, float temperature) {
    StaticJsonDocument<256> doc;
    doc["deviceId"]  = "ESP32_01";
    doc["timestamp"] = millis();
    doc["ns=2;s=Plant=MUMBAI_PLANT/Line=ASSEMBLY_01/Machine=CNC_02/Signal=VOLTAGE"] = voltage;
    doc["ns=2;s=Plant=MUMBAI_PLANT/Line=ASSEMBLY_01/Machine=CNC_02/Signal=TEMP"]    = temperature;
    char buf[256];
    size_t len = serializeJson(doc, buf);
    for (int i = 0; i < gatewayCount; i++)
        if (opcConnected[i] && opcClients[i].connected()) {
            opcClients[i].write((uint8_t*)buf, len);
            opcClients[i].write('\n');
        }
}

// ─────────────── SETUP ───────────────
void setup() {
    Serial.begin(115200);

    pinMode(ROT_CLK, INPUT_PULLUP);
    pinMode(ROT_DT,  INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(ROT_CLK), readEncoder, CHANGE);
    attachInterrupt(digitalPinToInterrupt(ROT_DT),  readEncoder, CHANGE);

    sensors.begin();
    connectToWiFi();
    setupOTA();              // ← OTA starts after WiFi is connected

    // Modbus TCP
    mbTcp.server();
    mbTcp.addHreg(REG_VOLTAGE);
    mbTcp.addHreg(REG_VOLTAGE     + 1);
    mbTcp.addHreg(REG_TEMPERATURE);
    mbTcp.addHreg(REG_TEMPERATURE + 1);

    // Modbus RTU
    Serial2.begin(RTU_BAUD, SERIAL_8N1, 16, 17);
    pinMode(PIN_RS485_DE, OUTPUT);
    digitalWrite(PIN_RS485_DE, LOW);

    mbRtu.begin(&Serial2, PIN_RS485_DE);
    mbRtu.slave(RTU_SLAVE_ID);

    mbRtu.onRequest([](Modbus::FunctionCode fc, Modbus::RequestData data)
        -> Modbus::ResultCode {
        Serial.print("RTU Request | FC: ");
        Serial.println((uint8_t)fc);
        return Modbus::EX_SUCCESS;
    });

    mbRtu.addHreg(REG_VOLTAGE);
    mbRtu.addHreg(REG_VOLTAGE     + 1);
    mbRtu.addHreg(REG_TEMPERATURE);
    mbRtu.addHreg(REG_TEMPERATURE + 1);
}

// ─────────────── LOOP ───────────────
void loop() {
    ArduinoOTA.handle();         // ← must be first, every loop

    static unsigned long lastTempMs = 0;
    static unsigned long lastOpcMs  = 0;
    static int lastCount = 0;

    // Highest priority
    mbRtu.task();
    mbTcp.task();

    // Encoder update
    int diff = encoderCount - lastCount;
    if (diff != 0) {
        g_voltage += diff * 0.1f;
        lastCount = encoderCount;
    }
    g_voltage = constrain(g_voltage, 15.0f, 30.0f);

    // Temperature every 1 sec
    if (millis() - lastTempMs >= 1000UL) {
        sensors.requestTemperatures();
        float t = sensors.getTempCByIndex(0);
        if (t != DEVICE_DISCONNECTED_C) g_temperature = t;
        lastTempMs = millis();
    }

    // Update Modbus registers
    writeFloat(REG_VOLTAGE,     g_voltage);
    writeFloat(REG_TEMPERATURE, g_temperature);

    // OPC send every 1 sec
    if (millis() - lastOpcMs >= 1000UL) {
        sendOpcData(g_voltage, g_temperature);
        lastOpcMs = millis();
    }

    // Lowest priority
    connectToOpcServers();
}