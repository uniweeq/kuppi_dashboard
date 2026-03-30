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

---

## Repository Structure

```
kuppi_dashboard/
├── app.py                  # Flask backend
├── requirements.txt        # Python dependencies
├── .env.example            # Environment variable template
├── templates/
│   └── dashboard.html      # Supervisor dashboard (single-page)
└── supabase/
    └── schema.sql          # Database schema & real-time setup
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
DOOR_ROOM=101                   # Room number for this door unit
```

See [Environment Variable Guide](#environment-variable-guide) for details.

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
  "card_uid": "ABC123",
  "tag_uid":  "TAG-101-BED",
  "area":     "Bed",
  "room":     "101"
}
```

### `POST /session/open`
Opens a new cleaning session (called automatically by the door RFID listener, or manually).

**Body (JSON):**
```json
{
  "card_uid": "ABC123",
  "room":     "101"
}
```

### `POST /session/close`
Closes a session.  Marks it `complete` if all 6 zones were scanned, otherwise `incomplete`.

**Body (JSON):**
```json
{
  "card_uid": "ABC123",
  "room":     "101"
}
```

### `GET /api/status`
Returns JSON array of all rooms with current status.

**Response:**
```json
[
  {
    "room":       "101",
    "status":     "active",
    "zones_done": 3,
    "scanned":    ["Bed", "Toilet", "Wardrobe"],
    "missing":    ["Curtain", "Drinks Bar", "Study Desk"],
    "start_time": "2024-01-01T10:00:00+00:00"
  }
]
```

---

## How to Connect the KUPPI Card

1. **KUPPI card** sends HTTP POST requests to `http://<server-ip>:<port>/scan` whenever an NFC tag is scanned.
2. Configure the KUPPI firmware with the server IP address and port.
3. The card must send JSON with fields: `card_uid`, `tag_uid`, `area`, `room`.

---

## How the Door Unit Works

A USB RFID reader is plugged into the computer running `app.py`.  When a staff member taps their card at the door:

- **Windows** — the `keyboard` library captures the HID key sequence emitted by the USB reader and assembles the UID from keystrokes terminated by Enter.
- **Linux / Mac** — the `evdev` library reads raw key events from the device specified by `RFID_DEVICE`.

On tap the background thread calls the open-session logic directly, inserting a new `active` session for the room number set in `DOOR_ROOM`.

To manage multiple rooms from a single server, run one Flask instance per door (each with its own `DOOR_ROOM` and `FLASK_PORT`), or extend `app.py` with a room-selection lookup based on UID.

---

## Environment Variable Guide

| Variable | Required | Description |
|----------|----------|-------------|
| `SUPABASE_URL` | ✅ | Your Supabase project URL, e.g. `https://abc123.supabase.co` |
| `SUPABASE_KEY` | ✅ | Supabase `anon` public key (found in Project Settings → API) |
| `FLASK_ENV` | — | `development` enables debug mode; default `production` |
| `FLASK_PORT` | — | Port Flask listens on; default `5000` |
| `RFID_DEVICE` | — | evdev device path for USB RFID reader on Linux/Mac; default `/dev/input/event2` |
| `DOOR_ROOM` | — | Room number associated with the door RFID reader on this machine; default `unknown` |

---

## License

MIT — see [LICENSE](LICENSE).

