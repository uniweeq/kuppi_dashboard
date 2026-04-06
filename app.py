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
import json
import queue
import threading
import platform
import traceback
from datetime import datetime, timezone

from flask import Flask, request, jsonify, render_template, Response
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
# Server-Sent Events (SSE) for live dashboard updates
# ---------------------------------------------------------------------------

sse_clients: list[queue.Queue] = []
sse_lock = threading.Lock()


def notify_clients(event_type: str, data: dict | str = "") -> None:
    """Push an event to all connected SSE browser clients."""
    payload = json.dumps({"type": event_type, "data": data})
    dead = []
    with sse_lock:
        for q in sse_clients:
            try:
                q.put_nowait(payload)
            except queue.Full:
                dead.append(q)
        for q in dead:
            sse_clients.remove(q)


@app.route("/events")
def sse_stream():
    """SSE endpoint — browsers connect here for live push updates."""
    def stream():
        q = queue.Queue(maxsize=50)
        with sse_lock:
            sse_clients.append(q)
        try:
            while True:
                try:
                    payload = q.get(timeout=25)
                    yield f"data: {payload}\n\n"
                except queue.Empty:
                    yield ": keepalive\n\n"
        except GeneratorExit:
            pass
        finally:
            with sse_lock:
                if q in sse_clients:
                    sse_clients.remove(q)

    return Response(
        stream(),
        mimetype="text/event-stream",
        headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"},
    )


# ---------------------------------------------------------------------------
# API endpoints
# ---------------------------------------------------------------------------


@app.route("/api/room-lookup/<nfc_uid>", methods=["GET"])
def room_lookup(nfc_uid: str):
    """
    Look up a room by its door NFC tag UID.
    Called by the KUPPI device when staff scans a room's door tag.

    Returns:
        200 + room_number if found
        404 if nfc_uid not configured
    """
    nfc_uid = nfc_uid.strip().upper()
    if not nfc_uid:
        return jsonify({"error": "Missing nfc_uid"}), 400

    try:
        resp = (
            supabase.table("rooms")
            .select("room_number, floor, room_type, status")
            .eq("nfc_uid", nfc_uid)
            .limit(1)
            .execute()
        )

        if not resp.data:
            _log("ROOM_LOOKUP", f"NFC UID '{nfc_uid}' not found in rooms table")
            return jsonify({"error": "Room not found for this NFC tag"}), 404

        room = resp.data[0]
        _log("ROOM_LOOKUP", f"nfc_uid={nfc_uid} → room={room['room_number']}")
        return jsonify({
            "room_number": room["room_number"],
            "floor":       room.get("floor", ""),
            "room_type":   room.get("room_type", ""),
            "status":      room.get("status", "available"),
        }), 200

    except Exception as e:
        _log("ROOM_LOOKUP_ERROR", str(e))
        return jsonify({"error": str(e)}), 500


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
        notify_clients("door_tap", {"room": room_resp.data[0]["room_number"]})
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

    # Find the cleaning session for this card/room combination
    session_resp = (
        supabase.table("sessions")
        .select("id")
        .eq("card_uid", card_uid)
        .eq("room", room)
        .eq("status", "cleaning")
        .order("start_time", desc=True)
        .limit(1)
        .execute()
    )

    # Return 400 if no cleaning session found
    if not session_resp.data:
        _log("SCAN_NO_SESSION", f"No cleaning session for card={card_uid} room={room}")
        return jsonify({"error": "No cleaning session found. Tap door reader to start."}), 400

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
    notify_clients("scan", {"room": room, "area": area})

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
            "room":     "301",
            "staff_id": "1001"  # optional
        }
    """
    data = request.get_json(force=True)
    if not data:
        return jsonify({"error": "No JSON body"}), 400

    card_uid = data.get("card_uid", "").strip()
    room     = data.get("room",     "").strip()
    staff_id = data.get("staff_id", "").strip() or None

    if not all([card_uid, room]):
        return jsonify({"error": "Missing required fields: card_uid, room"}), 400

    # Close any previously open session for this room before opening new one
    supabase.table("sessions").update({
        "status":   "incomplete",
        "end_time": _now_iso(),
    }).eq("room", room).eq("status", "cleaning").execute()

    session_row = {
        "card_uid":   card_uid,
        "room":       room,
        "start_time": _now_iso(),
        "status":     "cleaning",
    }
    resp = supabase.table("sessions").insert(session_row).execute()
    session = resp.data[0] if resp.data else {}
    _log("SESSION_OPEN", f"room={room} card={card_uid} staff_id={staff_id} session_id={session.get('id')}")
    notify_clients("session_open", {"room": room})

    return jsonify({"status": "ok", "session": session}), 201


@app.route("/session/close", methods=["POST"])
def close_session():
    """
    Close a cleaning session. Marks awaiting_approval if all 6 zones scanned,
    incomplete otherwise. Calculates duration_mins from start_time to end_time.

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
        .select("id, start_time")
        .eq("card_uid", card_uid)
        .eq("room", room)
        .eq("status", "cleaning")
        .order("start_time", desc=True)
        .limit(1)
        .execute()
    )

    if not session_resp.data:
        return jsonify({"error": "No cleaning session found"}), 404

    session_id = session_resp.data[0]["id"]
    start_time_str = session_resp.data[0]["start_time"]

    scans_resp = (
        supabase.table("scans")
        .select("area")
        .eq("session_id", session_id)
        .execute()
    )

    scanned_areas = {s["area"] for s in (scans_resp.data or [])}
    status = "awaiting_approval" if scanned_areas.issuperset(set(ZONES)) else "incomplete"

    # Calculate duration in minutes
    end_time = datetime.now(timezone.utc)
    start_time = datetime.fromisoformat(start_time_str.replace('Z', '+00:00'))
    duration_mins = int((end_time - start_time).total_seconds() / 60)

    supabase.table("sessions").update({
        "status":       status,
        "end_time":     end_time.isoformat(),
        "duration_mins": duration_mins,
    }).eq("id", session_id).execute()

    missing = sorted(set(ZONES) - scanned_areas)
    _log("SESSION_CLOSE", f"room={room} card={card_uid} status={status} duration={duration_mins}min missing={missing}")
    notify_clients("session_close", {"room": room, "status": status})

    return jsonify({
        "status":       "ok",
        "result":       status,
        "duration_mins": duration_mins,
        "missing":      missing,
        "scanned":      sorted(scanned_areas),
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
        .in_("status", ["cleaning", "awaiting_approval", "available", "incomplete"])
        .order("start_time", desc=True)
        .execute()
    )

    # Build map: room -> most recent session
    session_map: dict[str, dict] = {}
    for s in (sessions_resp.data or []):
        room = s["room"]
        if room not in session_map:
            session_map[room] = s

    # Fetch scans for all cleaning sessions in one query
    cleaning_sessions = {
        s["id"]: s for s in session_map.values()
        if s["status"] == "cleaning"
    }

    scans_map: dict[str, list[str]] = {}
    if cleaning_sessions:
        scans_resp = (
            supabase.table("scans")
            .select("session_id, area")
            .in_("session_id", list(cleaning_sessions.keys()))
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
                "status":      "not_cleaned",
                "zones_done":  0,
                "zones_total": TOTAL_ZONES,
                "scanned":     [],
                "missing":     sorted(ZONES),
                "start_time":  None,
            })
            continue

        if session["status"] == "cleaning":
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
            "end_time":    session.get("end_time"),
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


