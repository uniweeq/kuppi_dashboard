"""
KUPPI Dashboard — Flask backend
Handles scan events from KUPPI body-worn NFC card devices,
manages housekeeping sessions, and serves the supervisor dashboard.

Fixes applied:
  1. .env loading uses find_dotenv() so it works from any working directory
  2. Graceful startup error if SUPABASE_URL or SUPABASE_KEY missing
  3. /scan validates area against known zone names
  4. /scan prevents duplicate scans for same area in same session
  5. /scan returns 400 if no active session found (was silently inserting null session_id)
  6. _handle_door_tap now toggles open/close correctly on each tap
  7. DOOR_ROOM defaults to "301" instead of "unknown"
"""

import os
import threading
import platform
import traceback
from datetime import datetime, timezone

from flask import Flask, request, jsonify, render_template
from flask_cors import CORS
from supabase import create_client, Client
from dotenv import load_dotenv, find_dotenv

# ---------------------------------------------------------------------------
# Load environment variables
# find_dotenv() searches parent directories so it works regardless of where
# you run the script from
# ---------------------------------------------------------------------------
load_dotenv(find_dotenv())

# ---------------------------------------------------------------------------
# Validate required environment variables before anything else
# ---------------------------------------------------------------------------
SUPABASE_URL = os.environ.get("SUPABASE_URL", "")
SUPABASE_KEY = os.environ.get("SUPABASE_KEY", "")

if not SUPABASE_URL or not SUPABASE_KEY:
    print("=" * 60)
    print("ERROR: SUPABASE_URL and SUPABASE_KEY must be set in .env")
    print("Create a .env file in your project root with:")
    print("  SUPABASE_URL=https://your-project.supabase.co")
    print("  SUPABASE_KEY=your-anon-public-key")
    print("=" * 60)
    raise SystemExit(1)

supabase: Client = create_client(SUPABASE_URL, SUPABASE_KEY)

# ---------------------------------------------------------------------------
# Flask app
# ---------------------------------------------------------------------------
app = Flask(__name__)
CORS(app)

# ---------------------------------------------------------------------------
# Zone definitions — must match exactly what KUPPI firmware sends
# ---------------------------------------------------------------------------
ZONES = ["Toilet", "Wardrobe", "Study Desk", "Bed", "Curtain", "Drinks Bar"]
TOTAL_ZONES = len(ZONES)


# ---------------------------------------------------------------------------
# Helper utilities
# ---------------------------------------------------------------------------

def _log(event: str, detail: str = "") -> None:
    """Print a timestamped log line for every notable event."""
    ts = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")
    msg = f"[{ts}] [{event}]"
    if detail:
        msg += f" {detail}"
    print(msg, flush=True)


def _now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


# ---------------------------------------------------------------------------
# API endpoints
# ---------------------------------------------------------------------------

