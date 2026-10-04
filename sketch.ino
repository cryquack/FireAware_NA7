#include <DHTesp.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
 
// Pin connections
#define DHT_PIN 15
#define MQ2_PIN 34
#define SERVO_PIN 18
#define BUZZER_PIN 19
#define GREEN_LED 25
#define YELLOW_LED 26
#define ORANGE_LED 27
#define RED_LED 14
#define PURPLE_LED 13
 
// Wi-Fi and ThingSpeak settings
const char* WIFI_NAME = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";
const char* WRITE_API_KEY = "OUR_SECRET_API_KEY";
 
DHTesp dht;
Servo fanServo;
 
// Thresholds
const float FAN_ON_TEMP = 35.0;
const float FAN_OFF_TEMP = 32.0;
const float EMERGENCY_TEMP = 45.0;
const int SMOKE_THRESHOLD = 3000;
 
// System states
enum State {
  SAFE,
  HEAT_CONTROL,
  SMOKE_ALERT,
  EMERGENCY,
  FAILSAFE
};
 
State currentState = FAILSAFE;
State previousState = FAILSAFE;
float temperature = NAN;
float humidity = NAN;
int smokeValue = 0;
bool sensorOK = false;
bool fanEnabled = false;
bool buzzerState = false;
unsigned long lastSensorRead = 0;
unsigned long lastBuzzerToggle = 0;
unsigned long lastCloudUpload = 0;
unsigned long lastWiFiRetry = 0;
 
void allLEDsOff() {
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(YELLOW_LED, LOW);
  digitalWrite(ORANGE_LED, LOW);
  digitalWrite(RED_LED, LOW);
  digitalWrite(PURPLE_LED, LOW);
}
 
void showStateLED(State state) {
  allLEDsOff();
  switch (state) {
    case SAFE:         digitalWrite(GREEN_LED, HIGH); break;
    case HEAT_CONTROL: digitalWrite(YELLOW_LED, HIGH); break;
    case SMOKE_ALERT:  digitalWrite(ORANGE_LED, HIGH); break;
    case EMERGENCY:    digitalWrite(RED_LED, HIGH); break;
    case FAILSAFE:     digitalWrite(PURPLE_LED, HIGH); break;
  }
}
 
void uploadToThingSpeak() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Upload skipped: Wi-Fi is not connected.");
    return;
  }
  if (String(WRITE_API_KEY) == "OUR_SECRET_API_KEY") {
    Serial.println("Upload skipped: ThingSpeak API key is not set.");
    return;
  }
 
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = "https://api.thingspeak.com/update?api_key=";
  url += WRITE_API_KEY;
  if (sensorOK) {
    url += "&field1=" + String(temperature, 1);
    url += "&field2=" + String(humidity, 1);
  }
  url += "&field3=" + String(smokeValue);
  url += "&field4=" + String((int)currentState);
  if (sensorOK) url += "&status=DHT_OK";
  else url += "&status=DHT_FAULT";
 
  http.setConnectTimeout(2000);
  http.setTimeout(2000);
  if (!http.begin(client, url)) {
    Serial.println("Could not start cloud request.");
    return;
  }
  int responseCode = http.GET();
  String entryID = "";
  if (responseCode > 0) entryID = http.getString();
  if (responseCode == 200 && entryID.toInt() > 0) {
    Serial.print("ThingSpeak saved entry: ");
    Serial.println(entryID);
  } else {
    Serial.print("Upload failed. HTTP code: ");
    Serial.println(responseCode);
  }
  http.end();
}
 
void setup() {
  Serial.begin(115200);
  dht.setup(DHT_PIN, DHTesp::DHT22);
  analogReadResolution(12);
  fanServo.attach(SERVO_PIN);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(YELLOW_LED, OUTPUT);
  pinMode(ORANGE_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(PURPLE_LED, OUTPUT);
  fanServo.write(0);
  noTone(BUZZER_PIN);
  showStateLED(FAILSAFE);
  WiFi.begin(WIFI_NAME, WIFI_PASSWORD);
  Serial.println("FireAware Started - waiting for first sensor reading.");
}
 
void loop() {
  // Read temperature and humidity every 2 seconds
  if (millis() - lastSensorRead >= 2000) {
    lastSensorRead = millis();
    TempAndHumidity data = dht.getTempAndHumidity();
    temperature = data.temperature;
    humidity = data.humidity;
    sensorOK = !isnan(temperature) && !isnan(humidity)
               && temperature >= -40 && temperature <= 80
               && humidity >= 0 && humidity <= 100;
    Serial.print("Temp: "); Serial.print(temperature);
    Serial.print(" C | Humidity: "); Serial.print(humidity);
    Serial.print("% | Smoke ADC: "); Serial.println(analogRead(MQ2_PIN));
  }
  smokeValue = analogRead(MQ2_PIN);
 
  // Temperature control with hysteresis
  if (sensorOK) {
    if (temperature >= FAN_ON_TEMP) fanEnabled = true;
    if (temperature <= FAN_OFF_TEMP) fanEnabled = false;
  } else {
    fanEnabled = false;
  }
 
  // Choose the current system state
  if (smokeValue > SMOKE_THRESHOLD && sensorOK && temperature >= EMERGENCY_TEMP) {
    currentState = EMERGENCY;
  } else if (smokeValue > SMOKE_THRESHOLD) {
    currentState = SMOKE_ALERT;
  } else if (!sensorOK) {
    currentState = FAILSAFE;
  } else {
    currentState = fanEnabled ? HEAT_CONTROL : SAFE;
  }
 
  // Print state changes
  if (currentState != previousState) {
    Serial.println("---------------------");
    switch (currentState) {
      case SAFE:         Serial.println("State: SAFE"); break;
      case HEAT_CONTROL: Serial.println("State: HEAT CONTROL"); break;
      case SMOKE_ALERT:  Serial.println("State: SMOKE ALERT"); break;
      case EMERGENCY:    Serial.println("State: EMERGENCY"); break;
      case FAILSAFE:     Serial.println("State: FAILSAFE"); break;
    }
    noTone(BUZZER_PIN);
    buzzerState = false;
    lastBuzzerToggle = millis();
    previousState = currentState;
  }
 
  showStateLED(currentState);
 
  // Control the LEDs, vent and buzzer
  switch (currentState) {
    case SAFE:
      fanServo.write(0);
      noTone(BUZZER_PIN);
      break;
    case HEAT_CONTROL:
      if (temperature < 40.0) fanServo.write(90);
      else fanServo.write(180);
      noTone(BUZZER_PIN);
      break;
    case SMOKE_ALERT:
      fanServo.write(0);
      if (millis() - lastBuzzerToggle >= 250) {
        lastBuzzerToggle = millis();
        buzzerState = !buzzerState;
        if (buzzerState) tone(BUZZER_PIN, 1000);
        else noTone(BUZZER_PIN);
      }
      break;
    case EMERGENCY:
      fanServo.write(0);
      tone(BUZZER_PIN, 2000);
      break;
    case FAILSAFE:
      fanServo.write(0);
      noTone(BUZZER_PIN);
      break;
  }
 
  // Retry the Wi-Fi connection every 10 seconds
  if (WiFi.status() != WL_CONNECTED && millis() - lastWiFiRetry >= 10000) {
    lastWiFiRetry = millis();
    WiFi.reconnect();
  }
  // Upload data every 20 seconds
  if (millis() - lastCloudUpload >= 20000) {
    uploadToThingSpeak();
    lastCloudUpload = millis();
  }
}