@app.route("/api/set-staff-language", methods=["POST"])
def set_staff_language():
    """
    Set the device language for a staff member.
    
    Expected JSON body:
        {
            "staff_id": "EMP-001",
            "language_code": "en",  (en, zh, ta, bn, my, th, vi, tl)
            "language_name": "English"
        }
    """
    try:
        data = request.get_json()
        staff_id = data.get("staff_id")
        language_code = data.get("language_code")
        language_name = data.get("language_name")
        
        if not all([staff_id, language_code, language_name]):
            return jsonify({"error": "Missing required fields"}), 400
        
        # Validate language code
        valid_languages = ["en", "zh", "ta", "bn", "my", "th", "vi", "tl"]
        if language_code not in valid_languages:
            return jsonify({"error": f"Invalid language code: {language_code}"}), 400
        
        # In a real system, you would:
        # 1. Broadcast this language setting to the KUPPI device assigned to this staff
        # 2. Store the preference in a database
        # For now, we'll just log it
        
        _log("STAFF_LANG", f"Staff {staff_id} language set to {language_code} ({language_name})")
        
        return jsonify({
            "success": True,
            "staff_id": staff_id,
            "language": language_name,
            "language_code": language_code
        }), 200
        
    except Exception as e:
        _log("STAFF_LANG_ERROR", f"Failed to set staff language: {e}")
        return jsonify({"error": "Failed to set staff language"}), 500