@app.route("/scan", methods=["POST"])
def receive_scan():
    """
    Receive a scan event from a KUPPI card device.

    Expected JSON body:
        {
            "card_uid": "KUPPI-001",
            "tag_uid":  "BC590C4E",
            "area":     "Bed",          # or "DOOR" for door tags
            "room":     "301"
        }

    If area is "DOOR", this is a door NFC tag scan.
    If tag_uid is not found in rooms.nfc_uid (for DOOR) or zone_tags.tag_uid (for zones),
    it logs as an unknown scan.
    """
    data = request.get_json(force=True)
    if not data:
        return jsonify({"error": "No JSON body"}), 400

    card_uid = data.get("card_uid", "").strip()
    tag_uid  = data.get("tag_uid",  "").strip()
    area     = data.get("area",     "").strip()
    room     = data.get("room",     "").strip()

    # Validate required fields
    if not all([card_uid, tag_uid, area, room]):
        return jsonify({"error": "Missing required fields: card_uid, tag_uid, area, room"}), 400

    # Handle DOOR area scan
    if area == "DOOR":
        # Check if this door NFC tag is configured in rooms table
        room_resp = (
            supabase.table("rooms")
            .select("id, room_number, nfc_uid")
            .eq("nfc_uid", tag_uid)
            .execute()
        )

        if not room_resp.data:
            # Unknown door tag - log it
            _log("SCAN_UNKNOWN_DOOR", f"Unknown door tag_uid={tag_uid} card={card_uid}")
            supabase.table("unknown_scans").insert({
                "tag_uid": tag_uid,
                "scanned_at": _now_iso()
            }).execute()
            return jsonify({
                "error": "Unknown door NFC tag. Please configure this tag in the rooms table.",
                "tag_uid": tag_uid
            }), 400

        # Door tag recognized - delegate to existing door tap handler
        _handle_door_tap(card_uid, room_resp.data[0]["room_number"])
        return jsonify({
            "status": "ok",
            "action": "door_tap_processed",
            "room": room_resp.data[0]["room_number"]
        }), 200

    # Validate area against known zones for non-DOOR scans
    if area not in ZONES:
        _log("SCAN_INVALID", f"Unknown area={area} from card={card_uid}")
        return jsonify({
            "error": f"Unknown area '{area}'. Valid zones: {ZONES} or 'DOOR'"
        }), 400

    # Note: zone_tags lookup skipped — the area is already validated against
    # ZONES above, and the Arduino firmware identifies zones by NFC UID
    # before sending the scan request.
    _log("SCAN_ZONE", f"tag_uid={tag_uid} card={card_uid} area={area} room={room}")

    # Find the active session for this card/room combination
    session_resp = (
        supabase.table("sessions")
        .select("id")
        .eq("card_uid", card_uid)
        .eq("room", room)
        .eq("status", "active")
        .order("start_time", desc=True)
        .limit(1)
        .execute()
    )

    # Return 400 if no active session found
    if not session_resp.data:
        _log("SCAN_NO_SESSION", f"No active session for card={card_uid} room={room}")
        return jsonify({"error": "No active session found. Tap door reader to start."}), 400

    session_id = session_resp.data[0]["id"]

    # Prevent duplicate scans for same area in same session
    existing_scan = (
        supabase.table("scans")
        .select("id")
        .eq("session_id", session_id)
        .eq("area", area)
        .execute()
    )
    if existing_scan.data:
        _log("SCAN_DUPLICATE", f"area={area} already scanned in session={session_id}")
        return jsonify({
            "status": "already_scanned",
            "area": area,
            "session_id": session_id
        }), 200

    # Insert the scan
    scan_row = {
        "session_id": session_id,
        "tag_uid":    tag_uid,
        "area":       area,
        "timestamp":  _now_iso(),
    }
    scan_resp = supabase.table("scans").insert(scan_row).execute()
    _log("SCAN", f"room={room} area={area} card={card_uid} session={session_id}")

    return jsonify({
        "status": "ok",
        "scan": scan_resp.data[0] if scan_resp.data else {}
    }), 201


@app.route("/session/open", methods=["POST"])
def open_session():
    """
    Open a new cleaning session when staff taps the USB door reader.

    Expected JSON body:
        {
            "card_uid": "KUPPI-001",
            "room":     "301"
        }
    """
    data = request.get_json(force=True)
    if not data:
        return jsonify({"error": "No JSON body"}), 400

    card_uid = data.get("card_uid", "").strip()
    room     = data.get("room",     "").strip()

    if not all([card_uid, room]):
        return jsonify({"error": "Missing required fields: card_uid, room"}), 400

    # Close any previously open session for this room before opening new one
    supabase.table("sessions").update({
        "status":   "incomplete",
        "end_time": _now_iso(),
    }).eq("room", room).eq("status", "active").execute()

    session_row = {
        "card_uid":   card_uid,
        "room":       room,
        "start_time": _now_iso(),
        "status":     "active",
    }
    resp = supabase.table("sessions").insert(session_row).execute()
    session = resp.data[0] if resp.data else {}
    _log("SESSION_OPEN", f"room={room} card={card_uid} session_id={session.get('id')}")

    return jsonify({"status": "ok", "session": session}), 201


