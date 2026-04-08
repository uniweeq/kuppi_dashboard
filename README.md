# KUPPI Dashboard

**KUPPI** is a hotel housekeeping quality assurance system.  
Staff carry a body-worn NFC card device and scan hidden NFC tags in six zones of each hotel room.  
A supervisor monitors live cleaning progress through a real-time web dashboard.

---

## System Overview

```
                        ┌───────────────────────────┐
                        │    Supabase (PostgreSQL)   │
                        └────▲──────────────▲───────┘
                             │              │
          POST /scan         │              │   INSERT / UPDATE
   ┌─────────────┐       ┌───┴──────────────┴───┐
   │  KUPPI Card  │──────►│     Flask Backend     │
   │  (ESP32 NFC) │       │       app.py          │
   └─────────────┘       └───┬──────────────┬───┘
                             │              │
                         SSE /events    GET /api/*
                             │              │
                        ┌────▼──────────────▼───────┐
                        │   Dashboard (HTML/JS)      │
                        │   Supervisor browser        │
                        └───────────────────────────┘
                                    ▲
   ┌──────────────────┐             │
   │ USB RFID Reader  │─────► background thread
   │ (door unit)      │       (keyboard / evdev)
   └──────────────────┘
```

### Room Zones (6 per room)

`Toilet` · `Wardrobe` · `Study Desk` · `Bed` · `Curtain` · `Drinks Bar`

### Session Status Flow

```
not_cleaned ──► cleaning ──► awaiting_approval ──► ready
```

- **not_cleaned** — display-only; no session exists for the room
- **cleaning** — active session, staff scanning zone tags
- **awaiting_approval** — session closed, waiting for supervisor approval
- **ready** — supervisor approved, room is ready for guests

---

## Tech Stack

| Layer | Technology |
|-------|-----------|
| Backend | Python · Flask · Flask-CORS |
| Database | Supabase (PostgreSQL) via `supabase-py` |
| Real-time | Server-Sent Events (SSE) via `/events` endpoint |
| Frontend | HTML · CSS · Vanilla JavaScript (Jinja2 templates) |
| Door reader | `keyboard` library (Windows) · `evdev` (Linux/Mac) |
| Card firmware | ESP32 · Arduino · TFT_eSPI · Adafruit PN532 · ArduinoJson |

---

## Repository Structure

```
kuppi_dashboard/
├── app.py                      # Flask backend (API + SSE + RFID listener)
├── requirements.txt            # Python dependencies
├── .gitignore
├── LICENSE                     # MIT license
├── templates/
│   ├── base.html               # Shared layout template (navigation, styles)
│   ├── dashboard.html          # Supervisor dashboard — live room grid
│   ├── rooms.html              # Room management page
│   ├── staff.html              # Staff management page
│   └── settings.html           # System settings page
├── supabase/
│   └── schema.sql              # Database schema, views, triggers & seed data
└── kuppi_v14_fixed/
    └── kuppi_v14_fixed.ino     # ESP32 Arduino firmware for the KUPPI card
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

Create a `.env` file in the project root:

```
SUPABASE_URL=https://<project-ref>.supabase.co
SUPABASE_KEY=<your-anon-key>
FLASK_ENV=development
FLASK_PORT=5000
RFID_DEVICE=/dev/input/event2   # Linux/Mac only
DOOR_ROOM=301                   # Room number for this door unit
```

> **Note:** `app.py` uses `find_dotenv()` to locate the `.env` file, so it will be found even if you run the script from a subdirectory. If `SUPABASE_URL` or `SUPABASE_KEY` are missing the app prints a clear error and exits immediately.

### 4. Set up Supabase tables

1. Open your [Supabase project](https://app.supabase.com).
2. Go to **SQL Editor** and open a new query.
3. Paste the contents of `supabase/schema.sql` and click **Run**.

This creates:

| Table / View | Description |
|---|---|
| `staff` | Housekeeping staff members with personal NFC card UIDs |
| `sessions` | One row per cleaning session with status tracking |
| `scans` | One row per NFC zone tag scan within a session |
| `zone_tags` | Master list of NFC tags for room zones (6 per room) |
| `rooms` | Room registry with status, door NFC tag, floor, and room type |
| `unknown_scans` | Log of unrecognised NFC tag scans for resolution |
| `room_status` (view) | Per-room summary joining sessions and scans |

Real-time publication is enabled on `sessions`, `scans`, `rooms`, `unknown_scans`, and `staff`.

After creating the schema, seed your rooms and zone tags:

```sql
-- Add rooms with their door NFC tags
INSERT INTO rooms (room_number, status, nfc_uid, floor, room_type) VALUES
  ('101', 'available', 'DOOR-101', '1', 'Standard'),
  ('102', 'available', 'DOOR-102', '1', 'Standard'),
  ('201', 'available', 'DOOR-201', '2', 'Deluxe'),
  ('301', 'available', 'DOOR-301', '3', 'Suite');

