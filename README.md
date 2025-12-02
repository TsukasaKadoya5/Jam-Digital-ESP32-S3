# Jam Digital ESP32-S3

Proyek ini merupakan implementasi jam digital berbasis **ESP32-S3** dengan pemanfaatan **FreeRTOS**, **multicore processing**, serta **sinkronisasi menggunakan Mutex & Semaphore**. Sistem menampilkan waktu pada OLED, menyediakan alarm dengan lonceng terjadwal, dan mendukung kontrol melalui potensiometer serta rotary encoder.

---

## ✨ Fitur Utama

* **Pemrosesan Multicore**

  * *Core 0:* TimeTask – membaca waktu menggunakan `esp_timer_get_time()`
  * *Core 1:* Task untuk display, buzzer, LED, button, dan potensiometer
* **Alarm dengan Semaphore**

  * Semaphore `alarmSem` digunakan untuk memberi sinyal dari TimeTask ke BuzzerTask ketika waktu alarm tercapai.
* **Proteksi Data dengan Mutex**

  * Mutex `dataMutex` melindungi variabel *alarmRinging* dan *strikeRemaining* agar tidak race condition.
* **OLED SSD1306**

  * Menampilkan waktu dengan layout stabil dan tidak terpotong.
* **Rotary Encoder (Interrupt)**

  * Mengubah menit secara real-time melalui ISR.
* **Potensiometer**

  * Mengatur *volume* buzzer (`< threshold = mute`, maks = paling keras).
* **Button**

  * Menonaktifkan alarm saat ditekan.

---

## 🔧 Library yang Digunakan

* **Adafruit GFX**
* **Adafruit SSD1306**
* **Wire**
* **Arduino FreeRTOS (built-in ESP32)**
* `esp_timer.h`

Pastikan seluruh library tersedia pada IDE Arduino / PlatformIO sebelum kompilasi.

---

## 🔌 Hardware yang Digunakan

* ESP32-S3
* OLED SSD1306 (I2C)
* Buzzer
* Potensiometer
* Rotary Encoder
* Push Button
* LED indikator

Wiring detail terdapat pada file **diagram.json** (Wokwi).

---

## 🔄 Arsitektur Task (Ringkas)

| Task            | Core      | Fungsi                                                     |
| --------------- | --------- | ---------------------------------------------------------- |
| **TimeTask**    | Core 0    | Mengupdate waktu dan memicu alarm (semaphore)              |
| **DisplayTask** | Core 1    | Menampilkan jam ke OLED                                    |
| **BuzzerTask**  | Core 1    | Mengatur suara alarm berdasarkan semaphore & potensiometer |
| **ButtonTask**  | Core 1    | Membaca tombol untuk mematikan alarm                       |
| **Encoder ISR** | Interrupt | Mengubah menit secara langsung                             |

---

## ▶️ Cara Menjalankan

1. Upload *sketch.ino* ke ESP32-S3 menggunakan Arduino IDE / PlatformIO.
2. Pastikan wiring sesuai diagram Wokwi.
3. Setelah sistem berjalan:

   * Atur menit menggunakan **rotary encoder**
   * Alarm akan berbunyi saat waktu tercapai
   * Tekan **tombol** untuk menghentikan alarm
   * Putar **potensiometer** untuk mengatur volume buzzer

---

## 📝 Catatan

* Variabel penting untuk sinkronisasi:

  * `alarmRinging`
  * `strikeRemaining`
  * `alarmSem` (semaphore)
  * `dataMutex` (mutex)
* Task loop bawaan `loop()` tidak digunakan karena seluruh sistem berjalan di FreeRTOS.
