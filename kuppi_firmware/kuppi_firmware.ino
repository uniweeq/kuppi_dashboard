#include <TFT_eSPI.h>
#include <Wire.h>
#include <Adafruit_PN532.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ── MUTEX ────────────────────────────────────────────────────
SemaphoreHandle_t i2cMutex;

// ── PIN DEFINITIONS ─────────────────────────────────────────
#define BUZZER_PIN   25
#define TOUCH_ADDR   0x38

// ── WI-FI & SERVER CONFIGURATION ────────────────────────────
const char* WIFI_SSID     = "Fifone";
const char* WIFI_PASSWORD = "manutd10";
const char* SERVER_IP     = "172.20.10.9";
const int   SERVER_PORT   = 5000;
const char* CARD_UID      = "KUPPI-001";

// ── DYNAMIC ROOM (set by NFC scan) ──────────────────────────
char activeRoom[16]       = "";   // filled after scanning room NFC tag
bool roomIdentified       = false;

// ── DISPLAY & NFC OBJECTS ───────────────────────────────────
TFT_eSPI tft = TFT_eSPI();
Adafruit_PN532 nfc(-1, -1);

// ── COLOURS ─────────────────────────────────────────────────
#define COL_BG          tft.color565(15,   14,  14)
#define COL_RED         tft.color565(200,  50,  50)
#define COL_RED_DARK    tft.color565(120,  25,  25)
#define COL_GREEN       tft.color565(50,   200, 120)
#define COL_GREEN_DARK  tft.color565(25,   110,  60)
#define COL_WHITE       0xFFFF
#define COL_GRAY        tft.color565(200, 200, 200)
#define COL_HEADER      tft.color565(40,   40,  40)
#define COL_PILL_OFF    tft.color565(55,   55,  55)
#define COL_PILL_ON     tft.color565(25,   110,  60)
#define COL_ICON_OFF    tft.color565(90,   90,  90)
#define COL_ICON_ON     tft.color565(20,   85,  50)
#define COL_FADED       tft.color565(130, 130, 130)
#define COL_TIMER_FULL  tft.color565(50,   200, 120)
#define COL_TIMER_EMPTY tft.color565(45,   45,  45)
#define COL_SCROLLBAR   tft.color565(80,   80,  80)
#define COL_SCROLLTHUMB tft.color565(160, 160, 160)
#define COL_WIFI_OK     tft.color565(50,   200, 120)
#define COL_WIFI_FAIL   tft.color565(200,  50,  50)
#define COL_WIFI_SEND   tft.color565(255,  200,   0)

// ── ZONE DEFINITIONS ────────────────────────────────────────
#define NUM_ZONES  6
#define MAX_ITEMS  10

uint8_t zoneUIDs[NUM_ZONES][4] = {
  {0xBC, 0x59, 0x0C, 0x4E},
  {0xC4, 0x6C, 0x0C, 0x4E},
  {0xAA, 0x0D, 0x0D, 0x4E},
  {0x4D, 0x79, 0x0C, 0x4E},
  {0xD9, 0x5E, 0x0C, 0x4E},
  {0x51, 0x8D, 0x0C, 0x4E},
};

const char* zoneNames[NUM_ZONES] = {
  "Toilet", "Wardrobe", "Study Desk", "Bed", "Curtain", "Drinks Bar"
};

const char* zoneItems[NUM_ZONES][MAX_ITEMS] = {
  {"Toilet","Sink","Soap","Tissues","Amenities","Mirror","Shower","Bathtub","Towels","Floor"},
  {"Hangers","Iron","Safe","Laundry Bag","Slippers","","","","",""},
  {"Thermostat","Mirror","Plugs","Decor","Drawers","Chair","","","",""},
  {"Bed","Table","Drawers","Lighting","Controls","Writing Pad","","","",""},
  {"Seating","Window","TV","Tables","Drawers","Catalogue","Decor","Plugs","",""},
  {"Coffee","Refreshments","Kettle","Fridge","Counter","Floor","","","",""},
};

uint8_t zoneItemCount[NUM_ZONES] = {10, 5, 6, 6, 8, 6};

// ── STATE ───────────────────────────────────────────────────
bool zoneCompleted[NUM_ZONES]          = {false};
bool itemChecked[NUM_ZONES][MAX_ITEMS] = {false};

enum Screen { SCREEN_STAFF_LOGIN, SCREEN_SCAN_ROOM, SCREEN_HOME, SCREEN_CHECKLIST, SCREEN_COMPLETE };
Screen currentScreen = SCREEN_STAFF_LOGIN;
int activeZone   = -1;
int scrollOffset = 0;

// ── STAFF LOGIN STATE ────────────────────────────────────────
bool staffLoggedIn = false;
char staffName[32]  = "";
char staffApiId[64] = "";   // UUID returned by /api/staff-lookup

volatile bool pendingStaffLookup  = false;
char staffLookupUID[20]           = "";
volatile bool staffLookupDone     = false;
volatile bool staffLookupSuccess  = false;

// ── WI-FI STATUS ────────────────────────────────────────────
enum WifiStatus { WIFI_OFFLINE, WIFI_SENDING, WIFI_OK, WIFI_FAIL };
WifiStatus wifiStatus    = WIFI_OFFLINE;
unsigned long wifiStatusTime = 0;
#define WIFI_STATUS_RESET_MS 2000

// ── HTTP TASK QUEUE ─────────────────────────────────────────
volatile int  pendingScanZone     = -1;
volatile bool pendingSessionOpen  = false;
volatile bool pendingSessionClose = false;

// Room lookup state
volatile bool pendingRoomLookup   = false;
char roomLookupUID[20]            = "";
volatile bool roomLookupDone      = false;
volatile bool roomLookupSuccess   = false;
char roomStatus[32]               = "";   // status from server (e.g. "available", "awaiting_approval")

// Session open state (to detect 409 blocked)
volatile bool sessionOpenDone     = false;
volatile bool sessionOpenBlocked  = false;


// ── TIMER ───────────────────────────────────────────────────
#define TIMER_DURATION_MS  (25UL * 60UL * 1000UL)
unsigned long kuppiTimerStart = 0;
bool timerRunning = false;

// ── LAYOUT ──────────────────────────────────────────────────
#define BORDER_W      5
#define HEADER_H      44
#define ZONE_COL_W    62
#define SCROLL_W       8
#define PILL_AREA_W   (480 - BORDER_W - ZONE_COL_W - SCROLL_W - BORDER_W - 4)
#define PILL_W        ((PILL_AREA_W - 6) / 2)
#define PILL_H        52
#define PILL_GAP       6
#define PILL_X1       (BORDER_W + 2)
#define PILL_X2       (PILL_X1 + PILL_W + 6)
#define PILL_Y0       (HEADER_H + 6)
#define PILLS_VISIBLE  5

// ── TOUCH ────────────────────────────────────────────────────
struct TouchPoint { int x, y; bool pressed; };

TouchPoint readTouch() {
  TouchPoint tp = {0, 0, false};
  if(xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(30)) != pdTRUE) return tp;

  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) {
    xSemaphoreGive(i2cMutex);
    return tp;
  }
  Wire.requestFrom((uint8_t)TOUCH_ADDR, (uint8_t)7);
  if (Wire.available() >= 7) {
    uint8_t td = Wire.read();
    if (td > 0 && td < 6) {
      uint8_t xh = Wire.read(); uint8_t xl = Wire.read();
      uint8_t yh = Wire.read(); uint8_t yl = Wire.read();
      Wire.read(); Wire.read();
      int rawX = ((xh & 0x0F) << 8) | xl;
      int rawY = ((yh & 0x0F) << 8) | yl;
      tp.x = rawY;
      tp.y = 320 - rawX;
      tp.pressed = true;
      Serial.print("[TOUCH] screen=(");
      Serial.print(tp.x); Serial.print(","); Serial.print(tp.y); Serial.println(")");
    } else {
      while (Wire.available()) Wire.read();
    }
  }
  xSemaphoreGive(i2cMutex);
  return tp;
}