-- Add staff members
INSERT INTO staff (name, card_uid, staff_code) VALUES
  ('Maria Santos',  'A1B2C3D4', 'EMP-001'),
  ('James Lee',     'E5F6A7B8', 'EMP-002');

-- Add zone NFC tags for each room
INSERT INTO zone_tags (room_number, tag_uid, area_name) VALUES
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

## Dashboard Pages

| Route | Page | Description |
|-------|------|-------------|
| `/` or `/dashboard` | Dashboard | Live room grid with real-time status, progress, and zone checklists |
| `/rooms` | Room Management | Add, edit, and delete rooms; configure door NFC tags |
| `/staff` | Staff Management | Add, edit, and delete staff; assign NFC cards |
| `/settings` | Settings | System configuration |

All pages share a common navigation layout via `base.html`. The dashboard updates in real time via **Server-Sent Events (SSE)** — no page refresh needed.

---

## Real-Time Updates (SSE)

The dashboard connects to `GET /events` — an SSE endpoint that pushes live updates whenever sessions or scans change. This replaces the older Supabase Realtime subscription approach for more reliable, low-latency updates.

**Event types pushed:**

| Event | Trigger |
|-------|---------|
| `session_open` | A new cleaning session is started |
| `session_close` | A session is closed (cleaning complete or incomplete) |
| `scan` | A zone NFC tag is scanned |
| `door_tap` | A door NFC tag is scanned via `/scan` with area "DOOR" |

The SSE stream sends keepalive comments every 25 seconds to prevent connection timeouts.

---

## API Reference

### Session & Scan Endpoints

#### `POST /scan`

Receives a scan event from a KUPPI card device.

**Body (JSON):**
```json
{
  "card_uid": "KUPPI-001",
  "tag_uid":  "BC590C4E",
  "area":     "Bed",
  "room":     "301"
}
```

**Behaviour:**
- If `area` is `"DOOR"` — looks up `tag_uid` in the `rooms` table. If found, triggers a door-tap toggle (open/close session). If not found, logs to `unknown_scans` and returns **400**.
- If `area` is a zone name — validates against the six known zones. Returns **400** if the zone is unknown or no active session exists. Returns **200** with `"already_scanned"` for duplicate scans within the same session. Returns **201** on successful scan insertion.

#### `POST /session/open`

Opens a new cleaning session. Any previously active session for the same room is automatically closed as `awaiting_approval`.

**Body (JSON):**
```json
{
  "card_uid": "KUPPI-001",
  "room":     "301",
  "staff_id": "uuid-or-card-uid"
}
```

> `staff_id` is optional. Accepts either a UUID (direct FK) or a card UID string (resolved via staff table lookup).

#### `POST /session/close`

Closes an active session. Always sets status to `awaiting_approval`. Calculates `duration_mins` from start to end.

**Body (JSON):**
```json
{
  "card_uid": "KUPPI-001",
  "room":     "301"
}
```

**Response:**
```json
{
  "status":        "ok",
  "result":        "awaiting_approval",
  "duration_mins": 21,
  "missing":       ["Curtain", "Drinks Bar"],
  "scanned":       ["Bed", "Study Desk", "Toilet", "Wardrobe"]
}
```

#### `GET /api/status`

Returns JSON array of all rooms with current cleaning status, including staff name resolution.

