#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <SoftwareSerial.h>

/* ---------- Wi-Fi Config ---------- */
#define WIFI_SSID     "AITTEST"
#define WIFI_PASSWORD "12345678"
#define DASHBOARD_IP   "192.168.1.100"  // Flask dashboard IP
#define DASHBOARD_PORT 5000

/* ---------- UART with STM32 ---------- */
SoftwareSerial STM32_UART(4, 5); // RX = D2, TX = D1

/* ---------- Alert Pins ---------- */
#define ALERT_LED_PIN 2      // D4 onboard LED
#define ALERT_BUZZER_PIN 14  // GPIO14 buzzer

/* ---------- Function Declarations ---------- */
void WiFi_Init();
bool Check_WiFi();
void Send_To_Dashboard(const String &jsonPayload);
void Process_Incoming_Data(const String &msg);
void Local_Alert(const String &msg);
bool Handshake_With_STM32();

/* ---------- Setup ---------- */
void setup() {
  pinMode(ALERT_LED_PIN, OUTPUT);
  pinMode(ALERT_BUZZER_PIN, OUTPUT);
  digitalWrite(ALERT_LED_PIN, HIGH);
  digitalWrite(ALERT_BUZZER_PIN, LOW);

  Serial.begin(9600);
  STM32_UART.begin(38400); // Use safe baud rate for SoftwareSerial
  delay(2000);

  Serial.println("\nESP12E EEG Receiver starting...");
  WiFi_Init();

  if (Check_WiFi())
    Serial.println("Wi-Fi connected successfully.");
  else
    Local_Alert("Wi-Fi Connection Failed!");

  Serial.println("Waiting for STM32 handshake...");

  if (Handshake_With_STM32()) {
    Serial.println("✅ Handshake successful. Ready for EEG data.\n");
  } else {
    Serial.println("❌ Handshake failed. Continuing anyway.\n");
  }
}

/* ---------- Main Loop ---------- */
void loop() {
  if (!Check_WiFi()) WiFi_Init();

if (STM32_UART.available()) {
  static String msg = "";
  while (STM32_UART.available()) {
    char c = STM32_UART.read();

    if (c == '<') {
      msg = "";  // start of new message
    } 
    else if (c == '>') {
      msg.trim();
      if (msg.length() > 0) {
        Process_Incoming_Data(msg);
      }
      msg = "";  // reset after processing
    } 
    else {
      msg += c;
    }
  }
}


  
  delay(10);
}

/* ============================================================
 * Handshake
 * ============================================================ */
bool Handshake_With_STM32() {
  unsigned long start = millis();
  while (millis() - start < 10000) { // 10 sec timeout
    if (STM32_UART.available()) {
      String msg = STM32_UART.readStringUntil('\n');
      msg.trim();

      if (msg.equalsIgnoreCase("HELLO_ESP")) {
        Serial.println("Received handshake request: " + msg);
        STM32_UART.println("Response");
        // Flash LED for confirmation
        for (int i = 0; i < 3; i++) {
          digitalWrite(ALERT_LED_PIN, LOW);
          delay(150);
          digitalWrite(ALERT_LED_PIN, HIGH);
          delay(150);
        }
        return true;
      }
    }
  }
  return false;
}

/* ============================================================
 * Wi-Fi Handling
 * ============================================================ */
void WiFi_Init() {
  Serial.println("Connecting to Wi-Fi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Wi-Fi connection failed.");
  }
}

bool Check_WiFi() {
  return (WiFi.status() == WL_CONNECTED);
}

/* ============================================================
 * Process Incoming UART Data
 * ============================================================ */
void Process_Incoming_Data(const String &msg) {
  Serial.print("Received from STM32: ");
  Serial.println(msg);

  if (msg.startsWith("{") && msg.endsWith("}")) {
    Send_To_Dashboard(msg);
  } else {
    String wrapped = "{\"data\":\"" + msg + "\"}";
    Send_To_Dashboard(wrapped);
  }

  if (msg.indexOf("SEIZURE") >= 0) {
    Local_Alert("SEIZURE DETECTED!");
  }
}

/* ============================================================
 * Send Data to Dashboard Server
 * ============================================================ */
void Send_To_Dashboard(const String &jsonPayload) {
  if (!Check_WiFi()) {
    Serial.println("Wi-Fi lost, cannot send data.");
    return;
  }

  WiFiClient client;
  HTTPClient http;

  String url = "http://" + String(DASHBOARD_IP) + ":" + String(DASHBOARD_PORT) + "/update";
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");

  int response = http.POST(jsonPayload);

  if (response > 0)
    Serial.printf("Data sent successfully. HTTP %d\n", response);
  else
    Serial.printf("Failed to send data. Code: %d\n", response);

  http.end();
}

/* ============================================================
 * Local Alert (LED + Buzzer)
 * ============================================================ */
void Local_Alert(const String &msg) {
  Serial.println("⚠️ Local Alert: " + msg);

  for (int i = 0; i < 3; i++) {
    digitalWrite(ALERT_LED_PIN, LOW);
    digitalWrite(ALERT_BUZZER_PIN, HIGH);
    delay(250);
    digitalWrite(ALERT_LED_PIN, HIGH);
    digitalWrite(ALERT_BUZZER_PIN, LOW);
    delay(250);
  }
}
