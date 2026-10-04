#include <DHTesp.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
 
// Pins
#define DHT_PIN 15
#define MQ2_PIN 34
#define SERVO_PIN 18
#define BUZZER_PIN 19
#define GREEN_LED 25
#define YELLOW_LED 26
#define ORANGE_LED 27
#define RED_LED 14
#define PURPLE_LED 13
 
// Wokwi Wi-Fi and ThingSpeak settings.
const char* WIFI_NAME = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";
const char* WRITE_API_KEY = "CQUVZ42RRJ6BKJ01";
 
DHTesp dht;
Servo fanServo; // Represents a vent position, not a real fan speed.
 
// Thresholds
const float FAN_ON_TEMP = 35.0;
const float FAN_OFF_TEMP = 32.0;
const float EMERGENCY_TEMP = 45.0;
const int SMOKE_THRESHOLD = 3000; // Raw ADC value, NOT ppm.
 
// State numbers uploaded to ThingSpeak are 0, 1, 2, 3 and 4.
enum State {
  SAFE,
  HEAT_CONTROL,
  SMOKE_ALERT,
  EMERGENCY,
  FAILSAFE
};
 
// Start in FAILSAFE until the first valid temperature reading.
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
 
// send an HTTPS GET and check the returned entry ID.
void uploadToThingSpeak() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Upload skipped: Wi-Fi is not connected.");
    return;
  }
  if (String(WRITE_API_KEY) == "PASTE_WRITE_API_KEY_HERE") {
    Serial.println("Upload skipped: add your ThingSpeak Write API Key.");
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
  // This request can briefly pause the loop, including buzzer timing.
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
  // Read the DHT every 2 seconds without delaying the whole loop.
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
 
  // Fan hysteresis: remember the demand between 32 and 35 C.
  if (sensorOK) {
    if (temperature >= FAN_ON_TEMP) fanEnabled = true;
    if (temperature <= FAN_OFF_TEMP) fanEnabled = false;
  } else {
    fanEnabled = false;
  }
 
  // State determination: a DHT failure must not silence a smoke alarm.
  if (smokeValue > SMOKE_THRESHOLD && sensorOK && temperature >= EMERGENCY_TEMP) {
    currentState = EMERGENCY;
  } else if (smokeValue > SMOKE_THRESHOLD) {
    currentState = SMOKE_ALERT;
  } else if (!sensorOK) {
    currentState = FAILSAFE;
  } else {
    currentState = fanEnabled ? HEAT_CONTROL : SAFE;
  }
 
  // State change notification, as in the original sketch.
  if (currentState != previousState) {
    Serial.println("---------------------");
    switch (currentState) {
      case SAFE:         Serial.println("State: SAFE"); break;
      case HEAT_CONTROL: Serial.println("State: HEAT CONTROL"); break;
      case SMOKE_ALERT:  Serial.println("State: SMOKE ALERT"); break;
      case EMERGENCY:    Serial.println("State: EMERGENCY"); break;
      case FAILSAFE:     Serial.println("State: FAILSAFE"); break;
    }
    // Reset the alarm timing when entering a different state.
    noTone(BUZZER_PIN);
    buzzerState = false;
    lastBuzzerToggle = millis();
    previousState = currentState;
  }
 
  showStateLED(currentState);
 
  // Output actions: 0 degrees means the simulated vent is closed.
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
      fanServo.write(0); // Interlock: close ventilation during smoke.
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
 
  // Retry Wi-Fi without waiting in a while loop.
  if (WiFi.status() != WL_CONNECTED && millis() - lastWiFiRetry >= 10000) {
    lastWiFiRetry = millis();
    WiFi.reconnect();
  }
  // Allow at least 20 seconds between cloud attempts.
  if (millis() - lastCloudUpload >= 20000) {
    uploadToThingSpeak();
    lastCloudUpload = millis();
  }
}
