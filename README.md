# KUPPI Dashboard

**KUPPI** is a hotel housekeeping quality assurance system.  
Staff carry a body-worn NFC card device and scan hidden NFC tags in six zones of each hotel room.  
A supervisor monitors live cleaning progress through this web dashboard.

---

## System Overview

```
[KUPPI card] --scan--> [Flask /scan]  ---> [Supabase DB]
[USB RFID door reader] -----------> [Flask background thread]
                                         |
                              [Supabase real-time]
                                         |
                              [Dashboard HTML page] <-- supervisor
```

### Room Zones (6 per room)
`Toilet` · `Wardrobe` · `Study Desk` · `Bed` · `Curtain` · `Drinks Bar`

---

## Tech Stack

| Layer | Technology |
|-------|-----------|
| Backend | Python · Flask · Flask-CORS |
| Database | Supabase (PostgreSQL) via `supabase-py` |
| Real-time | Supabase Realtime (Postgres changes) |
| Frontend | HTML · CSS · Vanilla JavaScript |
| Door reader | `keyboard` library (Windows) · `evdev` (Linux/Mac) |
| Card firmware | ESP32 · Arduino · TFT_eSPI · Adafruit PN532 · ArduinoJson |

---

## Repository Structure

```
kuppi_dashboard/
├── app.py                  # Flask backend
├── requirements.txt        # Python dependencies
├── .env.example            # Environment variable template
├── templates/
│   └── dashboard.html      # Supervisor dashboard (single-page)
├── supabase/
│   └── schema.sql          # Database schema & real-time setup
└── kuppi_v14_fixed/
    └── kuppi_v14_fixed.ino # ESP32 Arduino firmware for the KUPPI card device
```

---

## Setup Instructions

### 1. Clone the repository

```bash
git clone https://github.com/uniweeq/kuppi_dashboard.git
cd kuppi_dashboard
```

### 2. Create a Python virtual environment and install dependencies

```bash
python -m venv .venv
# Windows
.venv\Scripts\activate
# Linux / Mac
source .venv/bin/activate

pip install -r requirements.txt
```

> **Note:** The `keyboard` library requires administrator / root privileges on most operating systems.

### 3. Set up environment variables

```bash
cp .env.example .env
```

Edit `.env` and fill in your real values:

```
SUPABASE_URL=https://<project-ref>.supabase.co
SUPABASE_KEY=<your-anon-key>
FLASK_ENV=development
FLASK_PORT=5000
RFID_DEVICE=/dev/input/event2   # Linux/Mac only
DOOR_ROOM=301                   # Room number for this door unit
```

> **Note:** `app.py` uses `find_dotenv()` to locate the `.env` file, so it will be found even if you run the script from a subdirectory.  If `SUPABASE_URL` or `SUPABASE_KEY` are missing the app will print a clear error message and exit immediately.

### 4. Set up Supabase tables