// ── BUZZER ──────────────────────────────────────────────────
void buzzOnce()     { digitalWrite(BUZZER_PIN,HIGH);delay(80); digitalWrite(BUZZER_PIN,LOW); }
void buzzComplete() { for(int i=0;i<3;i++){digitalWrite(BUZZER_PIN,HIGH);delay(60);digitalWrite(BUZZER_PIN,LOW);delay(60);} }
void buzzAllDone()  { digitalWrite(BUZZER_PIN,HIGH);delay(500);digitalWrite(BUZZER_PIN,LOW); }

// ── WI-FI HELPERS ───────────────────────────────────────────
void drawWifiDot() {
  uint16_t col;
  switch(wifiStatus){
    case WIFI_SENDING: col = COL_WIFI_SEND; break;
    case WIFI_OK:      col = COL_WIFI_OK;   break;
    case WIFI_FAIL:    col = COL_WIFI_FAIL; break;
    default:           col = COL_FADED;     break;
  }
  tft.fillCircle(BORDER_W + 6, BORDER_W + 6, 4, col);
}

bool connectWifi() {
  Serial.print("[WIFI] Connecting to ");
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("[WIFI] Connected! IP: ");
    Serial.println(WiFi.localIP());
    wifiStatus = WIFI_OK;
    return true;
  } else {
    Serial.println();
    Serial.println("[WIFI] Failed — running offline");
    wifiStatus = WIFI_OFFLINE;
    return false;
  }
}

String serverURL(const char* endpoint) {
  return String("http://") + SERVER_IP + ":" + SERVER_PORT + endpoint;
}

// ── HTTP SEND FUNCTIONS ──────────────────────────────────────
void sendScanEvent(int zoneIdx) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[HTTP] Offline — scan not sent");
    wifiStatus = WIFI_FAIL;
    wifiStatusTime = millis();
    return;
  }
  char tagUID[12];
  snprintf(tagUID, sizeof(tagUID), "%02X%02X%02X%02X",
    zoneUIDs[zoneIdx][0], zoneUIDs[zoneIdx][1],
    zoneUIDs[zoneIdx][2], zoneUIDs[zoneIdx][3]);
  StaticJsonDocument<256> doc;
  doc["card_uid"] = CARD_UID;
  doc["tag_uid"]  = tagUID;
  doc["area"]     = zoneNames[zoneIdx];
  doc["room"]     = activeRoom;
  String payload;
  serializeJson(doc, payload);
  Serial.print("[HTTP] POST /scan — area=");
  Serial.println(zoneNames[zoneIdx]);
  wifiStatus = WIFI_SENDING;
  drawWifiDot();
  HTTPClient http;
  http.begin(serverURL("/scan"));
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(3000);
  int code = http.POST(payload);
  wifiStatus = (code == 200 || code == 201) ? WIFI_OK : WIFI_FAIL;
  Serial.print("[HTTP] Scan response: "); Serial.println(code);
  wifiStatusTime = millis();
  http.end();
  drawWifiDot();
}

void sendSessionOpen() {
  if (WiFi.status() != WL_CONNECTED) { Serial.println("[HTTP] Offline — session open not sent"); return; }
  StaticJsonDocument<256> doc;
  doc["card_uid"] = CARD_UID;
  doc["room"]     = activeRoom;
  if (strlen(staffApiId) > 0) {
    doc["staff_id"] = staffApiId;
  }
  String payload;
  serializeJson(doc, payload);
  Serial.println("[HTTP] POST /session/open");
  HTTPClient http;
  http.begin(serverURL("/session/open"));
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(3000);
  int code = http.POST(payload);
  Serial.print("[HTTP] Session open: "); Serial.println(code);
  if (code == 409) {
    // Room is awaiting_approval or ready — blocked
    Serial.println("[HTTP] Session blocked by server (409)");
    sessionOpenBlocked = true;
  } else {
    sessionOpenBlocked = false;
  }
  sessionOpenDone = true;
  http.end();
}

void sendSessionClose() {
  if (WiFi.status() != WL_CONNECTED) { Serial.println("[HTTP] Offline — session close not sent"); return; }
  StaticJsonDocument<128> doc;
  doc["card_uid"] = CARD_UID;
  doc["room"]     = activeRoom;
  String payload;
  serializeJson(doc, payload);
  Serial.println("[HTTP] POST /session/close");
  HTTPClient http;
  http.begin(serverURL("/session/close"));
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(3000);
  int code = http.POST(payload);
  Serial.print("[HTTP] Session close: "); Serial.println(code);
  http.end();
}

void sendRoomLookup() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[HTTP] Offline — room lookup failed");
    roomLookupSuccess = false;
    roomLookupDone = true;
    return;
  }
  String url = String("http://") + SERVER_IP + ":" + SERVER_PORT + "/api/room-lookup/" + roomLookupUID;
  Serial.print("[HTTP] GET /api/room-lookup/"); Serial.println(roomLookupUID);
  HTTPClient http;
  http.begin(url);
  http.setTimeout(3000);
  int code = http.GET();
  Serial.print("[HTTP] Room lookup: "); Serial.println(code);

  if (code == 200) {
    String response = http.getString();
    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, response);
    if (!err) {
      const char* roomNum = doc["room_number"];
      const char* status  = doc["status"];
      if (roomNum) {
        strncpy(activeRoom, roomNum, sizeof(activeRoom) - 1);
        activeRoom[sizeof(activeRoom) - 1] = '\0';
        roomIdentified = true;
        roomLookupSuccess = true;
        // Capture room status
        if (status) {
          strncpy(roomStatus, status, sizeof(roomStatus) - 1);
          roomStatus[sizeof(roomStatus) - 1] = '\0';
        } else {
          strcpy(roomStatus, "available");
        }
        Serial.print("[HTTP] Room found: "); Serial.print(activeRoom);
        Serial.print(" status: "); Serial.println(roomStatus);
      } else {
        roomLookupSuccess = false;
      }
    } else {
      roomLookupSuccess = false;
    }
  } else {
    roomLookupSuccess = false;
  }
  http.end();
  roomLookupDone = true;
}

void sendStaffLookup() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[HTTP] Offline — staff lookup failed");
    staffLookupSuccess = false;
    staffLookupDone = true;
    return;
  }
  String url = String("http://") + SERVER_IP + ":" + SERVER_PORT + "/api/staff-lookup/" + staffLookupUID;
  Serial.print("[HTTP] GET /api/staff-lookup/"); Serial.println(staffLookupUID);
  HTTPClient http;
  http.begin(url);
  http.setTimeout(3000);
  int code = http.GET();
  Serial.print("[HTTP] Staff lookup: "); Serial.println(code);

  if (code == 200) {
    String response = http.getString();
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, response);
    if (!err) {
      const char* name = doc["name"];
      const char* id   = doc["id"];
      if (name && id) {
        strncpy(staffName,  name, sizeof(staffName)  - 1);
        strncpy(staffApiId, id,   sizeof(staffApiId) - 1);
        staffName[sizeof(staffName) - 1]   = '\0';
        staffApiId[sizeof(staffApiId) - 1] = '\0';
        staffLookupSuccess = true;
        Serial.print("[HTTP] Staff found: "); Serial.println(staffName);
      } else {
        staffLookupSuccess = false;
      }
    } else {
      staffLookupSuccess = false;
    }
  } else {
    staffLookupSuccess = false;
  }
  http.end();
  staffLookupDone = true;
}