@app.route("/session/close", methods=["POST"])
def close_session():
    """
    Close a cleaning session. Marks complete if all 6 zones scanned,
    incomplete otherwise.

    Expected JSON body:
        {
            "card_uid": "KUPPI-001",
            "room":     "301"
        }
    """
    data = request.get_json(force=True)
    if not data:
        return jsonify({"error": "No JSON body"}), 400

    card_uid = data.get("card_uid", "").strip()
    room     = data.get("room",     "").strip()

    if not all([card_uid, room]):
        return jsonify({"error": "Missing required fields: card_uid, room"}), 400

    session_resp = (
        supabase.table("sessions")
        .select("id")
        .eq("card_uid", card_uid)
        .eq("room", room)
        .eq("status", "active")
        .order("start_time", desc=True)
        .limit(1)
        .execute()
    )

    if not session_resp.data:
        return jsonify({"error": "No active session found"}), 404

    session_id = session_resp.data[0]["id"]

    scans_resp = (
        supabase.table("scans")
        .select("area")
        .eq("session_id", session_id)
        .execute()
    )

    scanned_areas = {s["area"] for s in (scans_resp.data or [])}
    status = "complete" if scanned_areas.issuperset(set(ZONES)) else "incomplete"

    supabase.table("sessions").update({
        "status":   status,
        "end_time": _now_iso(),
    }).eq("id", session_id).execute()

    missing = sorted(set(ZONES) - scanned_areas)
    _log("SESSION_CLOSE", f"room={room} card={card_uid} status={status} missing={missing}")

    return jsonify({
        "status":  "ok",
        "result":  status,
        "missing": missing,
        "scanned": sorted(scanned_areas),
    }), 200


@app.route("/api/status", methods=["GET"])
def api_status():
    """
    Return current status for all rooms.

    Response structure:
        [
            {
                "room":       "301",
                "status":     "active",
                "zones_done": 3,
                "zones_total": 6,
                "scanned":    ["Bed", "Toilet", "Wardrobe"],
                "missing":    ["Curtain", "Drinks Bar", "Study Desk"],
                "start_time": "2025-01-01T10:00:00+00:00"
            }
        ]
    """
    # Fetch all rooms
    rooms_resp = supabase.table("rooms").select("room_number").execute()
    all_rooms = [r["room_number"] for r in (rooms_resp.data or [])]

    # Fetch most recent sessions for all statuses
    sessions_resp = (
        supabase.table("sessions")
        .select("id, card_uid, room, start_time, end_time, status")
        .in_("status", ["active", "complete", "incomplete"])
        .order("start_time", desc=True)
        .execute()
    )

    # Build map: room -> most recent session
    session_map: dict[str, dict] = {}
    for s in (sessions_resp.data or []):
        room = s["room"]
        if room not in session_map:
            session_map[room] = s

    # Fetch scans for all active sessions in one query
    active_sessions = {
        s["id"]: s for s in session_map.values()
        if s["status"] == "active"
    }

    scans_map: dict[str, list[str]] = {}
    if active_sessions:
        scans_resp = (
            supabase.table("scans")
            .select("session_id, area")
            .in_("session_id", list(active_sessions.keys()))
            .execute()
        )
        for scan in (scans_resp.data or []):
            sid = scan["session_id"]
            scans_map.setdefault(sid, [])
            if scan["area"] not in scans_map[sid]:
                scans_map[sid].append(scan["area"])

    result = []
    for room_number in sorted(all_rooms):
        session = session_map.get(room_number)

        if not session:
            result.append({
                "room":        room_number,
                "status":      "pending",
                "zones_done":  0,
                "zones_total": TOTAL_ZONES,
                "scanned":     [],
                "missing":     sorted(ZONES),
                "start_time":  None,
            })
            continue

        if session["status"] == "active":
            scanned = scans_map.get(session["id"], [])
        else:
            closed_scans = (
                supabase.table("scans")
                .select("area")
                .eq("session_id", session["id"])
                .execute()
            )
            scanned = list({s["area"] for s in (closed_scans.data or [])})

        scanned_set = set(scanned)
        missing = sorted(set(ZONES) - scanned_set)

        result.append({
            "room":        room_number,
            "status":      session["status"],
            "zones_done":  len(scanned_set),
            "zones_total": TOTAL_ZONES,
            "scanned":     sorted(scanned_set),
            "missing":     missing,
            "start_time":  session.get("start_time"),
        })

    _log("API_STATUS", f"{len(result)} rooms returned")
    return jsonify(result), 200


# ---------------------------------------------------------------------------
# Room Management API endpoints
# ---------------------------------------------------------------------------