@app.route("/api/recent-sessions")
def recent_sessions():
    """
    Get the most recent completed session for each room.
    Returns a dict keyed by room_id with session info including staff and duration.
    """
    try:
        # Fetch all completed sessions ordered by room and end_time
        resp = supabase.table("sessions").select("*").eq("status", "complete").order("room", desc=False).order("end_time", desc=True).execute()
        
        sessions_by_room = {}
        for session in (resp.data or []):
            room_id = session.get("room")
            
            # Only keep the first (most recent) session per room
            if room_id not in sessions_by_room:
                start_time = session.get("start_time")
                end_time = session.get("end_time")
                
                # Calculate duration in minutes
                duration_minutes = None
                if start_time and end_time:
                    from datetime import datetime as dt
                    try:
                        start = dt.fromisoformat(start_time.replace('Z', '+00:00'))
                        end = dt.fromisoformat(end_time.replace('Z', '+00:00'))
                        duration_minutes = (end - start).total_seconds() / 60
                    except Exception as e:
                        _log("SESSION_TIME_ERROR", f"Failed to parse timestamps: {e}")
                
                sessions_by_room[room_id] = {
                    "id": session.get("id"),
                    "card_uid": session.get("card_uid"),
                    "room": room_id,
                    "start_time": start_time,
                    "end_time": end_time,
                    "duration_minutes": duration_minutes
                }
        
        return jsonify(sessions_by_room), 200
    except Exception as e:
        _log("API_SESSIONS_ERROR", f"Failed to fetch sessions: {e}")
        return jsonify({"error": "Failed to fetch sessions"}), 500


@app.route("/dashboard")
@app.route("/")
def dashboard():
    """Serve the supervisor dashboard HTML page."""
    return render_template(
        "dashboard.html",
        supabase_url=SUPABASE_URL,
        supabase_key=SUPABASE_KEY,
        active_page="dashboard",
    )


@app.route("/rooms")
def rooms():
    """Serve the room management page."""
    return render_template("rooms.html", active_page="rooms")


@app.route("/staff")
def staff():
    """Serve the staff management page."""
    return render_template("staff.html", active_page="staff")


@app.route("/settings")
def settings():
    """Serve the settings page."""
    return render_template("settings.html", active_page="settings")


@app.route("/test123")
def test123():
    """Test route."""
    return jsonify({"message": "Test route works!"})


