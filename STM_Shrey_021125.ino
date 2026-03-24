/*******************************************************************
 * STM32F446RE + ESP12E UART Handshake + Continuous EEG Seizure Detection
 * - Uses PC11 (RX) and PC10 (TX) for UART3
 * - Starts only after successful handshake with ESP12E
 * - Sends EEG analysis JSON packets continuously
 *******************************************************************/

#include <Arduino.h>
#include <arduinoFFT.h>
#include <math.h>

/* ---------------- UART Pins & LED ---------------- */
#define LED_PIN PB13
#define ESP_RX_PIN PC11   // STM32 RX (connects to ESP TX)
#define ESP_TX_PIN PC10   // STM32 TX (connects to ESP RX)

/* ---------------- Create HardwareSerial ---------------- */
HardwareSerial Serial3(USART3);  // Use USART3 instance directly

/* ---------------- EEG Constants ---------------- */
#define EEG_BANDS 4
#define BUFFER_SIZE 512
#define SAMPLE_RATE 256

/* ---------------- Buffers ---------------- */
float adc_buffer[BUFFER_SIZE];
float filtered_buffer[BUFFER_SIZE];
float features[16];
uint8_t seizure_flag = 0;
unsigned long cycle_count = 0;

/* ---------------- Pins ---------------- */
#define EEG_PIN A0

/* ================================================================
 *  SETUP
 * ================================================================ */
void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.begin(9600);         // USB debug
  Serial3.setRx(ESP_RX_PIN);  // assign pins safely
  Serial3.setTx(ESP_TX_PIN);
  Serial3.begin(38400);      // ESP UART
  delay(2000);

  Serial.println("\n=== STM32: EEG + Handshake System Starting ===");

  pinMode(EEG_PIN, INPUT);

  if (handshakeWithESP()) {
    Serial.println("✅ Handshake successful, starting EEG loop...");
    digitalWrite(LED_PIN, HIGH);
  } else {
    Serial.println("❌ Handshake failed. Halting...");
    while (1) {
      digitalWrite(LED_PIN, millis() % 500 < 250 ? HIGH : LOW);
      delay(100);
    }
  }
}

/* ================================================================
 *  HANDSHAKE FUNCTION
 * ================================================================ */
bool handshakeWithESP() {
  for (int attempt = 0; attempt < 5; ++attempt) {
    Serial.println("STM32: send HELLO_ESP (attempt " + String(attempt + 1) + ")");
    Serial3.println("HELLO_ESP");

    unsigned long t0 = millis();
    while (millis() - t0 < 2000) {
      if (Serial3.available()) {
        String r = Serial3.readStringUntil('\n');
        r.trim();
        if (r == "Response") {
          Serial.println("STM32: got '" + r + "'");
          Serial.println("STM32: handshake OK");
          digitalWrite(LED_PIN, HIGH);
          return true;
        }
      }
    }
    digitalWrite(LED_PIN, LOW);
    delay(500);
  }
  return false;
}

/* ================================================================
 *  MAIN LOOP
 * ================================================================ */
void loop() {
  cycle_count++;
  Serial.print("\n===== EEG Cycle [");
  Serial.print(cycle_count);
  Serial.println("] =====");

  Acquire_EEG();
  Filter_Signal(adc_buffer, filtered_buffer, BUFFER_SIZE);
  Extract_Features(filtered_buffer);

  String state;
  if (features[2] < 0.25) {
    state = "SENSOR INACTIVE";
    Serial.println("⚠️ Sensor seems inactive!");
  } else {
    seizure_flag = Detect_Seizure(features);
    if (seizure_flag) {
      state = "SEIZURE";
      Serial.println("⚠️ Seizure Detected!");
    } else {
      state = "NORMAL";
      Serial.println("✅ Normal EEG.");
    }
  }

  WiFi_Send_Alert(features, state.c_str());
  delay(2000);
}

/* ================================================================
 * 1. Data Acquisition
 * ================================================================ */