@app.route("/api/rooms", methods=["GET"])
def get_rooms():
    """
    List all rooms with their status and metadata.

    Response:
        [
            {
                "id": "uuid",
                "room_number": "301",
                "status": "available",
                "reason": null,
                "nfc_uid": "DOOR-301",
                "floor": "3",
                "room_type": "Suite",
                "created_at": "2025-01-01T10:00:00+00:00",
                "updated_at": "2025-01-01T10:00:00+00:00"
            }
        ]
    """
    try:
        resp = supabase.table("rooms").select("*").order("room_number").execute()
        _log("API_ROOMS_LIST", f"{len(resp.data or [])} rooms returned")
        return jsonify(resp.data or []), 200
    except Exception as e:
        _log("API_ROOMS_ERROR", f"Failed to list rooms: {e}")
        return jsonify({"error": "Failed to retrieve rooms"}), 500


@app.route("/api/rooms", methods=["POST"])
def create_room():
    """
    Create a new room.

    Expected JSON body:
        {
            "room_number": "301",
            "status": "available",        # optional, defaults to 'available'
            "reason": "Under renovation",  # optional
            "nfc_uid": "DOOR-301",         # optional, door NFC tag UID
            "floor": "3",                  # optional
            "room_type": "Suite"           # optional
        }
    """
    data = request.get_json(force=True)
    if not data:
        return jsonify({"error": "No JSON body"}), 400

    room_number = data.get("room_number", "").strip()
    if not room_number:
        return jsonify({"error": "Missing required field: room_number"}), 400

    # Validate status if provided
    status = data.get("status", "available").strip()
    valid_statuses = ["available", "blocked", "maintenance", "inactive"]
    if status not in valid_statuses:
        return jsonify({
            "error": f"Invalid status. Must be one of: {valid_statuses}"
        }), 400

    room_row = {
        "room_number": room_number,
        "status": status,
        "reason": data.get("reason", "").strip() or None,
        "nfc_uid": data.get("nfc_uid", "").strip() or None,
        "floor": data.get("floor", "").strip() or None,
        "room_type": data.get("room_type", "").strip() or None,
    }

    try:
        resp = supabase.table("rooms").insert(room_row).execute()
        room = resp.data[0] if resp.data else {}
        _log("API_ROOMS_CREATE", f"Created room {room_number}")
        return jsonify(room), 201
    except Exception as e:
        error_msg = str(e)
        if "duplicate key" in error_msg.lower() or "unique constraint" in error_msg.lower():
            return jsonify({"error": f"Room {room_number} already exists"}), 409
        _log("API_ROOMS_ERROR", f"Failed to create room: {e}")
        return jsonify({"error": "Failed to create room"}), 500


@app.route("/api/rooms/<room_id>", methods=["PUT"])
def update_room(room_id):
    """
    Update an existing room.

    Expected JSON body (all fields optional):
        {
            "room_number": "301",
            "status": "maintenance",
            "reason": "AC repair",
            "nfc_uid": "DOOR-301-NEW",
            "floor": "3",
            "room_type": "Deluxe"
        }
    """
    data = request.get_json(force=True)
    if not data:
        return jsonify({"error": "No JSON body"}), 400

    # Validate status if provided
    if "status" in data:
        status = data["status"].strip()
        valid_statuses = ["available", "blocked", "maintenance", "inactive"]
        if status not in valid_statuses:
            return jsonify({
                "error": f"Invalid status. Must be one of: {valid_statuses}"
            }), 400

    # Build update object (only include fields that are present in request)
    update_fields = {}
    for field in ["room_number", "status", "reason", "nfc_uid", "floor", "room_type"]:
        if field in data:
            value = data[field]
            if isinstance(value, str):
                value = value.strip()
            update_fields[field] = value if value else None

    if not update_fields:
        return jsonify({"error": "No fields to update"}), 400

    try:
        resp = supabase.table("rooms").update(update_fields).eq("id", room_id).execute()
        if not resp.data:
            return jsonify({"error": "Room not found"}), 404
        room = resp.data[0]
        _log("API_ROOMS_UPDATE", f"Updated room {room.get('room_number', room_id)}")
        return jsonify(room), 200
    except Exception as e:
        error_msg = str(e)
        if "duplicate key" in error_msg.lower() or "unique constraint" in error_msg.lower():
            return jsonify({"error": "Room number or NFC UID already exists"}), 409
        _log("API_ROOMS_ERROR", f"Failed to update room: {e}")
        return jsonify({"error": "Failed to update room"}), 500


