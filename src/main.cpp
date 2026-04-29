#include <Arduino.h>
#include <HardwareSerial.h>
#include <ModbusMaster.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <PubSubClient.h>

#define TX_PIN           13
#define RX_PIN           12
#define DE_RE_PIN        4
#define MAX485_POWER_PIN 5
#define waterstream_pin  32

unsigned long lastMoistureCheck  = 0;
const unsigned long MOISTURE_INTERVAL = 10UL * 60UL * 1000UL;

char ssid[]              = "EASV-Device";
char password[]          = "EASV6000";
char mqtt_server[]       = "10.176.210.220";
char unique_identifier[] = "sunfounder-client-easvfaetter";
const int mqtt_port      = 1883;

WiFiClient        espClient;
PubSubClient      client;          // no arg — set via setClient() in setup
HardwareSerial    ModbusSerial(1);
ModbusMaster      node;
LiquidCrystal_I2C lcd(0x27, 16, 2);
bool lcdReady = false;

void preTransmission()  { digitalWrite(DE_RE_PIN, HIGH); }
void postTransmission() { digitalWrite(DE_RE_PIN, LOW);  }

bool initLCD() {
  Wire.begin();
  Wire.beginTransmission(0x27);
  if (Wire.endTransmission() != 0) return false;
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Soil Sensor");
  lcd.setCursor(0, 1);
  lcd.print("Starting...");
  delay(1000);
  return true;
}

void reconnect() {
  while (!client.connected()) {
    if (client.connect(unique_identifier)) {
      Serial.println("MQTT connected");
    } else {
      Serial.print("MQTT failed, rc=");
      Serial.println(client.state());
      delay(5000);
    }
  }
}
void runPump() {

  if (!client.connected()) reconnect();
  client.loop();
  client.publish("esp32/jepstein/pump", "ON");
  digitalWrite(waterstream_pin, HIGH);
  delay(5000);
  digitalWrite(waterstream_pin, LOW);
}

void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");
  Serial.println(WiFi.localIP());
}

void checkMoisture() {
  uint8_t result = node.readHoldingRegisters(0x0000, 1);
  if (result == node.ku8MBSuccess) {
    float moisture = node.getResponseBuffer(0) / 10.0;
    if (moisture < 40.0) {
      Serial.println("Moisture low, running pump...");
      runPump();
    }
  } else {
    Serial.printf("Modbus error while checking moisture: 0x%02X\n", result);
  }
}

void setup() {
  Serial.begin(115200);
  setup_wifi();

  // Fix: bind transport before setServer
  client.setClient(espClient);
  client.setServer(mqtt_server, mqtt_port);

  pinMode(waterstream_pin, OUTPUT);
  digitalWrite(waterstream_pin, LOW);

  // MAX485 power — must come before Modbus init
  pinMode(MAX485_POWER_PIN, OUTPUT);
  digitalWrite(MAX485_POWER_PIN, LOW);
  delay(2000);
  digitalWrite(MAX485_POWER_PIN, HIGH);
  delay(500);

  pinMode(DE_RE_PIN, OUTPUT);
  digitalWrite(DE_RE_PIN, LOW);

  // Modbus init
  ModbusSerial.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);
  node.begin(1, ModbusSerial);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);

  // LCD
  Serial.println("Waiting for LCD...");
  while (!lcdReady) {
    lcdReady = initLCD();
    if (!lcdReady) {
      Serial.println("LCD not ready, retrying...");
      delay(500);
    }
  }
  Serial.println("LCD ready!");

  checkMoisture();
  lastMoistureCheck = millis();
}

void loop() {
  if (!client.connected()) reconnect();
  client.loop();

  uint8_t result = node.readHoldingRegisters(0x0000, 4);
  if (result == node.ku8MBSuccess) {
    float    moisture = node.getResponseBuffer(0) / 10.0;
    float    temp     = node.getResponseBuffer(1) / 10.0;
    uint16_t ec       = node.getResponseBuffer(2);
    float    ph       = node.getResponseBuffer(3) / 10.0;

    Serial.printf("Moisture: %.1f%%  Temp: %.1f°C  EC: %d µS/cm  pH: %.1f\n",
                  moisture, temp, ec, ph);

    if (millis() - lastMoistureCheck >= MOISTURE_INTERVAL) {
      lastMoistureCheck = millis();
      checkMoisture();
    }

    // Update LCD
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("M:"); lcd.print(moisture, 1); lcd.print("% T:"); lcd.print(temp, 1); lcd.print("C");
    lcd.setCursor(0, 1);
    lcd.print("EC:"); lcd.print(ec); lcd.print(" pH:"); lcd.print(ph, 1);

    // Publish to MQTT
    char payload[100];
    sprintf(payload, "{\"moisture\":%.1f,\"temp\":%.1f,\"ec\":%d,\"ph\":%.1f}",
            moisture, temp, ec, ph);
    client.publish("esp32/jepstein/readings", payload);

  } else {
    Serial.printf("Modbus error: 0x%02X\n", result);
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Modbus error!");
    lcd.setCursor(0, 1);
    lcd.print("0x"); lcd.print(result, HEX);
  }

  delay(3000);
}