// ── BACKGROUND HTTP TASK (Core 0) ────────────────────────────
void httpTask(void* parameter) {
  for(;;) {
    if(pendingStaffLookup) {
      pendingStaffLookup = false;
      sendStaffLookup();
    }
    if(pendingRoomLookup) {
      pendingRoomLookup = false;
      sendRoomLookup();
    }
    // Session open MUST be processed before scans, otherwise
    // the server rejects the scan with "No active session"
    if(pendingSessionOpen) {
      pendingSessionOpen = false;
      sendSessionOpen();
    }
    if(pendingScanZone >= 0) {
      int zone = pendingScanZone;
      pendingScanZone = -1;
      sendScanEvent(zone);
    }
    if(pendingSessionClose) {
      pendingSessionClose = false;
      sendSessionClose();
    }
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

// ── EDGE TIMER BORDER ────────────────────────────────────────
void drawTimerBorder() {
  uint32_t elapsed = timerRunning ? (millis() - kuppiTimerStart) : 0;
  if (elapsed > TIMER_DURATION_MS) elapsed = TIMER_DURATION_MS;
  float fraction = 1.0f - (float)elapsed / (float)TIMER_DURATION_MS;
  int W = 480, H = 320, B = BORDER_W;
  int perim  = 2 * (W + H);
  int filled = (int)(fraction * perim);
  tft.fillRect(0,   0,   W, B,   COL_TIMER_EMPTY);
  tft.fillRect(W-B, 0,   B, H,   COL_TIMER_EMPTY);
  tft.fillRect(0,   H-B, W, B,   COL_TIMER_EMPTY);
  tft.fillRect(0,   0,   B, H,   COL_TIMER_EMPTY);
  if (filled <= 0) return;
  int rem = filled, seg;
  seg = min(rem,W); if(seg>0) tft.fillRect(0,    0,    seg,B,   COL_TIMER_FULL); rem-=seg; if(rem<=0) return;
  seg = min(rem,H); if(seg>0) tft.fillRect(W-B,  0,    B,  seg, COL_TIMER_FULL); rem-=seg; if(rem<=0) return;
  seg = min(rem,W); if(seg>0) tft.fillRect(W-seg,H-B,  seg,B,   COL_TIMER_FULL); rem-=seg; if(rem<=0) return;
  seg = min(rem,H); if(seg>0) tft.fillRect(0,    H-seg,B,  seg, COL_TIMER_FULL);
}

// ── HEADER ──────────────────────────────────────────────────
void drawHeader(const char* left, const char* right, const char* centre, bool centreGreen) {
  tft.fillRect(BORDER_W, BORDER_W, 480-2*BORDER_W, HEADER_H-BORDER_W, COL_HEADER);
  tft.setTextColor(COL_GRAY);
  tft.setTextSize(2);
  tft.setCursor(BORDER_W+8, BORDER_W+10);
  tft.print(left);
  int rx = 480 - BORDER_W - (int)strlen(right)*12 - 8;
  tft.setCursor(rx, BORDER_W+10);
  tft.print(right);
  if(centre && strlen(centre) > 0){
    int cw = strlen(centre) * 12;
    int cx = (480 - cw) / 2;
    tft.setTextColor(centreGreen ? COL_GREEN : COL_WHITE);
    tft.setTextSize(2);
    tft.setCursor(cx, BORDER_W+10);
    tft.print(centre);
  }
  drawWifiDot();
}

// ── ZONE ICONS ──────────────────────────────────────────────
void drawToiletIcon(int cx, int cy, uint16_t c) {
  tft.fillRoundRect(cx-10, cy-22, 20, 10, 3, c);
  tft.fillRect(cx-5, cy-12, 10, 6, c);
  tft.fillEllipse(cx, cy+2, 14, 10, c);
  tft.fillEllipse(cx, cy+1, 9, 6, COL_BG);
  tft.fillRoundRect(cx-13, cy+11, 26, 5, 2, c);
}
void drawWardrobeIcon(int cx, int cy, uint16_t c) {
  tft.fillRoundRect(cx-18, cy-20, 36, 34, 3, c);
  tft.drawFastVLine(cx, cy-20, 34, COL_BG);
  tft.fillCircle(cx-5, cy-3, 3, COL_BG);
  tft.fillCircle(cx+5, cy-3, 3, COL_BG);
  tft.fillRect(cx-16, cy+14, 6, 6, c);
  tft.fillRect(cx+10, cy+14, 6, 6, c);
}
void drawDeskIcon(int cx, int cy, uint16_t c) {
  tft.fillRoundRect(cx-20, cy-8, 40, 6, 2, c);
  tft.fillRect(cx-17, cy-2, 5, 18, c);
  tft.fillRect(cx+12, cy-2, 5, 18, c);
  tft.fillRect(cx-17, cy+10, 34, 4, c);
  tft.fillRoundRect(cx-8, cy-18, 16, 10, 2, c);
  tft.fillRect(cx-2, cy-8, 4, 2, c);
}
void drawBedIcon(int cx, int cy, uint16_t c) {
  tft.fillRoundRect(cx-20, cy-16, 40, 8, 3, c);
  tft.fillRoundRect(cx-20, cy-8, 40, 18, 3, c);
  tft.fillRoundRect(cx-16, cy-14, 13, 8, 3, COL_BG);
  tft.fillRoundRect(cx+3,  cy-14, 13, 8, 3, COL_BG);
  tft.fillRect(cx-18, cy+10, 5, 7, c);
  tft.fillRect(cx+13, cy+10, 5, 7, c);
}
void drawCurtainIcon(int cx, int cy, uint16_t c) {
  tft.fillRoundRect(cx-20, cy-18, 40, 5, 2, c);
  for(int i=-12; i<=12; i+=8) tft.fillCircle(cx+i, cy-15, 2, COL_BG);
  tft.fillRoundRect(cx-20, cy-13, 14, 28, 3, c);
  tft.fillRoundRect(cx+6,  cy-13, 14, 28, 3, c);
  tft.fillRect(cx-5, cy-13, 10, 28, COL_BG);
}
void drawDrinksIcon(int cx, int cy, uint16_t c) {
  tft.fillRoundRect(cx-9, cy-8, 18, 18, 3, c);
  tft.fillRoundRect(cx-13, cy+10, 26, 5, 2, c);
  tft.drawCircle(cx+12, cy+1, 5, c);
  tft.drawFastVLine(cx-3, cy-14, 5, c);
  tft.drawFastVLine(cx+3, cy-16, 5, c);
}

typedef void (*IconFn)(int,int,uint16_t);
IconFn zoneFns[NUM_ZONES] = {
  drawToiletIcon, drawWardrobeIcon, drawDeskIcon,
  drawBedIcon, drawCurtainIcon, drawDrinksIcon
};

// ── ITEM ICONS ──────────────────────────────────────────────
void iconGeneric(int cx,int cy,uint16_t c){tft.drawRoundRect(cx-8,cy-8,16,16,3,c);tft.fillCircle(cx,cy,3,c);}
void iconToilet(int cx,int cy,uint16_t c){tft.fillRoundRect(cx-6,cy-10,12,6,2,c);tft.fillEllipse(cx,cy+1,8,6,c);tft.fillEllipse(cx,cy,5,4,COL_BG);tft.fillRect(cx-4,cy-4,8,5,c);tft.fillRoundRect(cx-7,cy+6,14,3,1,c);}
void iconSink(int cx,int cy,uint16_t c){tft.fillEllipse(cx,cy+3,9,6,c);tft.fillEllipse(cx,cy+4,5,4,COL_BG);tft.fillRect(cx-2,cy-6,4,10,c);tft.fillRect(cx-5,cy-7,10,3,c);}
void iconSoap(int cx,int cy,uint16_t c){tft.fillRoundRect(cx-8,cy-4,16,10,4,c);tft.fillCircle(cx-2,cy-1,2,COL_BG);tft.fillCircle(cx+3,cy+2,1,COL_BG);}
void iconTissues(int cx,int cy,uint16_t c){tft.fillRoundRect(cx-8,cy-6,16,14,3,c);tft.fillRoundRect(cx-4,cy-10,8,7,4,c);}
void iconAmenities(int cx,int cy,uint16_t c){tft.fillRoundRect(cx-4,cy-2,8,12,2,c);tft.fillRoundRect(cx-3,cy-6,6,5,2,c);tft.fillRoundRect(cx-2,cy-10,4,5,2,c);}
void iconMirror(int cx,int cy,uint16_t c){tft.fillRoundRect(cx-8,cy-10,16,16,4,c);tft.fillRoundRect(cx-5,cy-7,10,10,2,COL_BG);tft.fillRect(cx-2,cy+6,4,5,c);tft.fillRect(cx-5,cy+10,10,2,c);}
void iconShower(int cx,int cy,uint16_t c){tft.fillCircle(cx,cy-6,5,c);tft.fillRect(cx-1,cy-1,3,8,c);for(int dx=-4;dx<=4;dx+=2)tft.drawFastVLine(cx+dx,cy+8,4,c);}
void iconBathtub(int cx,int cy,uint16_t c){tft.fillRoundRect(cx-10,cy-2,20,10,3,c);tft.fillRect(cx-8,cy-8,5,8,c);tft.fillRect(cx-10,cy+8,5,4,c);tft.fillRect(cx+5,cy+8,5,4,c);tft.fillRect(cx-4,cy-9,5,4,c);}
void iconTowels(int cx,int cy,uint16_t c){tft.fillRoundRect(cx-9,cy-5,18,12,3,c);tft.drawFastHLine(cx-9,cy-1,18,COL_BG);tft.drawFastHLine(cx-9,cy+3,18,COL_BG);}
void iconFloor(int cx,int cy,uint16_t c){tft.drawRoundRect(cx-9,cy-9,18,18,2,c);tft.drawFastVLine(cx,cy-9,18,c);tft.drawFastHLine(cx-9,cy,18,c);}
void iconHangers(int cx,int cy,uint16_t c){tft.fillCircle(cx,cy-8,3,c);tft.fillRect(cx-1,cy-5,2,4,c);tft.drawLine(cx,cy-1,cx-11,cy+8,c);tft.drawLine(cx,cy-1,cx+11,cy+8,c);tft.drawFastHLine(cx-11,cy+8,22,c);}
void iconIron(int cx,int cy,uint16_t c){tft.fillRoundRect(cx-10,cy+1,20,8,3,c);tft.fillTriangle(cx+10,cy+1,cx+10,cy+9,cx+16,cy+5,c);tft.fillRoundRect(cx-4,cy-7,8,9,2,c);}
void iconSafe(int cx,int cy,uint16_t c){tft.fillRoundRect(cx-10,cy-9,20,18,2,c);tft.fillCircle(cx-1,cy,4,COL_BG);tft.fillCircle(cx-1,cy,2,c);tft.drawFastHLine(cx+3,cy,5,COL_BG);}
void iconLaundry(int cx,int cy,uint16_t c){tft.fillRoundRect(cx-9,cy-10,18,20,3,c);tft.drawCircle(cx,cy+2,5,COL_BG);tft.fillCircle(cx-3,cy-1,1,COL_BG);}
void iconSlippers(int cx,int cy,uint16_t c){tft.fillEllipse(cx-5,cy+3,6,4,c);tft.fillEllipse(cx+5,cy+3,6,4,c);tft.fillRoundRect(cx-9,cy-4,6,8,3,c);tft.fillRoundRect(cx+3,cy-4,6,8,3,c);}
void iconThermostat(int cx,int cy,uint16_t c){tft.fillCircle(cx,cy,9,c);tft.fillCircle(cx,cy,5,COL_BG);tft.fillRect(cx-1,cy-10,3,6,c);tft.fillRect(cx+5,cy-8,3,3,c);tft.fillRect(cx-8,cy-6,3,3,c);}
void iconPlugs(int cx,int cy,uint16_t c){tft.fillRoundRect(cx-8,cy-6,16,12,3,c);tft.fillRect(cx-4,cy-11,3,6,c);tft.fillRect(cx+1,cy-11,3,6,c);tft.fillRect(cx-1,cy+6,3,6,c);}
void iconDecor(int cx,int cy,uint16_t c){tft.fillCircle(cx,cy-4,6,c);tft.fillRect(cx-2,cy+2,5,8,c);tft.fillRoundRect(cx-6,cy+9,12,3,1,c);}
void iconDrawers(int cx,int cy,uint16_t c){tft.fillRoundRect(cx-10,cy-10,20,20,2,c);tft.drawFastHLine(cx-10,cy-2,20,COL_BG);tft.drawFastHLine(cx-10,cy+5,20,COL_BG);tft.fillRect(cx-2,cy-7,4,4,COL_BG);tft.fillRect(cx-2,cy+1,4,4,COL_BG);}
void iconChair(int cx,int cy,uint16_t c){tft.fillRoundRect(cx-10,cy-12,20,9,3,c);tft.fillRoundRect(cx-10,cy-3,20,8,2,c);tft.fillRect(cx-9,cy+5,4,7,c);tft.fillRect(cx+5,cy+5,4,7,c);tft.fillRect(cx-10,cy-12,4,20,c);}
void iconBedItem(int cx,int cy,uint16_t c){tft.fillRoundRect(cx-10,cy-8,20,6,2,c);tft.fillRoundRect(cx-10,cy-2,20,12,2,c);tft.fillRoundRect(cx-8,cy-7,8,6,2,COL_BG);tft.fillRoundRect(cx+1,cy-7,7,6,2,COL_BG);}
void iconTable(int cx,int cy,uint16_t c){tft.fillRoundRect(cx-10,cy-6,20,4,1,c);tft.fillRect(cx-8,cy-2,4,12,c);tft.fillRect(cx+4,cy-2,4,12,c);tft.fillRect(cx-8,cy+8,20,3,c);}
void iconLighting(int cx,int cy,uint16_t c){tft.fillCircle(cx,cy-3,6,c);tft.fillTriangle(cx-7,cy+3,cx+7,cy+3,cx,cy+11,c);tft.fillRect(cx-1,cy-11,3,6,c);}
void iconControls(int cx,int cy,uint16_t c){tft.fillRoundRect(cx-10,cy-8,20,16,3,c);tft.fillCircle(cx-4,cy+1,3,COL_BG);tft.fillCircle(cx+4,cy+1,3,COL_BG);tft.drawFastHLine(cx-8,cy-3,16,COL_BG);}
void iconWritingPad(int cx,int cy,uint16_t c){tft.fillRoundRect(cx-8,cy-10,16,20,2,c);tft.fillRect(cx-8,cy-10,4,20,COL_BG);for(int dy=-5;dy<=5;dy+=4)tft.drawFastHLine(cx-2,cy+dy,8,COL_BG);}
void iconSeating(int cx,int cy,uint16_t c){tft.fillRoundRect(cx-10,cy-10,20,9,3,c);tft.fillRoundRect(cx-8,cy-1,16,8,2,c);tft.fillRect(cx-8,cy+7,4,5,c);tft.fillRect(cx+4,cy+7,4,5,c);}
void iconWindow(int cx,int cy,uint16_t c){tft.drawRoundRect(cx-10,cy-10,20,20,2,c);tft.drawFastVLine(cx,cy-10,20,c);tft.drawFastHLine(cx-10,cy,20,c);tft.fillRect(cx-9,cy-9,8,8,tft.color565(100,160,220));tft.fillRect(cx+2,cy-9,7,8,tft.color565(100,160,220));}
void iconTV(int cx,int cy,uint16_t c){tft.fillRoundRect(cx-11,cy-9,22,14,3,c);tft.fillRoundRect(cx-8,cy-6,16,8,2,tft.color565(60,120,180));tft.fillRect(cx-3,cy+5,6,5,c);tft.fillRect(cx-7,cy+9,14,3,c);}
void iconCatalogue(int cx,int cy,uint16_t c){tft.fillRoundRect(cx-8,cy-10,16,20,2,c);tft.fillRect(cx-8,cy-10,4,20,tft.color565(80,80,80));for(int dy=-5;dy<=5;dy+=5)tft.drawFastHLine(cx-2,cy+dy,8,COL_BG);}
void iconCoffee(int cx,int cy,uint16_t c){tft.fillRoundRect(cx-8,cy-5,16,14,3,c);tft.fillRoundRect(cx-11,cy+9,22,4,2,c);tft.drawCircle(cx+11,cy+2,4,c);for(int i=-1;i<=1;i++)tft.drawFastVLine(cx+i*3,cy-10,4,c);}
void iconRefreshments(int cx,int cy,uint16_t c){tft.fillRoundRect(cx-6,cy-9,12,18,3,c);tft.fillRoundRect(cx-4,cy-12,8,6,2,c);tft.drawFastHLine(cx-6,cy-3,12,COL_BG);tft.drawFastHLine(cx-6,cy+3,12,COL_BG);}
void iconKettle(int cx,int cy,uint16_t c){tft.fillEllipse(cx-1,cy+2,10,9,c);tft.fillRoundRect(cx-4,cy-9,8,10,2,c);tft.drawLine(cx+9,cy-2,cx+14,cy-5,c);tft.drawLine(cx+9,cy+3,cx+14,cy+5,c);}
void iconFridge(int cx,int cy,uint16_t c){tft.fillRoundRect(cx-8,cy-11,16,22,3,c);tft.drawFastHLine(cx-8,cy-1,16,COL_BG);tft.fillRect(cx+3,cy-9,3,6,COL_BG);tft.fillRect(cx+3,cy+2,3,5,COL_BG);}
void iconCounter(int cx,int cy,uint16_t c){tft.fillRoundRect(cx-11,cy-4,22,6,2,c);tft.fillRect(cx-9,cy+2,18,8,c);tft.fillRect(cx-11,cy+9,22,3,c);tft.fillCircle(cx-3,cy-2,2,COL_BG);tft.fillCircle(cx+4,cy-2,2,COL_BG);}

typedef void (*ItemIconFn)(int,int,uint16_t);
ItemIconFn itemIcons[NUM_ZONES][MAX_ITEMS] = {
  {iconToilet,iconSink,iconSoap,iconTissues,iconAmenities,iconMirror,iconShower,iconBathtub,iconTowels,iconFloor},
  {iconHangers,iconIron,iconSafe,iconLaundry,iconSlippers,iconGeneric,iconGeneric,iconGeneric,iconGeneric,iconGeneric},
  {iconThermostat,iconMirror,iconPlugs,iconDecor,iconDrawers,iconChair,iconGeneric,iconGeneric,iconGeneric,iconGeneric},
  {iconBedItem,iconTable,iconDrawers,iconLighting,iconControls,iconWritingPad,iconGeneric,iconGeneric,iconGeneric,iconGeneric},
  {iconSeating,iconWindow,iconTV,iconTable,iconDrawers,iconCatalogue,iconDecor,iconPlugs,iconGeneric,iconGeneric},
  {iconCoffee,iconRefreshments,iconKettle,iconFridge,iconCounter,iconFloor,iconGeneric,iconGeneric,iconGeneric,iconGeneric},
};

// ── SCROLLBAR ────────────────────────────────────────────────
void drawScrollbar(int count, int offset) {
  int sbX  = PILL_X1 + PILL_W + 6 + PILL_W + 4;
  int sbY  = PILL_Y0;
  int sbH  = 320 - BORDER_W - PILL_Y0 - 4;
  tft.fillRoundRect(sbX, sbY, SCROLL_W, sbH, 3, COL_SCROLLBAR);
  if(count <= PILLS_VISIBLE) return;
  int thumbH = max(20, sbH * PILLS_VISIBLE / count);
  int thumbY = sbY + (sbH - thumbH) * offset / (count - PILLS_VISIBLE);
  tft.fillRoundRect(sbX, thumbY, SCROLL_W, thumbH, 3, COL_SCROLLTHUMB);
}



// ── SCAN ROOM SCREEN ─────────────────────────────────────────
void drawScanRoomScreen() {
  tft.fillScreen(COL_BG);
  drawHeader("KUPPI", "v4", "", false);
  drawTimerBorder();

  int cx = 240, cy = 120;

  // NFC icon (large circle)
  tft.fillCircle(cx, cy, 52, COL_PILL_OFF);
  tft.fillCircle(cx, cy, 46, COL_BG);
  // NFC signal arcs
  for(int r = 18; r <= 38; r += 10) {
    tft.drawCircle(cx, cy, r, COL_FADED);
  }
  // Center dot
  tft.fillCircle(cx, cy, 6, COL_FADED);

  // Text
  tft.setTextColor(COL_WHITE);
  tft.setTextSize(3);
  const char* line1 = "Scan Room Tag";
  int w1 = strlen(line1) * 18;
  tft.setCursor((480 - w1) / 2, cy + 65);
  tft.print(line1);

  tft.setTextColor(COL_FADED);
  tft.setTextSize(2);
  const char* line2 = "Hold device near door NFC";
  int w2 = strlen(line2) * 12;
  tft.setCursor((480 - w2) / 2, cy + 100);
  tft.print(line2);
}

void drawRoomFoundScreen() {
  tft.fillScreen(COL_BG);
  drawHeader("KUPPI", "v4", "", false);
  drawTimerBorder();

  int cx = 240, cy = 120;

  // Green check circle
  tft.fillCircle(cx, cy, 48, COL_GREEN);
  tft.fillCircle(cx, cy, 42, COL_GREEN_DARK);
  // Check mark
  for(int t = -2; t <= 2; t++) {
    tft.drawLine(cx - 20, cy + t, cx - 6, cy + 14 + t, COL_WHITE);
    tft.drawLine(cx - 6, cy + 14 + t, cx + 22, cy - 16 + t, COL_WHITE);
  }

  // Room number
  tft.setTextColor(COL_WHITE);
  tft.setTextSize(3);
  char roomLabel[32];
  snprintf(roomLabel, sizeof(roomLabel), "Room %s", activeRoom);
  int w1 = strlen(roomLabel) * 18;
  tft.setCursor((480 - w1) / 2, cy + 60);
  tft.print(roomLabel);

  tft.setTextColor(COL_FADED);
  tft.setTextSize(2);
  const char* line2 = "Starting session...";
  int w2 = strlen(line2) * 12;
  tft.setCursor((480 - w2) / 2, cy + 95);
  tft.print(line2);
}

void drawRoomNotFoundScreen() {
  tft.fillScreen(COL_BG);
  drawHeader("KUPPI", "v4", "", false);
  drawTimerBorder();

  int cx = 240, cy = 130;

  // Red X circle
  tft.fillCircle(cx, cy, 48, COL_RED);
  tft.fillCircle(cx, cy, 42, COL_RED_DARK);
  for(int t = -2; t <= 2; t++) {
    tft.drawLine(cx - 16, cy - 16 + t, cx + 16, cy + 16 + t, COL_WHITE);
    tft.drawLine(cx + 16, cy - 16 + t, cx - 16, cy + 16 + t, COL_WHITE);
  }

  tft.setTextColor(COL_RED);
  tft.setTextSize(3);
  const char* line1 = "Room Not Found";
  int w1 = strlen(line1) * 18;
  tft.setCursor((480 - w1) / 2, cy + 60);
  tft.print(line1);

  tft.setTextColor(COL_FADED);
  tft.setTextSize(2);
  const char* line2 = "Try a different tag";
  int w2 = strlen(line2) * 12;
  tft.setCursor((480 - w2) / 2, cy + 95);
  tft.print(line2);
}

void drawRoomBlockedScreen() {
  tft.fillScreen(COL_BG);
  drawHeader("KUPPI", "v4", "", false);
  drawTimerBorder();

  int cx = 240, cy = 110;

  // Orange/yellow warning circle
  uint16_t COL_WARN = tft.color565(255, 180, 0);
  uint16_t COL_WARN_DARK = tft.color565(180, 120, 0);
  tft.fillCircle(cx, cy, 48, COL_WARN);
  tft.fillCircle(cx, cy, 42, COL_WARN_DARK);

  // Exclamation mark
  tft.fillRoundRect(cx - 4, cy - 22, 8, 28, 3, COL_WHITE);
  tft.fillCircle(cx, cy + 14, 5, COL_WHITE);

  // Room number
  tft.setTextColor(COL_WARN);
  tft.setTextSize(3);
  char roomLabel[32];
  snprintf(roomLabel, sizeof(roomLabel), "Room %s", activeRoom);
  int w1 = strlen(roomLabel) * 18;
  tft.setCursor((480 - w1) / 2, cy + 58);
  tft.print(roomLabel);

  tft.setTextColor(COL_WHITE);
  tft.setTextSize(2);
  const char* line2 = "Already cleaned";
  int w2 = strlen(line2) * 12;
  tft.setCursor((480 - w2) / 2, cy + 92);
  tft.print(line2);

  tft.setTextColor(COL_FADED);
  tft.setTextSize(2);
  const char* line3 = "Awaiting supervisor approval";
  int w3 = strlen(line3) * 12;
  tft.setCursor((480 - w3) / 2, cy + 120);
  tft.print(line3);
}

// ── STAFF LOGIN SCREENS ──────────────────────────────────────
void drawStaffLoginScreen() {
  tft.fillScreen(COL_BG);
  drawHeader("KUPPI", "v4", "", false);
  drawTimerBorder();

  int cx = 240, cy = 115;

  // Person icon inside a circle
  tft.fillCircle(cx, cy, 52, COL_PILL_OFF);
  tft.fillCircle(cx, cy, 46, COL_BG);
  tft.fillCircle(cx, cy - 12, 14, COL_FADED);
  tft.fillEllipse(cx, cy + 22, 22, 14, COL_FADED);

  tft.setTextColor(COL_WHITE);
  tft.setTextSize(3);
  const char* line1 = "Scan Staff Card";
  int w1 = strlen(line1) * 18;
  tft.setCursor((480 - w1) / 2, cy + 65);
  tft.print(line1);

  tft.setTextColor(COL_FADED);
  tft.setTextSize(2);
  const char* line2 = "Tap your personal NFC card";
  int w2 = strlen(line2) * 12;
  tft.setCursor((480 - w2) / 2, cy + 100);
  tft.print(line2);
}

void drawStaffFoundScreen() {
  tft.fillScreen(COL_BG);
  drawHeader("KUPPI", "v4", "", false);

  int cx = 240, cy = 110;

  // Green check circle
  tft.fillCircle(cx, cy, 48, COL_GREEN);
  tft.fillCircle(cx, cy, 42, COL_GREEN_DARK);
  for(int t = -2; t <= 2; t++) {
    tft.drawLine(cx - 20, cy + t, cx - 6, cy + 14 + t, COL_WHITE);
    tft.drawLine(cx - 6, cy + 14 + t, cx + 22, cy - 16 + t, COL_WHITE);
  }

  tft.setTextColor(COL_WHITE);
  tft.setTextSize(3);
  char welcomeMsg[48];
  snprintf(welcomeMsg, sizeof(welcomeMsg), "Hello, %s!", staffName);
  int w1 = strlen(welcomeMsg) * 18;
  if (w1 > 460) {
    // Name too long — fall back to smaller text
    tft.setTextSize(2);
    w1 = strlen(welcomeMsg) * 12;
  }
  tft.setCursor((480 - w1) / 2, cy + 60);
  tft.print(welcomeMsg);

  tft.setTextColor(COL_FADED);
  tft.setTextSize(2);
  const char* line2 = "Scan room tag to begin...";
  int w2 = strlen(line2) * 12;
  tft.setCursor((480 - w2) / 2, cy + 96);
  tft.print(line2);
}

void drawStaffNotFoundScreen() {
  tft.fillScreen(COL_BG);
  drawHeader("KUPPI", "v4", "", false);

  int cx = 240, cy = 120;

  // Red X circle
  tft.fillCircle(cx, cy, 48, COL_RED);
  tft.fillCircle(cx, cy, 42, COL_RED_DARK);
  for(int t = -2; t <= 2; t++) {
    tft.drawLine(cx - 16, cy - 16 + t, cx + 16, cy + 16 + t, COL_WHITE);
    tft.drawLine(cx + 16, cy - 16 + t, cx - 16, cy + 16 + t, COL_WHITE);
  }

  tft.setTextColor(COL_RED);
  tft.setTextSize(3);
  const char* line1 = "Card Not Found";
  int w1 = strlen(line1) * 18;
  tft.setCursor((480 - w1) / 2, cy + 60);
  tft.print(line1);

  tft.setTextColor(COL_FADED);
  tft.setTextSize(2);
  const char* line2 = "Register card with supervisor";
  int w2 = strlen(line2) * 12;
  tft.setCursor((480 - w2) / 2, cy + 95);
  tft.print(line2);
}

// ── HOME SCREEN ─────────────────────────────────────────────
void drawHomeScreen() {
  tft.fillScreen(COL_BG);
  char roomHeader[20];
  snprintf(roomHeader, sizeof(roomHeader), "Room %s", activeRoom);
  drawHeader(roomHeader, "KUPPI", "", false);
  drawTimerBorder();
  int r=52, startX=80, startY=100, gapX=160, gapY=128;
  for(int i=0;i<NUM_ZONES;i++){
    int col=i%3, row=i/3;
    int cx=startX+col*gapX, cy=startY+row*gapY;
    uint16_t outer=zoneCompleted[i]?COL_GREEN:COL_RED;
    uint16_t inner=zoneCompleted[i]?COL_GREEN_DARK:COL_RED_DARK;
    tft.fillCircle(cx,cy,r,outer);
    tft.fillCircle(cx,cy,r-6,inner);
    zoneFns[i](cx,cy-4,COL_WHITE);
  }

}

// ── CHECKLIST SCREEN ─────────────────────────────────────────
void drawPill(int zoneIdx, int itemIdx, int px, int py) {
  bool checked  = itemChecked[zoneIdx][itemIdx];
  uint16_t bg   = checked ? COL_PILL_ON  : COL_PILL_OFF;
  uint16_t icbg = checked ? COL_ICON_ON  : COL_ICON_OFF;
  uint16_t tc   = checked ? COL_FADED    : COL_WHITE;
  tft.fillRoundRect(px, py, PILL_W, PILL_H, PILL_H/2, bg);
  int icx = px + PILL_H/2;
  int icy = py + PILL_H/2;
  tft.fillCircle(icx, icy, PILL_H/2-4, icbg);
  itemIcons[zoneIdx][itemIdx](icx, icy, COL_WHITE);
  tft.setTextColor(tc);
  tft.setTextSize(2);
  tft.setCursor(px + PILL_H + 4, py + PILL_H/2 - 8);
  tft.print(zoneItems[zoneIdx][itemIdx]);
}

void drawChecklistScreen(int zoneIdx) {
  tft.fillScreen(COL_BG);
  int count = zoneItemCount[zoneIdx];
  int done  = 0;
  for(int i=0;i<count;i++) if(itemChecked[zoneIdx][i]) done++;
  bool allDone = (done == count);
  char prog[12];
  snprintf(prog, sizeof(prog), "%d/%d", done, count);
  char roomHdr[20];
  snprintf(roomHdr, sizeof(roomHdr), "Room %s", activeRoom);
  drawHeader(roomHdr, "KUPPI", prog, allDone);
  drawTimerBorder();
  int rows = (count + 1) / 2;
  int visRows = min(PILLS_VISIBLE, rows - scrollOffset);
  for(int row = 0; row < visRows; row++){
    int py = PILL_Y0 + row * (PILL_H + PILL_GAP);
    int i0 = (scrollOffset + row) * 2;
    int i1 = i0 + 1;
    if(i0 < count && strlen(zoneItems[zoneIdx][i0]) > 0)
      drawPill(zoneIdx, i0, PILL_X1, py);
    if(i1 < count && strlen(zoneItems[zoneIdx][i1]) > 0)
      drawPill(zoneIdx, i1, PILL_X2, py);
  }
  drawScrollbar(rows, scrollOffset);
  int icx = 480 - BORDER_W - ZONE_COL_W/2;
  int icy  = HEADER_H + 30;
  uint16_t outer = allDone ? COL_GREEN : COL_RED;
  uint16_t inner = allDone ? COL_GREEN_DARK : COL_RED_DARK;
  tft.fillCircle(icx, icy, 26, outer);
  tft.fillCircle(icx, icy, 20, inner);
  zoneFns[zoneIdx](icx, icy-2, COL_WHITE);
  if(allDone){
    int bx = 480 - BORDER_W - ZONE_COL_W/2;
    int by = 320 - BORDER_W - 30;
    tft.drawCircle(bx, by, 22, COL_WHITE);
    tft.drawCircle(bx, by, 21, COL_WHITE);
    tft.fillTriangle(bx-9,by-9, bx-9,by+9, bx+11,by, COL_WHITE);
  }
}

// ── COMPLETE SCREEN ──────────────────────────────────────────
void drawCompleteScreen() {
  tft.fillScreen(COL_BG);
  char roomHdrC[20];
  snprintf(roomHdrC, sizeof(roomHdrC), "Room %s", activeRoom);
  drawHeader(roomHdrC, "KUPPI", "", false);
  int cx=240, cy=150;
  tft.fillCircle(cx,cy,72,COL_GREEN);
  tft.fillCircle(cx,cy,66,COL_GREEN_DARK);
  for(int t=-2;t<=2;t++){
    tft.drawLine(cx-32,cy+t,   cx-10,cy+22+t, COL_WHITE);
    tft.drawLine(cx-10,cy+22+t,cx+36,cy-24+t, COL_WHITE);
  }
  tft.setTextColor(COL_WHITE);
  tft.setTextSize(2);
  tft.setCursor(150, 244);
  tft.print("Room Ready!");
  drawTimerBorder();
}

// ── NFC HELPERS ──────────────────────────────────────────────

// Sends the PN532 InRelease command to deselect the active tag.
// Without this, subsequent readPassiveTargetID calls can hang the I2C bus
// because the PN532 still has the previous card selected.
// Must be called while holding the i2cMutex.
void nfcReleaseTarget() {
  uint8_t cmd[] = { PN532_COMMAND_INRELEASE, 0x01 };
  if (nfc.sendCommandCheckAck(cmd, sizeof(cmd), 100)) {
    // readDetectedPassiveTargetID is a public method that calls the private
    // readdata internally — we use it here only to flush the InRelease
    // response bytes from the PN532's buffer so the bus is clean for the
    // next readPassiveTargetID call. The return value is intentionally ignored.
    uint8_t uid[7];
    uint8_t uidLen = 0;
    nfc.readDetectedPassiveTargetID(uid, &uidLen);
  }
}

bool uidMatch(uint8_t* uid, uint8_t* target) {
  for(int i=0;i<4;i++) if(uid[i+1]!=target[i]) return false;
  return true;
}
int identifyZone(uint8_t* uid) {
  for(int i=0;i<NUM_ZONES;i++) if(uidMatch(uid,zoneUIDs[i])) return i;
  return -1;
}
bool allZonesComplete() {
  for(int i=0;i<NUM_ZONES;i++) if(!zoneCompleted[i]) return false;
  return true;
}
void resetAll() {
  for(int i=0;i<NUM_ZONES;i++){
    zoneCompleted[i]=false;
    for(int j=0;j<MAX_ITEMS;j++) itemChecked[i][j]=false;
  }
  currentScreen   = SCREEN_STAFF_LOGIN;
  activeZone      = -1;
  scrollOffset    = 0;
  timerRunning    = false;
  roomIdentified  = false;
  activeRoom[0]   = '\0';
  // Clear staff session
  staffLoggedIn       = false;
  staffName[0]        = '\0';
  staffApiId[0]       = '\0';
  staffLookupDone     = false;
  staffLookupSuccess  = false;
}

// ── TOUCH HANDLING ───────────────────────────────────────────

void handleChecklistTouch(int tx, int ty) {
  if(activeZone < 0) return;
  int count = zoneItemCount[activeZone];
  int rows  = (count + 1) / 2;

  int sbX = PILL_X1 + PILL_W + 6 + PILL_W + 4;
  int sbY = PILL_Y0;
  int sbH = 320 - BORDER_W - PILL_Y0 - 4;
  if(tx >= sbX - 8 && tx <= sbX + SCROLL_W + 8 && ty >= sbY && ty <= sbY + sbH){
    int newOffset = (ty - sbY) * (rows - PILLS_VISIBLE) / sbH;
    newOffset = max(0, min(newOffset, rows - PILLS_VISIBLE));
    if(newOffset != scrollOffset){
      scrollOffset = newOffset;
      drawChecklistScreen(activeZone);
      delay(250);
    }
    return;
  }

  int visRows = min(PILLS_VISIBLE, rows - scrollOffset);
  for(int row = 0; row < visRows; row++){
    int py = PILL_Y0 + row * (PILL_H + PILL_GAP);
    if(ty < py || ty > py + PILL_H) continue;
    int i0 = (scrollOffset + row) * 2;
    if(tx >= PILL_X1 && tx <= PILL_X1 + PILL_W && i0 < count && strlen(zoneItems[activeZone][i0]) > 0){
      itemChecked[activeZone][i0] = !itemChecked[activeZone][i0];
      Serial.print("[TOUCH] Toggled: "); Serial.println(zoneItems[activeZone][i0]);
      buzzOnce();
      drawChecklistScreen(activeZone);
      delay(250);
      return;
    }
    int i1 = i0 + 1;
    if(tx >= PILL_X2 && tx <= PILL_X2 + PILL_W && i1 < count && strlen(zoneItems[activeZone][i1]) > 0){
      itemChecked[activeZone][i1] = !itemChecked[activeZone][i1];
      Serial.print("[TOUCH] Toggled: "); Serial.println(zoneItems[activeZone][i1]);
      buzzOnce();
      drawChecklistScreen(activeZone);
      delay(250);
      return;
    }
  }

  int done=0;
  for(int i=0;i<count;i++) if(itemChecked[activeZone][i]) done++;
  if(done == count){
    int bx = 480 - BORDER_W - ZONE_COL_W/2;
    int by = 320 - BORDER_W - 30;
    int dx = tx-bx, dy = ty-by;
    if(dx*dx + dy*dy <= 26*26){
      Serial.println("[TOUCH] Zone complete");
      zoneCompleted[activeZone] = true;
      pendingScanZone = activeZone;
      buzzComplete();
      activeZone    = -1;
      scrollOffset  = 0;
      currentScreen = SCREEN_HOME;
      if(allZonesComplete()){
        buzzAllDone();
        pendingSessionClose = true;
        drawCompleteScreen();
        currentScreen = SCREEN_COMPLETE;
      } else {
        drawHomeScreen();
      }
    }
  }
}

// ── SETUP ────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);
  i2cMutex = xSemaphoreCreateMutex();
  Serial.println("=== KUPPI V4 + SUPABASE + FREERTOS ===");

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  Wire.begin(21, 22);
  Wire.setClock(100000);
  delay(100);

  tft.init();
  tft.setRotation(1);
  tft.invertDisplay(true);
  tft.fillScreen(COL_BG);
  tft.setTextColor(COL_WHITE);
  tft.setTextSize(2);
  tft.setCursor(120, 120);
  tft.print("KUPPI starting...");

  nfc.begin();
  uint32_t ver = nfc.getFirmwareVersion();
  if(!ver){
    tft.fillScreen(tft.color565(100,0,0));
    tft.setCursor(40,130); tft.setTextSize(2); tft.print("NFC not found!");
    tft.setCursor(40,160); tft.setTextSize(1); tft.print("Power cycle: unplug USB 10s");
    Serial.println("ERROR: PN532 not found");
    while(1) delay(1000);
  }
  Serial.print("[NFC] Firmware: ");
  Serial.print((ver>>16)&0xFF); Serial.print("."); Serial.println((ver>>8)&0xFF);
  nfc.SAMConfig();
  Serial.println("[NFC] Ready");

  tft.setTextSize(1);
  tft.setCursor(120, 150);
  tft.print("Connecting to Wi-Fi...");
  bool wifiOK = connectWifi();
  if(wifiOK){
    tft.setTextColor(COL_GREEN);
    tft.setCursor(120, 165);
    tft.print("Wi-Fi connected!");
    Serial.println("[WIFI] Ready");
    delay(300);
  } else {
    tft.setTextColor(COL_WIFI_FAIL);
    tft.setCursor(120, 165);
    tft.print("Wi-Fi offline — local only");
  }

  // HTTP task on Core 0, priority 0 (lower than touch)
  xTaskCreatePinnedToCore(httpTask, "httpTask", 8192, NULL, 0, NULL, 0);
  Serial.println("[HTTP] Background task started on Core 0");

  delay(800);
  currentScreen = SCREEN_STAFF_LOGIN;
  drawStaffLoginScreen();
  Serial.println("[KUPPI] Ready - scan staff card to log in");
}

// ── LOOP (Core 1) ────────────────────────────────────────────
bool lastPressed         = false;
unsigned long lastTouchTime  = 0;
unsigned long lastNFCTime    = 0;
unsigned long lastBorderTime = 0;

void loop() {

  // Touch — 200ms debounce
  if(millis() - lastTouchTime > 200){
    TouchPoint tp = readTouch();
    if(tp.pressed && !lastPressed){
      lastTouchTime = millis();
      lastPressed   = true;
      if     (currentScreen == SCREEN_CHECKLIST) handleChecklistTouch(tp.x, tp.y);
      else if(currentScreen == SCREEN_COMPLETE)  { resetAll(); drawScanRoomScreen(); }
      lastPressed   = false;
      lastTouchTime = millis();
    } else if(!tp.pressed){
      lastPressed   = false;
      lastTouchTime = millis();
    }
  }

  // ── NFC: STAFF LOGIN mode ────────────────────────────────
  if(currentScreen == SCREEN_STAFF_LOGIN && millis() - lastNFCTime > 300){
    lastNFCTime = millis();

    // Check if staff lookup just completed
    if(staffLookupDone) {
      staffLookupDone = false;
      if(staffLookupSuccess) {
        staffLoggedIn = true;
        buzzComplete();
        drawStaffFoundScreen();
        delay(1800);
        currentScreen = SCREEN_SCAN_ROOM;
        drawScanRoomScreen();
        Serial.print("[STAFF] Logged in: "); Serial.println(staffName);
      } else {
        buzzOnce();
        drawStaffNotFoundScreen();
        delay(2200);
        drawStaffLoginScreen();
      }
      return;
    }

    // Try to read an NFC card
    uint8_t uid[7];
    uint8_t uidLen = 0;
    bool found = false;

    if(xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      found = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 50);
      if (found) nfcReleaseTarget();
      xSemaphoreGive(i2cMutex);
    }

    if(found) {
      char hexUID[20] = "";
      for(int i = 0; i < uidLen && i < 7; i++) {
        char hex[4];
        snprintf(hex, sizeof(hex), "%02X", uid[i]);
        strcat(hexUID, hex);
      }
      Serial.print("[NFC] Staff card UID: "); Serial.println(hexUID);

      strncpy(staffLookupUID, hexUID, sizeof(staffLookupUID) - 1);
      staffLookupUID[sizeof(staffLookupUID) - 1] = '\0';
      staffLookupDone    = false;
      staffLookupSuccess = false;
      pendingStaffLookup = true;
      buzzOnce();

      // Show "Looking up..." on screen
      tft.fillScreen(COL_BG);
      drawHeader("KUPPI", "v4", "", false);
      tft.setTextColor(COL_WIFI_SEND);
      tft.setTextSize(2);
      const char* msg = "Verifying staff card...";
      int w = strlen(msg) * 12;
      tft.setCursor((480 - w) / 2, 150);
      tft.print(msg);
    }
  }

  // ── NFC: SCAN ROOM mode ───────────────────────────────────
  if(currentScreen == SCREEN_SCAN_ROOM && millis() - lastNFCTime > 300){
    lastNFCTime = millis();

    // Check if a room lookup just completed
    if(roomLookupDone) {
      roomLookupDone = false;
      if(roomLookupSuccess) {
        // Check if room is already cleaned (awaiting_approval or ready)
        if (strcmp(roomStatus, "awaiting_approval") == 0 || strcmp(roomStatus, "ready") == 0) {
          Serial.print("[ROOM] Blocked — room "); Serial.print(activeRoom);
          Serial.print(" is '"); Serial.print(roomStatus); Serial.println("'");
          buzzOnce();
          drawRoomBlockedScreen();
          delay(3000);
          // Reset ALL room/lookup state to prevent stale duplicate lookups
          roomIdentified     = false;
          activeRoom[0]      = '\0';
          roomStatus[0]      = '\0';
          roomLookupDone     = false;
          roomLookupSuccess  = false;
          pendingRoomLookup  = false;
          pendingSessionOpen = false;
          sessionOpenDone    = false;
          sessionOpenBlocked = false;
          currentScreen      = SCREEN_SCAN_ROOM;
          drawScanRoomScreen();
          return;
        }

        // Room found and available! Show success then go to home screen
        buzzComplete();
        drawRoomFoundScreen();
        delay(1500);
        // Open session and start timer
        pendingSessionOpen = true;
        kuppiTimerStart = millis();
        timerRunning = true;
        currentScreen = SCREEN_HOME;
        drawHomeScreen();
        Serial.print("[ROOM] Identified room: "); Serial.println(activeRoom);
      } else {
        // Room not found
        buzzOnce();
        drawRoomNotFoundScreen();
        delay(2000);
        drawScanRoomScreen();
      }
      return;
    }

    // Try to read an NFC tag
    uint8_t uid[7];
    uint8_t uidLen = 0;
    bool found = false;

    if(xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      found = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 50);
      if (found) nfcReleaseTarget();
      xSemaphoreGive(i2cMutex);
    }

    if(found){
      // Build hex string from UID
      char hexUID[20] = "";
      for(int i = 0; i < uidLen && i < 7; i++) {
        char hex[4];
        snprintf(hex, sizeof(hex), "%02X", uid[i]);
        strcat(hexUID, hex);
      }
      Serial.print("[NFC] Room tag UID: "); Serial.println(hexUID);

      // Skip if it's a known zone tag
      int zone = identifyZone(uid);
      if(zone >= 0) {
        Serial.println("[NFC] This is a zone tag, not a room tag — ignoring");
        return;
      }

      // Send to server for room lookup
      strncpy(roomLookupUID, hexUID, sizeof(roomLookupUID) - 1);
      roomLookupDone = false;
      roomLookupSuccess = false;
      pendingRoomLookup = true;
      buzzOnce();

      // Show "Looking up..." on screen
      tft.fillScreen(COL_BG);
      drawHeader("KUPPI", "v4", "", false);
      tft.setTextColor(COL_WIFI_SEND);
      tft.setTextSize(2);
      const char* msg = "Looking up room...";
      int w = strlen(msg) * 12;
      tft.setCursor((480 - w) / 2, 150);
      tft.print(msg);
    }
  }

  // ── NFC: ZONE SCAN mode (home screen) ─────────────────────
  if(currentScreen == SCREEN_HOME && millis() - lastNFCTime > 300){
    lastNFCTime = millis();
    uint8_t uid[7];
    uint8_t uidLen = 0;
    bool found = false;

    if(xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      found = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 50);
      if (found) nfcReleaseTarget();
      xSemaphoreGive(i2cMutex);
    }

    if(found){
      Serial.print("[NFC] UID: ");
      for(int i=0;i<uidLen;i++){
        if(uid[i]<0x10) Serial.print("0");
        Serial.print(uid[i],HEX); Serial.print(" ");
      }
      Serial.println();
      int zone = identifyZone(uid);
      if(zone >= 0){
        buzzOnce();
        activeZone    = zone;
        scrollOffset  = 0;
        currentScreen = SCREEN_CHECKLIST;
        drawChecklistScreen(zone);
        // Push lastNFCTime forward to prevent re-reading the same tag
        // if the user keeps it near the reader while on the checklist screen.
        lastNFCTime = millis();
        Serial.print("[NFC] Zone: "); Serial.println(zoneNames[zone]);
      } else {
        Serial.println("[NFC] Unknown tag (not a zone)");
      }
    }
  }
}