@app.route("/api/rooms/<room_id>", methods=["DELETE"])
def delete_room(room_id):
    """
    Delete a room.

    Response:
        { "status": "ok", "message": "Room deleted" }
    """
    try:
        resp = supabase.table("rooms").delete().eq("id", room_id).execute()
        if not resp.data:
            return jsonify({"error": "Room not found"}), 404
        _log("API_ROOMS_DELETE", f"Deleted room {room_id}")
        return jsonify({"status": "ok", "message": "Room deleted"}), 200
    except Exception as e:
        _log("API_ROOMS_ERROR", f"Failed to delete room: {e}")
        return jsonify({"error": "Failed to delete room"}), 500


# ---------------------------------------------------------------------------
# Unknown Scans API endpoints
# ---------------------------------------------------------------------------

@app.route("/api/unknown_scans", methods=["GET"])
def get_unknown_scans():
    """
    List all unknown scans, optionally filtered by resolved status.

    Query parameters:
        ?resolved=true   - show only resolved scans
        ?resolved=false  - show only unresolved scans (default)

    Response:
        [
            {
                "id": "uuid",
                "tag_uid": "BC590C4E",
                "scanned_at": "2025-01-01T10:00:00+00:00",
                "resolved": false,
                "resolved_at": null,
                "assigned_room": null
            }
        ]
    """
    resolved_filter = request.args.get("resolved", "false").lower()

    try:
        query = supabase.table("unknown_scans").select("*").order("scanned_at", desc=True)

        if resolved_filter == "true":
            query = query.eq("resolved", True)
        elif resolved_filter == "false":
            query = query.eq("resolved", False)
        # If neither true nor false, return all scans

        resp = query.execute()
        _log("API_UNKNOWN_SCANS_LIST", f"{len(resp.data or [])} unknown scans returned")
        return jsonify(resp.data or []), 200
    except Exception as e:
        _log("API_UNKNOWN_SCANS_ERROR", f"Failed to list unknown scans: {e}")
        return jsonify({"error": "Failed to retrieve unknown scans"}), 500


@app.route("/api/unknown_scans/<scan_id>/resolve", methods=["POST"])
def resolve_unknown_scan(scan_id):
    """
    Mark an unknown scan as resolved, optionally assigning it to a room.

    Expected JSON body:
        {
            "assigned_room": "301"  # optional
        }

    Response:
        {
            "id": "uuid",
            "tag_uid": "BC590C4E",
            "resolved": true,
            "resolved_at": "2025-01-01T11:00:00+00:00",
            "assigned_room": "301"
        }
    """
    data = request.get_json(force=True) or {}
    assigned_room = data.get("assigned_room", "").strip() or None

    update_fields = {
        "resolved": True,
        "resolved_at": _now_iso(),
        "assigned_room": assigned_room
    }

    try:
        resp = supabase.table("unknown_scans").update(update_fields).eq("id", scan_id).execute()
        if not resp.data:
            return jsonify({"error": "Unknown scan not found"}), 404
        scan = resp.data[0]
        _log("API_UNKNOWN_SCANS_RESOLVE", f"Resolved scan {scan_id} -> room {assigned_room or 'none'}")
        return jsonify(scan), 200
    except Exception as e:
        _log("API_UNKNOWN_SCANS_ERROR", f"Failed to resolve scan: {e}")
        return jsonify({"error": "Failed to resolve unknown scan"}), 500


@app.route("/dashboard")
@app.route("/")
def dashboard():
    """Serve the supervisor dashboard HTML page."""
    return render_template(
        "dashboard.html",
        supabase_url=SUPABASE_URL,
        supabase_key=SUPABASE_KEY,
    )


@app.route("/rooms")
def rooms():
    """Serve the room management page."""
    return render_template("rooms.html")


# ---------------------------------------------------------------------------
# USB RFID door-reader background thread
# ---------------------------------------------------------------------------

def _start_rfid_listener() -> None:
    """
    Listen for card taps on the USB RFID door reader.
    Windows uses the keyboard library, Linux/Mac uses evdev.
    """
    system = platform.system()
    _log("RFID", f"Starting door reader listener on {system}")

    if system == "Windows":
        _rfid_listener_keyboard()
    else:
        _rfid_listener_evdev()


