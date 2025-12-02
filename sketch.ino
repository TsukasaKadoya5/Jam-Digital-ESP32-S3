/* Jam Digital ESP32-S3 - Multicore + Mutex + Semaphore
   Fitur:
   - TimeTask di Core 0 (tick pakai esp_timer_get_time)
   - Buzzer, LED, Display, Button di Core 1
   - Potensiometer mengatur "volume": < THRESH => mute, max => keras
   - OLED SSD1306 menampilkan jam tanpa terpotong
   - Encoder mengubah menit (ISR)
   - Semaphore: alarmSem untuk memberi sinyal ke BuzzerTask
   - Mutex: protect alarmRinging & strikeRemaining
*/

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "esp_timer.h"

// ---------- Pins ----------
#define PIN_LED        2
#define PIN_BUZZER     10
#define PIN_BTN_STOP   15   // tombol stop alarm
#define PIN_ENC_CLK    4
#define PIN_ENC_DT     5
#define PIN_ENC_SW     7
#define PIN_POT        6    // ADC pin (pastikan pin valid untuk ADC pada boardmu)

// ---------- OLED ----------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ---------- Time (shared) ----------
volatile int hourNow   = 5;
volatile int minuteNow = 29;
volatile int secondNow = 50;

// alarm config
const int alarmHour   = 5;
const int alarmMinute = 30;

// cuckoo strike
volatile int strikeRemaining = 0;

// alarm flag (protected by mutex)
bool alarmRinging = false;

// tick baseline
uint64_t lastSecondMicros = 0;

// ---------- Sync primitives ----------
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED; // protect time vars between ISR & tasks
SemaphoreHandle_t dataMutex;    // protect alarmRinging & strikeRemaining
SemaphoreHandle_t alarmSem;     // signal from TimeTask to BuzzerTask

// ---------- Encoder state ----------
volatile int lastClk = HIGH;

// ---------- Potentiometer thresholds/freq ----------
const int POT_MIN_THRESHOLD = 50;       // below this => mute
const int BUZZER_FREQ_MIN = 800;        // lowest audible freq when pot > threshold
const int BUZZER_FREQ_MAX = 3000;       // highest freq

// ---------- Forward declarations ----------
void IRAM_ATTR encoderISR();
void timeTask(void *pvParameters);
void buzzerTask(void *pvParameters);
void ledTask(void *pvParameters);
void displayTask(void *pvParameters);
void buttonTask(void *pvParameters);
void copyTime(int &h, int &m, int &s);

// ---------- Helper: copy time safely ----------
void copyTime(int &h, int &m, int &s) {
  portENTER_CRITICAL(&mux);
  h = hourNow; m = minuteNow; s = secondNow;
  portEXIT_CRITICAL(&mux);
}

// ---------- ISR for encoder ----------
void IRAM_ATTR encoderISR() {
  portENTER_CRITICAL_ISR(&mux);
  int clk = digitalRead(PIN_ENC_CLK);
  int dt  = digitalRead(PIN_ENC_DT);

  if (clk != lastClk && clk == HIGH) {
    if (dt == LOW) { // putar kanan -> menit++
      minuteNow++;
      if (minuteNow >= 60) { minuteNow = 0; hourNow = (hourNow + 1) % 24; }
    } else { // putar kiri -> menit--
      minuteNow--;
      if (minuteNow < 0) { minuteNow = 59; hourNow = (hourNow + 23) % 24; }
    }
    secondNow = 0;
  }
  lastClk = clk;
  portEXIT_CRITICAL_ISR(&mux);
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  delay(50);

  // pins
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_BTN_STOP, INPUT_PULLUP);
  pinMode(PIN_ENC_CLK, INPUT_PULLUP);
  pinMode(PIN_ENC_DT, INPUT_PULLUP);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);

  lastClk = digitalRead(PIN_ENC_CLK);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_CLK), encoderISR, CHANGE);

  // I2C for OLED: adjust SDA/SCL pins to your board if needed
  Wire.begin(20, 19);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 allocation failed");
    while (true) delay(10);
  }
  display.clearDisplay();
  display.display();

  // baseline micros
  lastSecondMicros = esp_timer_get_time();

  // create sync objects
  dataMutex = xSemaphoreCreateMutex();
  if (dataMutex == NULL) {
    Serial.println("Failed to create data mutex");
    while (true) delay(10);
  }
  alarmSem = xSemaphoreCreateBinary();
  if (alarmSem == NULL) {
    Serial.println("Failed to create alarm semaphore");
    while (true) delay(10);
  }

  // create tasks
  xTaskCreatePinnedToCore(timeTask, "TimeTask", 4096, NULL, 3, NULL, 0);   // core 0
  xTaskCreatePinnedToCore(buzzerTask, "BuzzerTask", 4096, NULL, 2, NULL, 1); // core 1
  xTaskCreatePinnedToCore(ledTask, "LEDTask", 2048, NULL, 1, NULL, 1);       // core 1
  xTaskCreatePinnedToCore(displayTask, "DisplayTask", 4096, NULL, 1, NULL, 1); // core 1
  xTaskCreatePinnedToCore(buttonTask, "ButtonTask", 2048, NULL, 2, NULL, 1); // core 1

  Serial.println("Setup complete");
}