1. Open your [Supabase project](https://app.supabase.com).
2. Go to **SQL Editor** and open a new query.
3. Paste the contents of `supabase/schema.sql` and click **Run**.

This creates:
- `sessions` table — one row per cleaning session
- `scans` table — one row per NFC tag scan
- `rooms` table — master list of rooms and their NFC tags
- `room_status` view — per-room summary joining sessions and scans
- Real-time publication enabled on `sessions` and `scans`

After creating the schema, add your rooms to the `rooms` table:

```sql
INSERT INTO rooms (room_number, tag_uid, area_name) VALUES
  ('101', 'TAG-101-TOI', 'Toilet'),
  ('101', 'TAG-101-WAR', 'Wardrobe'),
  ('101', 'TAG-101-STU', 'Study Desk'),
  ('101', 'TAG-101-BED', 'Bed'),
  ('101', 'TAG-101-CUR', 'Curtain'),
  ('101', 'TAG-101-DRK', 'Drinks Bar');
-- Repeat for each room
```

### 5. Run the Flask app

```bash
# On Windows (requires administrator prompt for keyboard library)
python app.py

# On Linux / Mac
sudo python app.py   # sudo needed for evdev / keyboard
```

The server starts on `http://0.0.0.0:5000` by default.

---

## Accessing the Dashboard

Open a browser and navigate to:

```
http://localhost:5000/dashboard
```

or just:

```
http://localhost:5000/
```

The dashboard shows a live grid of all rooms with their current cleaning status, progress bars, and zone checklists.  Tiles update in real time via Supabase Realtime subscriptions — no page refresh needed.

---

## API Reference

### `POST /scan`
Receives a scan event from a KUPPI card.

**Body (JSON):**
```json
{
  "card_uid": "KUPPI-001",
  "tag_uid":  "BC590C4E",
  "area":     "Bed",
  "room":     "301"
}
```

**Notes:**
- `area` must be one of the six known zone names (`Toilet`, `Wardrobe`, `Study Desk`, `Bed`, `Curtain`, `Drinks Bar`); otherwise returns **400**.
- Returns **400** if no active session exists for the card/room combination.
- Returns **200** with `"status": "already_scanned"` if the zone was already recorded in the current session (idempotent).
- Returns **201** with the inserted scan object on success.

### `POST /session/open`
Opens a new cleaning session (called automatically by the door RFID listener on the first card tap, or manually).  Any previously active session for the same room is automatically closed as `incomplete` before the new session is created.

**Body (JSON):**
```json
{
  "card_uid": "KUPPI-001",
  "room":     "301"
}
```

### `POST /session/close`
Closes a session.  Marks it `complete` if all 6 zones were scanned, otherwise `incomplete`.

**Body (JSON):**
```json
{
  "card_uid": "KUPPI-001",
  "room":     "301"
}
```

### `GET /api/status`
Returns JSON array of all rooms with current status.

**Response:**
```json
[
  {
    "room":        "301",
    "status":      "active",
    "zones_done":  3,
    "zones_total": 6,
    "scanned":     ["Bed", "Toilet", "Wardrobe"],
    "missing":     ["Curtain", "Drinks Bar", "Study Desk"],
    "start_time":  "2024-01-01T10:00:00+00:00"
  }
]
```

---

## How to Connect the KUPPI Card

1. **KUPPI card** sends HTTP POST requests to `http://<server-ip>:<port>/scan` whenever an NFC tag is scanned.
2. Configure the KUPPI firmware with the server IP address and port.
3. The card must send JSON with fields: `card_uid`, `tag_uid`, `area`, `room`.

---

## KUPPI Card Firmware

The `kuppi_v14_fixed/kuppi_v14_fixed.ino` sketch runs on an **ESP32** with a TFT display, a PN532 NFC reader (I2C), and a buzzer.  It provides a touchscreen checklist UI, reads NFC tags in the six room zones, and sends HTTP requests to the Flask backend.

### Hardware Required

- ESP32 development board
- TFT display (configured via `TFT_eSPI` `User_Setup.h`)
- PN532 NFC module (I2C, address `0x48`)
- CST816 capacitive touch controller (I2C, address `0x38`)
- Passive buzzer on GPIO 25

### Arduino Libraries

Install the following libraries via the Arduino Library Manager or PlatformIO:

| Library | Purpose |
|---------|---------|
| `TFT_eSPI` | TFT display driver |
| `Adafruit PN532` | NFC reader |
| `ArduinoJson` | JSON serialisation |
| `WiFi` | Built-in ESP32 Wi-Fi |
| `HTTPClient` | Built-in ESP32 HTTP client |

### Firmware Configuration

Open `kuppi_v14_fixed/kuppi_v14_fixed.ino` and edit the constants near the top of the file:

```cpp
const char* WIFI_SSID     = "YourNetwork";
const char* WIFI_PASSWORD = "YourPassword";
const char* SERVER_IP     = "192.168.1.100";   // IP of the machine running app.py
const int   SERVER_PORT   = 5000;
const char* CARD_UID      = "KUPPI-001";        // Unique ID for this card
const char* ROOM_NUMBER   = "301";              // Room this card is assigned to
```

Also update `zoneUIDs` with the actual UID bytes read from your NFC tags for the six zones.

### Flashing

1. Open the sketch in the **Arduino IDE** (2.x recommended) or PlatformIO.
2. Select **ESP32 Dev Module** (or your specific board) as the target.
3. Configure `TFT_eSPI` for your display by editing its `User_Setup.h`.
4. Click **Upload**.

### Runtime Behaviour

- On boot the device connects to Wi-Fi and calls `POST /session/open`.
- Staff tap each NFC zone tag; the device calls `POST /scan` and checks off the zone on the touchscreen.
- A 25-minute countdown timer is displayed.  When all six zones are complete the device calls `POST /session/close` and shows a completion screen.

---

## How the Door Unit Works

A USB RFID reader is plugged into the computer running `app.py`.  The background thread implements a **tap-to-toggle** model:

- **First tap** — opens a new `active` session for the room.  Any previously stale active session for the same room is closed as `incomplete` first.
- **Second tap** — closes the session.  Status is set to `complete` if all six zones were scanned, otherwise `incomplete`.

On **Windows** the `keyboard` library captures the HID key sequence emitted by the USB reader and assembles the UID from keystrokes terminated by Enter.  On **Linux / Mac** the `evdev` library reads raw key events from the device specified by `RFID_DEVICE`.

---

## Environment Variable Guide

| Variable | Required | Description |
|----------|----------|-------------|
| `SUPABASE_URL` | ✅ | Your Supabase project URL, e.g. `https://abc123.supabase.co` |
| `SUPABASE_KEY` | ✅ | Supabase `anon` public key (found in Project Settings → API) |
| `FLASK_ENV` | — | `development` enables debug mode; default `production` |
| `FLASK_PORT` | — | Port Flask listens on; default `5000` |
| `RFID_DEVICE` | — | evdev device path for USB RFID reader on Linux/Mac; default `/dev/input/event2` |
| `DOOR_ROOM` | — | Room number associated with the door RFID reader on this machine; default `301` |

---

## License

MIT — see [LICENSE](LICENSE).