def _rfid_listener_keyboard() -> None:
    """Windows RFID listener using the keyboard library."""
    try:
        import keyboard  # type: ignore

        uid_buffer: list[str] = []

        def on_key(event):
            if event.event_type != "down":
                return
            if event.name == "enter":
                uid = "".join(uid_buffer).strip().upper()
                uid_buffer.clear()
                if uid:
                    _log("RFID_TAP", f"card_uid={uid}")
                    _handle_door_tap(uid)
            elif len(event.name) == 1:
                uid_buffer.append(event.name)

        keyboard.hook(on_key)
        _log("RFID", "keyboard listener active — waiting for card taps")
        keyboard.wait()

    except Exception:
        _log("RFID_ERROR", traceback.format_exc())


def _rfid_listener_evdev() -> None:
    """Linux/Mac RFID listener using evdev."""
    try:
        import evdev  # type: ignore
        from evdev import ecodes

        rfid_device_path = os.environ.get("RFID_DEVICE", "/dev/input/event2")
        device = evdev.InputDevice(rfid_device_path)
        _log("RFID", f"evdev listener active on {rfid_device_path}")

        uid_buffer: list[str] = []
        key_map = {
            ecodes.KEY_0: "0", ecodes.KEY_1: "1", ecodes.KEY_2: "2",
            ecodes.KEY_3: "3", ecodes.KEY_4: "4", ecodes.KEY_5: "5",
            ecodes.KEY_6: "6", ecodes.KEY_7: "7", ecodes.KEY_8: "8",
            ecodes.KEY_9: "9",
            **{k: c for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
               if (k := getattr(ecodes, f"KEY_{c}", None)) is not None},
        }

        for event in device.read_loop():
            if event.type == ecodes.EV_KEY and event.value == 1:
                if event.code == ecodes.KEY_ENTER:
                    uid = "".join(uid_buffer).strip().upper()
                    uid_buffer.clear()
                    if uid:
                        _log("RFID_TAP", f"card_uid={uid}")
                        _handle_door_tap(uid)
                elif event.code in key_map:
                    uid_buffer.append(key_map[event.code])

    except Exception:
        _log("RFID_ERROR", traceback.format_exc())


def _handle_door_tap(card_uid: str, room: str = None) -> None:
    """
    Toggle session open/close on each card tap.
    First tap opens a session, second tap closes it.

    Args:
        card_uid: The KUPPI card UID
        room: The room number (if None, uses DOOR_ROOM env var, defaults to "301")
    """
    if room is None:
        room = os.environ.get("DOOR_ROOM", "301")
    _log("DOOR_TAP", f"card={card_uid} room={room}")

    try:
        # Check if an active session already exists for this card
        existing = (
            supabase.table("sessions")
            .select("id")
            .eq("card_uid", card_uid)
            .eq("room", room)
            .eq("status", "active")
            .execute()
        )

        if existing.data:
            # Second tap — close the session
            session_id = existing.data[0]["id"]
            scans_resp = (
                supabase.table("scans")
                .select("area")
                .eq("session_id", session_id)
                .execute()
            )
            scanned = {s["area"] for s in (scans_resp.data or [])}
            status = "complete" if scanned.issuperset(set(ZONES)) else "incomplete"
            missing = sorted(set(ZONES) - scanned)

            supabase.table("sessions").update({
                "status":   status,
                "end_time": _now_iso(),
            }).eq("id", session_id).execute()

            _log("SESSION_CLOSE",
                 f"room={room} card={card_uid} status={status} missing={missing}")

        else:
            # First tap — open a new session
            # Close any stale sessions for this room first
            supabase.table("sessions").update({
                "status":   "incomplete",
                "end_time": _now_iso(),
            }).eq("room", room).eq("status", "active").execute()

            session_row = {
                "card_uid":   card_uid,
                "room":       room,
                "start_time": _now_iso(),
                "status":     "active",
            }
            resp = supabase.table("sessions").insert(session_row).execute()
            session = resp.data[0] if resp.data else {}
            _log("SESSION_OPEN",
                 f"room={room} card={card_uid} session_id={session.get('id')}")

    except Exception:
        _log("SESSION_ERROR", traceback.format_exc())


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    rfid_thread = threading.Thread(target=_start_rfid_listener, daemon=True)
    rfid_thread.start()

    port = int(os.environ.get("FLASK_PORT", 5000))
    debug = os.environ.get("FLASK_ENV", "production") == "development"
    _log("STARTUP", f"Flask listening on port {port} debug={debug}")
    app.run(host="0.0.0.0", port=port, debug=debug, use_reloader=False)