# E-reader

# Pink Propeller eReader

A minimal e-reader built with an ESP32-C3 and a WeAct 2.9" ePaper display. Upload `.txt` books over WiFi and read them with a single button.

---

## What you need

| Part | Details |
|---|---|
| Microcontroller | ESP32-C3 Supermini |
| Display | WeAct 2.9" ePaper (296×128, 8-pin) |
| Button | 6×6mm tactile push button |
| Battery | LiPo with charge board (optional) |

---

## Wiring

| ePaper Pin | ESP32-C3 |
|---|---|
| 1 BUSY | GPIO 21 |
| 2 RES | GPIO 6 |
| 3 D/C | GPIO 7 |
| 4 CS | GPIO 10 |
| 5 SCL | GPIO 20 |
| 6 SDA | GPIO 5 |
| 7 GND | GND |
| 8 VCC | 3.3V ⚠️ not 5V |

Button: one pin to **GPIO 3**, other pin to **GND** (use diagonal corners of the button).

---

## Arduino IDE setup

1. Install board: **ESP32 by Espressif** via Boards Manager
2. Select board: `ESP32C3 Dev Module`
3. Tools → USB CDC On Boot: `Enabled`
4. Tools → Partition Scheme: `Default 4MB with spiffs`
5. Install libraries via Library Manager:
   - `GxEPD2` by ZinggJM
   - `Adafruit GFX Library` by Adafruit

---

## How to use

**One button does everything:**

| Press | Action |
|---|---|
| Short press | Move selection down |
| Long press (hold ~1s) | Confirm / go back |

**To upload a book:**
1. From the main menu, select **Download Books**
2. On your phone or laptop, connect to WiFi: `Pink-Propeller EB` / password `readbooks`
3. Open your browser and go to the URL displayed on the e-ink screen
4. Upload any `.txt` file
5. Long press to go back when done — WiFi turns off automatically

**To delete a book:**
1. Go to **View Booklist**
2. Short press to select a book
3. Long press → select **Delete book** → long press to confirm

---

## Notes

- Books must be plain `.txt` files
- The device saves your last page per book — this survives power cycles
- After 10 minutes of no activity the screen goes to the splash/hibernate screen
- Press the button to wake it back up