void Acquire_EEG() {
  Serial.println("[1] Acquiring EEG samples...");
  unsigned long interval = 1000 / SAMPLE_RATE;

  for (int i = 0; i < BUFFER_SIZE; i++) {
    int raw = analogRead(EEG_PIN);
    if (raw == 0) raw = random(1600, 1800); // simulate if disconnected
    adc_buffer[i] = raw * (3.3f / 4095.0f);
    delayMicroseconds(interval * 1000);
  }

  Serial.println("[2] EEG samples acquired.");
}

/* ================================================================
 * 2. Filtering (Bandpass 0.5–40Hz)
 * ================================================================ */
void Filter_Signal(float* input, float* output, uint16_t length) {
  const float bp_a[3] = {1.0, -1.5610181, 0.6413515};
  const float bp_b[3] = {0.8005924, 0.0, -0.8005924};

  float x1 = 0, x2 = 0, y1 = 0, y2 = 0;

  for (uint16_t i = 0; i < length; i++) {
    float x = input[i];
    float y = bp_b[0]*x + bp_b[1]*x1 + bp_b[2]*x2 - bp_a[1]*y1 - bp_a[2]*y2;
    x2 = x1; x1 = x; y2 = y1; y1 = y;
    output[i] = y;
  }
}

/* ================================================================
 * 3. Feature Extraction
 * ================================================================ */
void Extract_Features(float* signal) {
  Compute_RMS(signal, BUFFER_SIZE, &features[0]);
  Compute_Variance(signal, BUFFER_SIZE, &features[1]);
  Compute_Hjorth(signal, BUFFER_SIZE, &features[2], &features[3], &features[4]);
  Compute_Bandpower(signal, BUFFER_SIZE, &features[5]);
  Compute_Entropy(signal, BUFFER_SIZE, &features[9]);

  Serial.print("Activity: "); Serial.println(features[2]);
  Serial.print("Mobility: "); Serial.println(features[3]);
  Serial.print("Complexity: "); Serial.println(features[4]);
  Serial.print("Variance: "); Serial.println(features[1]);
}

/* ---------------- RMS ---------------- */
void Compute_RMS(float* input, uint16_t len, float* rms) {
  float sum = 0.0;
  for (uint16_t i = 0; i < len; i++) sum += input[i]*input[i];
  *rms = sqrt(sum / len);
}

/* ---------------- Variance ---------------- */
void Compute_Variance(float* input, uint16_t len, float* var) {
  float mean = 0.0;
  for (uint16_t i = 0; i < len; i++) mean += input[i];
  mean /= len;
  float sum_sq = 0.0;
  for (uint16_t i = 0; i < len; i++) sum_sq += pow(input[i] - mean, 2);
  *var = sum_sq / len;
}

/* ---------------- Hjorth ---------------- */
void Compute_Hjorth(float* x, uint16_t len, float* activity, float* mobility, float* complexity) {
  if (len < 3) return;
  float mean = 0.0;
  for (uint16_t i = 0; i < len; i++) mean += x[i];
  mean /= len;

  float var_x = 0.0;
  for (uint16_t i = 0; i < len; i++) var_x += pow(x[i] - mean, 2);
  var_x /= len;
  *activity = var_x;

  float diff1_sum = 0.0, diff2_sum = 0.0;
  for (uint16_t i = 1; i < len; i++) diff1_sum += pow(x[i] - x[i-1], 2);
  for (uint16_t i = 2; i < len; i++) diff2_sum += pow((x[i]-x[i-1])-(x[i-1]-x[i-2]), 2);

  float var_diff1 = diff1_sum / (len - 1);
  float var_diff2 = diff2_sum / (len - 2);

  *mobility = sqrt(var_diff1 / var_x);
  *complexity = sqrt(var_diff2 / var_diff1) / (*mobility);
}