**Response:**
```json
[
  {
    "room":        "301",
    "status":      "cleaning",
    "zones_done":  3,
    "zones_total": 6,
    "scanned":     ["Bed", "Toilet", "Wardrobe"],
    "missing":     ["Curtain", "Drinks Bar", "Study Desk"],
    "start_time":  "2025-01-01T10:00:00+00:00",
    "end_time":    null,
    "staff_name":  "Maria Santos"
  }
]
```

#### `GET /api/recent-sessions`

Returns the most recent `ready` session for each room, keyed by room number. Includes staff name and calculated duration.

---

### Room Management Endpoints

#### `GET /api/rooms`
List all rooms with metadata, ordered by room number.

#### `POST /api/rooms`
Create a new room.

**Body (JSON):**
```json
{
  "room_number": "301",
  "status":      "available",
  "reason":      "Under renovation",
  "nfc_uid":     "DOOR-301",
  "floor":       "3",
  "room_type":   "Suite"
}
```

Valid statuses: `available` | `blocked` | `maintenance` | `inactive`

#### `PUT /api/rooms/<room_id>`
Update an existing room (all fields optional).

#### `DELETE /api/rooms/<room_id>`
Delete a room.

#### `GET /api/room-lookup/<nfc_uid>`
Look up a room by its door NFC tag UID. Used by the KUPPI device when staff scans a room's door tag to identify the room dynamically.

**Response (200):**
```json
{
  "room_number": "301",
  "floor":       "3",
  "room_type":   "Suite",
  "status":      "available"
}
```

---

### Staff Management Endpoints

#### `GET /api/staff`
List all staff members, ordered by name.

#### `POST /api/staff`
Create a new staff member.

**Body (JSON):**
```json
{
  "name":       "Maria Santos",
  "card_uid":   "A1B2C3D4",
  "staff_code": "EMP-001"
}
```

#### `PUT /api/staff/<staff_id>`
Update a staff member's details (all fields optional).

#### `DELETE /api/staff/<staff_id>`
Delete a staff member.

#### `GET /api/staff-lookup/<card_uid>`
Look up a staff member by their personal NFC card UID. Used by the KUPPI device for staff login.

**Response (200):**
```json
{
  "id":         "uuid",
  "name":       "Maria Santos",
  "staff_code": "EMP-001"
}
```

#### `POST /api/set-staff-language`
Set the preferred language for a staff member's KUPPI device.

**Body (JSON):**
```json
{
  "staff_id":      "EMP-001",
  "language_code": "en",
  "language_name": "English"
}
```

Supported languages: `en`, `zh`, `ta`, `bn`, `my`, `th`, `vi`, `tl`

---

### Unknown Scans Endpoints

#### `GET /api/unknown_scans`
List unknown NFC tag scans. Filter with `?resolved=true` or `?resolved=false` (default).

#### `POST /api/unknown_scans/<scan_id>/resolve`
Mark an unknown scan as resolved, optionally assigning it to a room.

**Body (JSON):**
```json
{
  "assigned_room": "301"
}
```

---

### Development Endpoints

#### `POST /api/populate-test-data`
Creates fake test data covering all session statuses (`not_cleaned`, `cleaning`, `awaiting_approval`, `ready`) for dashboard visualisation during development.

---

## How to Connect the KUPPI Card

1. The **KUPPI card** sends HTTP POST requests to `http://<server-ip>:<port>/scan` whenever an NFC tag is scanned.
2. Configure the firmware with the server IP, port, and card UID constants.
3. The card must send JSON with fields: `card_uid`, `tag_uid`, `area`, `room`.

### Device Boot Flow

1. **Staff Login** — Staff taps their personal NFC card → device calls `GET /api/staff-lookup/<card_uid>` to identify them.
2. **Room Identification** — Staff scans the room's door NFC tag → device calls `GET /api/room-lookup/<nfc_uid>` to resolve the room number.
3. **Session Start** — Device calls `POST /session/open` (with `staff_id`), starts the 25-minute countdown timer.
4. **Zone Scanning** — Staff taps each of the 6 zone NFC tags → device calls `POST /scan` for each and checks off the zone on the touchscreen checklist.
5. **Session End** — When all 6 zones are complete (or timer expires), device calls `POST /session/close` and shows the completion screen.