// ---------- TimeTask (Core 0) ----------
void timeTask(void *pvParameters) {
  (void) pvParameters;
  for (;;) {
    uint64_t nowMicros = esp_timer_get_time();
    if (nowMicros - lastSecondMicros >= 1000000ULL) {
      // advance baseline (handles occasional jitter)
      lastSecondMicros += 1000000ULL;

      // update time under critical
      portENTER_CRITICAL(&mux);
      secondNow++;
      if (secondNow >= 60) {
        secondNow = 0;
        minuteNow++;
        if (minuteNow >= 60) {
          minuteNow = 0;
          hourNow = (hourNow + 1) % 24;
        }
      }
      int h = hourNow;
      int m = minuteNow;
      int s = secondNow;
      portEXIT_CRITICAL(&mux);

      // check alarm exact match (HH:MM:00)
      if (h == alarmHour && m == alarmMinute && s == 0) {
        // set alarmRinging flag
        if (xSemaphoreTake(dataMutex, (TickType_t) 10) == pdTRUE) {
          alarmRinging = true;
          xSemaphoreGive(dataMutex);
        }
        // notify buzzer task
        xSemaphoreGive(alarmSem);
      }

      // check hourly strike at HH:00:00
      if (m == 0 && s == 0) {
        if (xSemaphoreTake(dataMutex, (TickType_t) 10) == pdTRUE) {
          if (strikeRemaining == 0 && !alarmRinging) {
            int hh;
            portENTER_CRITICAL(&mux);
            hh = hourNow % 12;
            portEXIT_CRITICAL(&mux);
            if (hh == 0) hh = 12;
            strikeRemaining = hh;
            // notify buzzer task
            xSemaphoreGive(alarmSem);
          }
          xSemaphoreGive(dataMutex);
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ---------- BuzzerTask (Core 1) ----------
void buzzerTask(void *pvParameters) {
  (void) pvParameters;
  noTone(PIN_BUZZER);

  for (;;) {
    // wait for alarm/strike signal
    if (xSemaphoreTake(alarmSem, portMAX_DELAY) == pdTRUE) {
      // read current status
      bool doAlarm = false;
      int strikes = 0;
      if (xSemaphoreTake(dataMutex, (TickType_t) 50) == pdTRUE) {
        doAlarm = alarmRinging;
        strikes = strikeRemaining;
        xSemaphoreGive(dataMutex);
      }

      // handle strikes first
      while (strikes > 0) {
        int potVal = analogRead(PIN_POT); // 0..4095

        if (potVal < POT_MIN_THRESHOLD) {
          noTone(PIN_BUZZER); // mute
        } else {
          int freq = map(potVal, 0, 4095, BUZZER_FREQ_MIN, BUZZER_FREQ_MAX);
          tone(PIN_BUZZER, freq);
        }

        vTaskDelay(pdMS_TO_TICKS(300));
        noTone(PIN_BUZZER);
        vTaskDelay(pdMS_TO_TICKS(700));

        // decrement shared strikeRemaining safely
        if (xSemaphoreTake(dataMutex, (TickType_t) 10) == pdTRUE) {
          if (strikeRemaining > 0) strikeRemaining--;
          strikes = strikeRemaining;
          xSemaphoreGive(dataMutex);
        } else {
          // if cannot get mutex, decrement local and continue
          strikes--;
        }
      }

      // after strikes, handle continuous alarm until stopped
      bool stillRinging = false;
      if (xSemaphoreTake(dataMutex, (TickType_t) 50) == pdTRUE) {
        stillRinging = alarmRinging;
        xSemaphoreGive(dataMutex);
      }

      while (stillRinging) {
        int potVal = analogRead(PIN_POT);

        if (potVal < POT_MIN_THRESHOLD) {
          noTone(PIN_BUZZER);
        } else {
          int freq = map(potVal, 0, 4095, BUZZER_FREQ_MIN, BUZZER_FREQ_MAX);
          tone(PIN_BUZZER, freq);
        }

        vTaskDelay(pdMS_TO_TICKS(250));
        noTone(PIN_BUZZER);
        vTaskDelay(pdMS_TO_TICKS(200));

        if (xSemaphoreTake(dataMutex, (TickType_t) 20) == pdTRUE) {
          stillRinging = alarmRinging;
          xSemaphoreGive(dataMutex);
        } else {
          vTaskDelay(pdMS_TO_TICKS(50));
        }
      }

      noTone(PIN_BUZZER);
    }
  }
}

// ---------- LEDTask (Core 1) ----------
void ledTask(void *pvParameters) {
  (void) pvParameters;
  for (;;) {
    int h, m, s;
    copyTime(h, m, s);
    bool isNight = (h >= 17 || h < 6);
    digitalWrite(PIN_LED, isNight ? HIGH : LOW);
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// ---------- DisplayTask (Core 1) ----------
void displayTask(void *pvParameters) {
  (void) pvParameters;
  char buf[48];

  for (;;) {
    int h, m, s;
    copyTime(h, m, s);

    bool isAlarm = false;
    int strikes = 0;
    int potVal = analogRead(PIN_POT);
    int potPct = map(potVal, 0, 4095, 0, 100);

    if (xSemaphoreTake(dataMutex, (TickType_t) 20) == pdTRUE) {
      isAlarm = alarmRinging;
      strikes = strikeRemaining;
      xSemaphoreGive(dataMutex);
    }

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);

    // Large time HH:MM (size 2) positioned to avoid clipping
    display.setTextSize(2);
    display.setCursor(6, 6);
    snprintf(buf, sizeof(buf), "%02d:%02d", h, m);
    display.println(buf);

    // seconds smaller at top-right
    display.setTextSize(1);
    display.setCursor(96, 14);
    snprintf(buf, sizeof(buf), ":%02d", s);
    display.println(buf);

    // alarm & pot & strikes in info area
    display.setTextSize(1);
    display.setCursor(0, 42);
    snprintf(buf, sizeof(buf), "Alarm: %02d:%02d  STR:%d", alarmHour, alarmMinute, strikes);
    display.println(buf);

    display.setCursor(0, 52);
    snprintf(buf, sizeof(buf), "A:%s  POT:%d%%", (isAlarm ? "ON " : "OFF"), potPct);
    display.println(buf);

    display.display();
    vTaskDelay(pdMS_TO_TICKS(180));
  }
}

// ---------- ButtonTask (Core 1) ----------
void buttonTask(void *pvParameters) {
  (void) pvParameters;
  const TickType_t deb = pdMS_TO_TICKS(200);
  TickType_t lastPress = 0;

  for (;;) {
    if (digitalRead(PIN_BTN_STOP) == LOW) {
      TickType_t now = xTaskGetTickCount();
      if ((now - lastPress) > deb) {
        // clear alarm & strikes
        if (xSemaphoreTake(dataMutex, (TickType_t) 50) == pdTRUE) {
          alarmRinging = false;
          strikeRemaining = 0;
          xSemaphoreGive(dataMutex);
        }
        lastPress = now;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(30));
  }
}

// ---------- loop (unused) ----------
void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}