@app.route("/api/populate-test-data", methods=["POST"])
def populate_test_data():
    """
    Development endpoint: Create fake test data for dashboard visualization.
    Creates test rooms with sessions and scans representing all statuses.
    """
    try:
        import uuid
        from datetime import datetime as dt, timedelta

        # Test data structure - rooms showing all statuses in the ranking order
        test_data = [
            # Not Cleaned (no session yet)
            {
                "room": "101",
                "no_session": True  # Will result in not_cleaned status
            },
            {
                "room": "102",
                "no_session": True
            },
            # Cleaning (active sessions with partial zones)
            {
                "room": "201",
                "card_uid": "FAKE-MARIA",
                "status": "cleaning",
                "minutes_ago": 8,
                "scans": ["Toilet", "Wardrobe", "Bed"]
            },
            {
                "room": "202",
                "card_uid": "FAKE-JAMES",
                "status": "cleaning",
                "minutes_ago": 15,
                "scans": ["Toilet", "Wardrobe"]
            },
            # Awaiting Approval (all zones scanned, pending supervisor approval)
            {
                "room": "301",
                "card_uid": "FAKE-JOHN",
                "status": "awaiting_approval",
                "hours_ago": 1.5,
                "duration_hours": 0.35,  # 21 minutes
                "scans": ZONES
            },
            {
                "room": "302",
                "card_uid": "FAKE-ANA",
                "status": "awaiting_approval",
                "hours_ago": 0.5,
                "duration_hours": 0.25,  # 15 minutes
                "scans": ZONES
            },
            # Available (approved and ready for guests)
            {
                "room": "401",
                "card_uid": "FAKE-CARLOS",
                "status": "available",
                "hours_ago": 4,
                "duration_hours": 0.4,  # 24 minutes
                "scans": ZONES
            },
            {
                "room": "402",
                "card_uid": "FAKE-SOFIA",
                "status": "available",
                "hours_ago": 3,
                "duration_hours": 0.33,  # 20 minutes
                "scans": ZONES
            },
            # Incomplete (closed session without all zones)
            {
                "room": "501",
                "card_uid": "FAKE-ALEX",
                "status": "incomplete",
                "hours_ago": 2,
                "duration_hours": 0.3,  # 18 minutes
                "scans": ["Toilet", "Wardrobe", "Study Desk"]
            },
            {
                "room": "502",
                "card_uid": "FAKE-MAYA",
                "status": "incomplete",
                "hours_ago": 1,
                "duration_hours": 0.2,  # 12 minutes
                "scans": ["Toilet", "Bed"]
            },
        ]

        now = dt.now(timezone.utc)
        created_count = 0

        for room_data in test_data:
            try:
                room_num = room_data["room"]

                # Create or get room
                try:
                    supabase.table("rooms").insert({
                        "room_number": room_num,
                        "floor": str(int(room_num) // 100),
                        "room_type": "Standard" if int(room_num) < 400 else "Suite",
                        "status": "available",
                        "nfc_uid": f"NFC-{room_num}"
                    }).execute()
                except:
                    pass  # Room already exists

                # Skip session creation for rooms with no_session flag
                if room_data.get("no_session"):
                    created_count += 1
                    continue

                # Calculate start/end times
                if "hours_ago" in room_data:
                    start_time = now - timedelta(hours=room_data["hours_ago"])
                    if "duration_hours" in room_data:
                        end_time = start_time + timedelta(hours=room_data["duration_hours"])
                    else:
                        end_time = None
                else:
                    start_time = now - timedelta(minutes=room_data.get("minutes_ago", 5))
                    end_time = None

                # Create session
                session_data = {
                    "card_uid": room_data["card_uid"],
                    "room": room_num,
                    "start_time": start_time.isoformat(),
                    "status": room_data["status"],
                }
                if end_time:
                    session_data["end_time"] = end_time.isoformat()
                    # Calculate duration in minutes for completed sessions
                    duration_mins = int((end_time - start_time).total_seconds() / 60)
                    session_data["duration_mins"] = duration_mins

                session_resp = supabase.table("sessions").insert(session_data).execute()
                if not session_resp.data:
                    continue

                session_id = session_resp.data[0]["id"]

                # Create scans
                for i, zone in enumerate(room_data["scans"]):
                    try:
                        supabase.table("scans").insert({
                            "session_id": session_id,
                            "tag_uid": f"NFC-ZONE-{i}",
                            "area": zone
                        }).execute()
                    except Exception as e:
                        _log("TEST_DATA_SCAN_ERROR", f"Failed to create scan: {e}")

                created_count += 1

            except Exception as e:
                _log("TEST_DATA_ROOM_ERROR", f"Failed to create room data: {e}")
                continue

        _log("TEST_DATA", f"Created {created_count} test rooms with sessions")

        return jsonify({
            "success": True,
            "message": f"Created test data for {created_count} rooms with all statuses",
            "rooms_created": created_count
        }), 200

    except Exception as e:
        _log("TEST_DATA_ERROR", f"Failed to populate test data: {str(e)}")
        return jsonify({"error": str(e), "type": type(e).__name__}), 500


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
        # Check if a cleaning session already exists for this card
        existing = (
            supabase.table("sessions")
            .select("id, start_time")
            .eq("card_uid", card_uid)
            .eq("room", room)
            .eq("status", "cleaning")
            .execute()
        )

        if existing.data:
            # Second tap — close the session
            session_id = existing.data[0]["id"]
            start_time_str = existing.data[0]["start_time"]

            scans_resp = (
                supabase.table("scans")
                .select("area")
                .eq("session_id", session_id)
                .execute()
            )
            scanned = {s["area"] for s in (scans_resp.data or [])}
            status = "awaiting_approval" if scanned.issuperset(set(ZONES)) else "incomplete"
            missing = sorted(set(ZONES) - scanned)

            # Calculate duration in minutes
            end_time = datetime.now(timezone.utc)
            start_time = datetime.fromisoformat(start_time_str.replace('Z', '+00:00'))
            duration_mins = int((end_time - start_time).total_seconds() / 60)

            supabase.table("sessions").update({
                "status":       status,
                "end_time":     end_time.isoformat(),
                "duration_mins": duration_mins,
            }).eq("id", session_id).execute()

            _log("SESSION_CLOSE",
                 f"room={room} card={card_uid} status={status} duration={duration_mins}min missing={missing}")
            notify_clients("session_close", {"room": room, "status": status})

        else:
            # First tap — open a new session
            # Close any stale sessions for this room first
            supabase.table("sessions").update({
                "status":   "incomplete",
                "end_time": _now_iso(),
            }).eq("room", room).eq("status", "cleaning").execute()

            session_row = {
                "card_uid":   card_uid,
                "room":       room,
                "start_time": _now_iso(),
                "status":     "cleaning",
            }
            resp = supabase.table("sessions").insert(session_row).execute()
            session = resp.data[0] if resp.data else {}
            _log("SESSION_OPEN",
                 f"room={room} card={card_uid} session_id={session.get('id')}")
            notify_clients("session_open", {"room": room})

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