/* ---------------- Bandpower ---------------- */
void Compute_Bandpower(float* input, uint16_t len, float* bandpower_array) {
  const double Fs = SAMPLE_RATE;
  const double delta[2] = {0.5, 4.0};
  const double theta[2] = {4.0, 8.0};
  const double alpha[2] = {8.0, 13.0};
  const double beta[2]  = {13.0, 30.0};

  double vReal[len], vImag[len];
  for (int i = 0; i < len; i++) { vReal[i] = input[i]; vImag[i] = 0; }

  ArduinoFFT FFT = ArduinoFFT(vReal, vImag, len, Fs);
  FFT.windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD);
  FFT.compute(FFT_FORWARD);
  FFT.complexToMagnitude();

  for (uint8_t i = 0; i < EEG_BANDS; i++) bandpower_array[i] = 0.0;

  for (uint16_t i = 1; i < len/2; i++) {
    double freq = i * (Fs / len);
    double mag = vReal[i] * vReal[i];
    if (freq >= delta[0] && freq < delta[1]) bandpower_array[0] += mag;
    else if (freq >= theta[0] && freq < theta[1]) bandpower_array[1] += mag;
    else if (freq >= alpha[0] && freq < alpha[1]) bandpower_array[2] += mag;
    else if (freq >= beta[0] && freq < beta[1])  bandpower_array[3] += mag;
  }
}

/* ---------------- Entropy ---------------- */
void Compute_Entropy(float* input, uint16_t len, float* entropy) {
  const uint8_t bins = 16;
  uint16_t hist[bins] = {0};

  float minVal = input[0], maxVal = input[0];
  for (uint16_t i = 1; i < len; i++) {
    if (input[i] < minVal) minVal = input[i];
    if (input[i] > maxVal) maxVal = input[i];
  }
  float range = maxVal - minVal;
  if (range == 0) { *entropy = 0; return; }

  for (uint16_t i = 0; i < len; i++) {
    uint8_t bin = (uint8_t)(((input[i]-minVal)/range) * (bins-1));
    hist[bin]++;
  }

  float H = 0.0;
  for (uint8_t i = 0; i < bins; i++) {
    if (hist[i] > 0) {
      float p = (float)hist[i] / len;
      H -= p * log2(p);
    }
  }
  *entropy = H;
}

/* ================================================================
 * 4. Detection Logic
 * ================================================================ */
uint8_t Detect_Seizure(float* f) {
  float rms_thresh = 50.0;
  float var_thresh = 200.0;
  float delta_power_thresh = 1000.0;
  float entropy_thresh = 3.5;

  if ((f[0] > rms_thresh || f[1] > var_thresh) &&
      (f[5] > delta_power_thresh) &&
      (f[9] < entropy_thresh))
    return 1;
  else
    return 0;
}

/* ================================================================
 * 5. Communication - Send JSON via UART
 * ================================================================ */
void WiFi_Send_Alert(float* f, const char* state) {
if (f == nullptr || state == nullptr) return;

// --- Build string safely ---
char buffer[512];
char fstr[10][12];

for (int i = 0; i < 10; i++) {
  dtostrf(f[i], 6, 4, fstr[i]);  // float to string conversion
}

snprintf(buffer, sizeof(buffer),
         "{ \"rms\": %s, \"variance\": %s, \"activity\": %s, "
         "\"mobility\": %s, \"complexity\": %s, "
         "\"delta_power\": %s, \"theta_power\": %s, "
         "\"alpha_power\": %s, \"beta_power\": %s, "
         "\"entropy\": %s, \"state\": \"%s\" }",
         fstr[0], fstr[1], fstr[2], fstr[3], fstr[4],
         fstr[5], fstr[6], fstr[7], fstr[8], fstr[9], state);

// --- Framed transmit with explicit delays and flushes ---
Serial3.write('<');                     // Start marker
Serial3.write((uint8_t*)buffer, strlen(buffer)); // Send entire JSON at once
Serial3.write('>');                     // End marker
Serial3.write('\n');                    // Optional newline for readability
Serial3.flush();                        // Wait until all data is physically sent
delay(10);                              // Short delay to ensure UART TX completion

// --- Debug on Serial Monitor ---
Serial.println(F("[UART] Sent data:"));
Serial.println(buffer);
}
