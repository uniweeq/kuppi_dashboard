-- =============================================================================
-- KUPPI Dashboard — Supabase Schema
-- Run this SQL in the Supabase SQL Editor to set up all required tables.
-- =============================================================================

-- ---------------------------------------------------------------------------
-- 1. sessions
--    One row per cleaning session.  Opened when staff taps door reader,
--    closed (with status cleaning/awaiting_approval/available/incomplete) when they leave.
--    Status flow: not_cleaned -> cleaning -> awaiting_approval -> available
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS sessions (
    id          UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    card_uid    TEXT        NOT NULL,
    room        TEXT        NOT NULL,
    start_time  TIMESTAMPTZ NOT NULL DEFAULT now(),
    end_time    TIMESTAMPTZ,
    status      TEXT        NOT NULL DEFAULT 'cleaning'
                            CHECK (status IN ('cleaning', 'awaiting_approval', 'available', 'incomplete'))
);

CREATE INDEX IF NOT EXISTS idx_sessions_room_status ON sessions (room, status);
CREATE INDEX IF NOT EXISTS idx_sessions_card_uid    ON sessions (card_uid);

-- ---------------------------------------------------------------------------
-- 2. scans
--    One row for every NFC tag scan performed by a KUPPI card.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS scans (
    id          UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    session_id  UUID        REFERENCES sessions(id) ON DELETE SET NULL,
    tag_uid     TEXT        NOT NULL,
    area        TEXT        NOT NULL,
    timestamp   TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS idx_scans_session_id ON scans (session_id);
CREATE INDEX IF NOT EXISTS idx_scans_timestamp  ON scans (timestamp DESC);

-- ---------------------------------------------------------------------------
-- 3. zone_tags (formerly rooms)
--    Master list of zone NFC tags.  Each room has 6 NFC tag definitions (one per
--    zone).  The tag_uid is the UID programmed into the physical NFC sticker.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS zone_tags (
    id           UUID  PRIMARY KEY DEFAULT gen_random_uuid(),
    room_number  TEXT  NOT NULL,
    tag_uid      TEXT  NOT NULL UNIQUE,
    area_name    TEXT  NOT NULL
                       CHECK (area_name IN (
                           'Toilet', 'Wardrobe', 'Study Desk',
                           'Bed', 'Curtain', 'Drinks Bar'
                       ))
);

CREATE INDEX IF NOT EXISTS idx_zone_tags_room_number ON zone_tags (room_number);

-- ---------------------------------------------------------------------------
-- 4. rooms (Room Management Table)
--    One row per room with status, door NFC tag, and metadata.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS rooms (
    id           UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    room_number  TEXT        NOT NULL UNIQUE,
    status       TEXT        NOT NULL DEFAULT 'available'
                             CHECK (status IN ('available', 'blocked', 'maintenance', 'inactive')),
    reason       TEXT,
    nfc_uid      TEXT,
    floor        TEXT,
    room_type    TEXT,
    created_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at   TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS idx_rooms_room_number ON rooms (room_number);
CREATE INDEX IF NOT EXISTS idx_rooms_status ON rooms (status);
CREATE INDEX IF NOT EXISTS idx_rooms_nfc_uid ON rooms (nfc_uid);

-- ---------------------------------------------------------------------------
-- 5. unknown_scans
--    Logs scans of unconfigured/unknown NFC tags for resolution.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS unknown_scans (
    id             UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    tag_uid        TEXT        NOT NULL,
    scanned_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    resolved       BOOLEAN     NOT NULL DEFAULT false,
    resolved_at    TIMESTAMPTZ,
    assigned_room  TEXT
);

CREATE INDEX IF NOT EXISTS idx_unknown_scans_resolved ON unknown_scans (resolved);
CREATE INDEX IF NOT EXISTS idx_unknown_scans_tag_uid ON unknown_scans (tag_uid);

-- ---------------------------------------------------------------------------
-- 6. room_status view
--    Convenience view joining sessions and scans to produce a per-room
--    summary of the most recent session.
-- ---------------------------------------------------------------------------
CREATE OR REPLACE VIEW room_status AS
WITH latest_sessions AS (
    -- One row per room — the most recently started session
    SELECT DISTINCT ON (room)
           id          AS session_id,
           room,
           card_uid,
           start_time,
           end_time,
           status
    FROM   sessions
    ORDER  BY room, start_time DESC
),
session_scans AS (
    -- Aggregate scanned areas per session
    SELECT   session_id,
             COUNT(DISTINCT area)              AS zones_done,
             array_agg(DISTINCT area ORDER BY area) AS scanned_areas
    FROM     scans
    GROUP BY session_id
)
SELECT  ls.room,
        ls.status,
        ls.start_time,
        ls.end_time,
        COALESCE(ss.zones_done, 0)      AS zones_done,
        COALESCE(ss.scanned_areas, '{}') AS scanned_areas
FROM    latest_sessions ls
LEFT JOIN session_scans ss ON ss.session_id = ls.session_id;

-- ---------------------------------------------------------------------------
-- 7. Enable Supabase real-time on the relevant tables
--    (Supabase uses publication-based replication.  Add tables here so that
--    the JS client receives change events.)
-- ---------------------------------------------------------------------------

-- If the supabase_realtime publication does not yet exist, create it:
DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1 FROM pg_publication WHERE pubname = 'supabase_realtime'
    ) THEN
        CREATE PUBLICATION supabase_realtime;
    END IF;
END $$;

ALTER PUBLICATION supabase_realtime ADD TABLE sessions;
ALTER PUBLICATION supabase_realtime ADD TABLE scans;
ALTER PUBLICATION supabase_realtime ADD TABLE rooms;
ALTER PUBLICATION supabase_realtime ADD TABLE unknown_scans;

-- ---------------------------------------------------------------------------
-- 8. Trigger to auto-update updated_at timestamp on rooms table
-- ---------------------------------------------------------------------------
CREATE OR REPLACE FUNCTION update_updated_at_column()
RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = now();
    RETURN NEW;
END;
$$ language 'plpgsql';

CREATE TRIGGER update_rooms_updated_at BEFORE UPDATE ON rooms
    FOR EACH ROW EXECUTE FUNCTION update_updated_at_column();

-- ---------------------------------------------------------------------------
-- 9. Sample seed data (optional — remove in production)
-- ---------------------------------------------------------------------------
-- INSERT INTO zone_tags (room_number, tag_uid, area_name) VALUES
--   ('101', 'TAG-101-TOI', 'Toilet'),
--   ('101', 'TAG-101-WAR', 'Wardrobe'),
--   ('101', 'TAG-101-STU', 'Study Desk'),
--   ('101', 'TAG-101-BED', 'Bed'),
--   ('101', 'TAG-101-CUR', 'Curtain'),
--   ('101', 'TAG-101-DRK', 'Drinks Bar'),
--   ('102', 'TAG-102-TOI', 'Toilet'),
--   ('102', 'TAG-102-WAR', 'Wardrobe'),
--   ('102', 'TAG-102-STU', 'Study Desk'),
--   ('102', 'TAG-102-BED', 'Bed'),
--   ('102', 'TAG-102-CUR', 'Curtain'),
--   ('102', 'TAG-102-DRK', 'Drinks Bar');

-- INSERT INTO rooms (room_number, status, nfc_uid, floor, room_type) VALUES
--   ('101', 'available', 'DOOR-101', '1', 'Standard'),
--   ('102', 'available', 'DOOR-102', '1', 'Standard'),
--   ('201', 'available', 'DOOR-201', '2', 'Deluxe'),
--   ('301', 'available', 'DOOR-301', '3', 'Suite');