---

## KUPPI Card Firmware

The `kuppi_v14_fixed/kuppi_v14_fixed.ino` sketch runs on an **ESP32** with a TFT display, a PN532 NFC reader (I2C), and a buzzer. It provides a touchscreen checklist UI, reads NFC tags in the six room zones, and sends HTTP requests to the Flask backend.

### Hardware Required

- ESP32 development board
- TFT display (configured via `TFT_eSPI` `User_Setup.h`)
- PN532 NFC module (I2C, address `0x48`)
- CST816 capacitive touch controller (I2C, address `0x38`)
- Passive buzzer on GPIO 25

### Arduino Libraries

| Library | Purpose |
|---------|---------|
| `TFT_eSPI` | TFT display driver |
| `Adafruit PN532` | NFC reader |
| `ArduinoJson` | JSON serialisation |
| `WiFi` | Built-in ESP32 Wi-Fi |
| `HTTPClient` | Built-in ESP32 HTTP client |

### Firmware Configuration

Open `kuppi_v14_fixed/kuppi_v14_fixed.ino` and edit the constants near the top:

```cpp
const char* WIFI_SSID     = "YourNetwork";
const char* WIFI_PASSWORD = "YourPassword";
const char* SERVER_IP     = "192.168.1.100";   // IP of the machine running app.py
const int   SERVER_PORT   = 5000;
const char* CARD_UID      = "KUPPI-001";        // Unique ID for this card
```

> **Note:** Room number is no longer hardcoded. The device dynamically identifies the room by scanning the door NFC tag and calling `/api/room-lookup/<nfc_uid>`.

Also update `zoneUIDs` with the actual UID bytes read from your NFC tags for the six zones.

### Flashing

1. Open the sketch in the **Arduino IDE** (2.x recommended) or PlatformIO.
2. Select **ESP32 Dev Module** (or your specific board) as the target.
3. Configure `TFT_eSPI` for your display by editing its `User_Setup.h`.
4. Click **Upload**.

### Device Screens

| Screen | Description |
|--------|-------------|
| Staff Login | Prompts staff to tap their personal NFC card for identification |
| Scan Room | Prompts staff to scan the room's door NFC tag |
| Home | Shows 6 zone circles (red = pending, green = done) |
| Checklist | Sub-item checklist for each zone with scrollable pill-style items |
| Complete | All zones scanned — session closing animation |

### Runtime Architecture

- HTTP requests run on **Core 0** in a dedicated FreeRTOS task (`httpTask`), keeping the UI responsive on Core 1.
- An I2C mutex (`i2cMutex`) prevents bus contention between NFC reads and touch controller reads.
- A 25-minute countdown timer is rendered as an animated border around the screen.
- Wi-Fi status is shown as a coloured dot in the header (green = OK, yellow = sending, red = failed).

---

## How the Door Unit Works

A USB RFID reader is plugged into the computer running `app.py`. The background thread implements a **tap-to-toggle** model:

- **First tap** — opens a new `cleaning` session for the room. Any previously stale active session is closed as `incomplete` first.
- **Second tap** — closes the session. Status is set to `awaiting_approval` if all six zones were scanned, otherwise `incomplete`.

On **Windows** the `keyboard` library captures the HID key sequence emitted by the USB reader and assembles the UID from keystrokes terminated by Enter. On **Linux / Mac** the `evdev` library reads raw key events from the device specified by `RFID_DEVICE`.

---

## Environment Variable Guide

| Variable | Required | Default | Description |
|----------|----------|---------|-------------|
| `SUPABASE_URL` | ✅ | — | Supabase project URL, e.g. `https://abc123.supabase.co` |
| `SUPABASE_KEY` | ✅ | — | Supabase `anon` public key (Project Settings → API) |
| `FLASK_ENV` | — | `production` | Set to `development` to enable debug mode |
| `FLASK_PORT` | — | `5000` | Port Flask listens on |
| `RFID_DEVICE` | — | `/dev/input/event2` | evdev device path for USB RFID reader (Linux/Mac) |
| `DOOR_ROOM` | — | `301` | Room number for the door RFID reader on this machine |

---

## License

MIT — see [LICENSE](LICENSE).
