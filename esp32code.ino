#include <TFT_eSPI.h>
#include <ArduinoJson.h>   // v7.x
#include <mbedtls/md.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "esp_timer.h"
#include <math.h>

// user config
const char* NS_URL     = "NIGHT_SCOUT_URL_HERE"; // no trailing slash
const char* API_SECRET = "UNHASHED_NIGHTSCOUT_ACCESS_TOKEN";
const bool  USE_MMOL   = true;

const int   SCREEN_ROTATION = 2;     // 0 = portrait, 2 = portrait flipped 180°
const int   SENSOR_LIFE_H   = 360;   // guardian 4 = 7 days (libre 2/3 = 336, dexcom g6/g7 = 240)
const int   TZ_BASE_MIN     = 0;     // uk standard time (gmt)f
const bool  TZ_UK_DST       = true;  // auto bst (last sun mar -> last sun oct)
const bool  USE_NS_THRESHOLDS = false;
const bool  DEBUG_RAW       = false; // print raw payload start/end to serial

const uint8_t BACKLIGHT_LEVEL = 120; // 0-255, lower = dimmer

// alert limits (mg/dl internally)
int thrUrgentLow  = 55;
int thrLow        = 70;
int thrHigh       = 180;
int thrUrgentHigh = 250;

// hardware
#define TFT_BL_PIN 21
#define LED_RED    4
#define LED_GREEN  16
#define LED_BLUE   17
#define TOUCH_IRQ  36   // xpt2046 t_irq (low while touched) - tap anywhere to change graph range

// ble uuids
#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

// timing
const unsigned long POLL_INTERVAL_MS = 60000;
const unsigned long FIRST_POLL_DELAY = 2500;
const unsigned long RESPONSE_TIMEOUT = 25000;
const unsigned long RETRY_DELAY_MS   = 8000;
const size_t        RX_BUFFER_MAX    = 30000;
const int           BLE_CHUNK        = 20;

// colours (dark mode)
#define RGB565(r,g,b) ((uint16_t)((((r)&0xF8)<<8)|(((g)&0xFC)<<3)|((b)>>3)))
#define C_BG      0x0000
#define C_PANEL   0x18C3                 // dark slate
#define C_BAND    RGB565(8,40,34)
#define C_TEXT    0xCE79                 // muted light grey
#define C_BLACK   0x0000
#define C_DIM     0x8410                 // medium grey
#define C_GREY    0x2965                 // soft divider
#define C_ACCENT  RGB565(0,140,195)
#define C_OK      RGB565(50,185,125)
#define C_HIGH    0xDCA0                 // soft gold
#define C_WARN    RGB565(225,100,0)
#define C_LOW     RGB565(210,45,55)

// layout
const int SG_Y = 46;
const int GRAPH_Y = 132;
const int PLOT_X = 4, PLOT_W = 200, PLOT_Y = 146, PLOT_H = 90;
const int TILE_Y = 252;
const int STATUS_Y = 303;

// state
TFT_eSPI tft = TFT_eSPI();
BLECharacteristic *pTxCharacteristic = nullptr;
SemaphoreHandle_t rxMutex;

volatile bool deviceConnected = false;
volatile bool newDataAvailable = false;
String rxBuffer = "";

bool oldDeviceConnected = false;
bool waitingForResponse = false;
unsigned long requestSentAt = 0, nextPollAt = 0, lastTick = 0, lastTouchMs = 0;
String secretHash = "";
String lastPath = "";

enum Stage { ST_IDLE, ST_STATUS, ST_ENTRIES, ST_PROPS, ST_BOLUS };
Stage stage = ST_IDLE;
bool statusTried = false;
unsigned long lastStatusTryMs = 0;

// graph ranges (tap to cycle)
const int RANGE_H[3] = {1, 3, 6};
int rangeIdx = 1;
int requestedHours = 6, loadedHours = 0;

// readings (oldest -> newest)
#define MAX_PTS 370
struct Reading { uint32_t t; uint16_t sgv; };
Reading readings[MAX_PTS];
int nReadings = 0;
int curSgv = 0, deltaMg = 0;
bool haveReading = false, hasDelta = false;
String curDir = "";
uint32_t lastReadingEpoch = 0;
int64_t lastRxMs = 0;

// clock
bool clockSynced = false;
bool synthTime = false;       // true when timestamps are invented (no date field)
int64_t clockOffsetMs = 0;

// insulin / sensor info (mdi)
float iobU = NAN, cobG = NAN;
int   sensorAgeH = -1;
bool  haveBolus = false;
float lastBolusU = 0;
uint32_t lastBolusEpoch = 0;

// ui cache
int lastClockMin = -2;
bool lastStale = false;
const char* lastBannerText = (const char*)1;
String lastStatusText = "";
uint16_t lastStatusColor = 0;
String statusMsg = "";
uint16_t statusMsgColor = 0;
unsigned long statusMsgUntil = 0;

// utils
String sha1Hash(const String &payload) {
  byte shaResult[20];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), 0);
  mbedtls_md_starts(&ctx);
  mbedtls_md_update(&ctx, (const unsigned char *)payload.c_str(), payload.length());
  mbedtls_md_finish(&ctx, shaResult);
  mbedtls_md_free(&ctx);
  String h = "";
  for (int i = 0; i < 20; i++) { char s[3]; sprintf(s, "%02x", (int)shaResult[i]); h += s; }
  return h;
}

void setRGB(bool r, bool g, bool b) {
  digitalWrite(LED_RED,   r ? LOW : HIGH);
  digitalWrite(LED_GREEN, g ? LOW : HIGH);
  digitalWrite(LED_BLUE,  b ? LOW : HIGH);
}

// backlight: 0 = off, 255 = max (driven inverted, as on the working sketch)
void setBacklight(uint8_t brightness) {
  ledcWrite(TFT_BL_PIN, 255 - brightness);
}

int64_t nowMs() { return esp_timer_get_time() / 1000; }

// calendar helpers (uk dst)
long daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long)doe - 719468;
}
int yearFromDays(long z) {
  z += 719468;
  long era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned doe = (unsigned)(z - era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  long y = (long)yoe + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned mp = (5 * doy + 2) / 153;
  unsigned m = mp < 10 ? mp + 3 : mp - 9;
  return (int)(y + (m <= 2));
}
long lastSundayDays(int y, int m) {          // m has 31 days (mar/oct)
  long d31 = daysFromCivil(y, m, 31);
  return d31 - ((d31 + 4) % 7);              // 1970-01-01 was thursday
}
int tzOffsetMin(uint32_t utc) {
  int off = TZ_BASE_MIN;
  if (TZ_UK_DST) {
    int y = yearFromDays(utc / 86400);
    uint32_t s = (uint32_t)(lastSundayDays(y, 3)  * 86400L + 3600);
    uint32_t e = (uint32_t)(lastSundayDays(y, 10) * 86400L + 3600);
    if (utc >= s && utc < e) off += 60;
  }
  return off;
}

uint32_t utcNow() {
  if (clockSynced) return (uint32_t)((nowMs() + clockOffsetMs) / 1000);
  if (haveReading) return lastReadingEpoch + (uint32_t)((nowMs() - lastRxMs) / 1000);
  return 0;
}
long ageSec() {
  uint32_t u = utcNow();
  if (!u || !haveReading) return 0;
  long a = (long)u - (long)lastReadingEpoch;
  return a < 0 ? 0 : a;
}
int localMinuteOfDay() {
  if (!clockSynced && synthTime) return -1;
  uint32_t u = utcNow();
  if (!u) return -1;
  long l = (long)u + tzOffsetMin(u) * 60L;
  return (int)((l % 86400) / 60);
}

String fmtG(int mg) { return USE_MMOL ? String(mg / 18.0182f, 1) : String(mg); }
String fmtDelta(int d) {
  if (USE_MMOL) { float v = d / 18.0182f; return String(v >= 0 ? "+" : "") + String(v, 1); }
  return String(d >= 0 ? "+" : "") + String(d);
}
String fmtDH(int h) {
  if (h < 0) return "--";
  if (h >= 24) return String(h / 24) + "d " + String(h % 24) + "h";
  return String(h) + "h";
}
String fmtAgo(long s) {
  if (s < 0) s = 0;
  if (s < 3600) return String(s / 60) + "m";
  long h = s / 3600, m = (s % 3600) / 60;
  if (h >= 24) return String(h / 24) + "d " + String(h % 24) + "h";
  return String(h) + "h " + String(m) + "m";
}

float rateMgMin() {   // mg/dl per minute over last ~15 min
  if (nReadings < 2) return 0;
  int a = max(0, nReadings - 4);
  float dt = (readings[nReadings - 1].t - readings[a].t) / 60.0f;
  if (dt <= 0 || dt > 25) return 0;
  return (readings[nReadings - 1].sgv - readings[a].sgv) / dt;
}

// alerts
// globals instead of a struct return (avoids arduino auto-prototype issues)
const char* alText = nullptr;
uint16_t alBg = 0, alFg = 0;
uint8_t  alLed = 0;

void setAlert(const char* t, uint16_t bg, uint16_t fg, uint8_t led) {
  alText = t; alBg = bg; alFg = fg; alLed = led;
}

void computeAlert() {
  setAlert(nullptr, 0, 0, 0);
  if (!haveReading) return;
  if (!deviceConnected) { setAlert("PHONE LINK LOST", C_GREY, C_TEXT, 0); return; }
  if (ageSec() > 900)   { setAlert("NO RECENT SG DATA", C_GREY, C_TEXT, 0); return; }
  if (curSgv <= thrUrgentLow) { setAlert("URGENT LOW SG", C_LOW, C_TEXT, 1); return; }
  if (curSgv <  thrLow)       { setAlert("LOW SG", C_LOW, C_TEXT, 1); return; }
  float rate = rateMgMin();
  if (rate < -0.5f && curSgv + rate * 20 <= thrLow)
                              { setAlert("LOW SG PREDICTED", C_WARN, C_BLACK, 2); return; }
  if (curSgv >= thrUrgentHigh) { setAlert("VERY HIGH SG", C_HIGH, C_BLACK, 2); return; }
  if (curSgv >  thrHigh)       { setAlert("HIGH SG", C_HIGH, C_BLACK, 2); return; }
  if (curDir == "DoubleDown")  { setAlert("FALLING FAST", C_WARN, C_BLACK, 0); return; }
  if (curDir == "DoubleUp")    { setAlert("RISING FAST", C_HIGH, C_BLACK, 0); return; }
  if (sensorAgeH >= SENSOR_LIFE_H) { setAlert("CHANGE SENSOR", C_HIGH, C_BLACK, 0); return; }
}

void drawBanner(bool force = false) {
  computeAlert();
  if (!force && alText == lastBannerText) return;
  lastBannerText = alText;
  tft.fillRect(0, 22, 240, 22, C_BG);
  if (alText) {
    tft.fillRoundRect(4, 24, 232, 19, 4, alBg);
    tft.setTextColor(alFg, alBg);
    tft.drawCentreString(alText, 120, 26, 2);
  }
  setRGB(alLed == 1 || alLed == 2, alLed == 2, false);
}

// drawing
void drawSplash(const char *a, const char *b) {
  tft.fillScreen(C_BG);
  tft.setTextColor(C_TEXT, C_BG);
  tft.drawCentreString(a, 120, 130, 4);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawCentreString(b, 120, 170, 2);
}

void drawTopBar() {
  tft.fillRect(0, 0, 240, 22, C_PANEL);
  int m = localMinuteOfDay();
  lastClockMin = m;
  char buf[12];
  if (m >= 0) sprintf(buf, "%02d:%02d", m / 60, m % 60); else strcpy(buf, "--:--");
  tft.setTextColor(C_TEXT, C_PANEL);
  tft.drawString(buf, 6, 3, 2);
  tft.setTextColor(C_ACCENT, C_PANEL);
  tft.drawCentreString("Statera", 120, 3, 2);
  tft.fillCircle(228, 11, 5, deviceConnected ? C_ACCENT : C_LOW);
}

void drawTrendArrow(int cx, int cy, const String &dir, uint16_t col, uint16_t bg) {
  int count = 1; float ang = 0;
  if      (dir == "DoubleUp")      { count = 2; ang = -90; }
  else if (dir == "SingleUp")      ang = -90;
  else if (dir == "FortyFiveUp")   ang = -45;
  else if (dir == "Flat")          ang = 0;
  else if (dir == "FortyFiveDown") ang = 45;
  else if (dir == "SingleDown")    ang = 90;
  else if (dir == "DoubleDown")    { count = 2; ang = 90; }
  else { tft.setTextColor(C_DIM, bg); tft.drawCentreString("--", cx, cy - 12, 4); return; }

  float r = ang * DEG_TO_RAD;
  float dx = cosf(r), dy = sinf(r), px = -dy, py = dx;
  float half = (count == 1) ? 15 : 11;
  for (int k = 0; k < count; k++) {
    float off = (k - (count - 1) / 2.0f) * 17.0f;
    float sx = cx + px * off - dx * half, sy = cy + py * off - dy * half;
    float ex = cx + px * off + dx * half, ey = cy + py * off + dy * half;
    for (int w = -1; w <= 1; w++)
      tft.drawLine(sx + px * w, sy + py * w, ex - dx * 6 + px * w, ey - dy * 6 + py * w, col);
    tft.fillTriangle(ex + dx * 3, ey + dy * 3,
                     ex - dx * 9 + px * 8, ey - dy * 9 + py * 8,
                     ex - dx * 9 - px * 8, ey - dy * 9 - py * 8, col);
  }
}

void drawSG() {
  tft.fillRoundRect(4, SG_Y, 232, 82, 10, C_PANEL);
  if (!haveReading) return;
  bool stale = ageSec() > 600;
  lastStale = stale;
  uint16_t col = stale ? C_DIM : (curSgv < thrLow ? C_LOW : (curSgv > thrHigh ? C_HIGH : C_OK));

  tft.setTextColor(col, C_PANEL);
  tft.drawCentreString(fmtG(curSgv), 92, SG_Y + 8, 7);
  tft.setTextColor(C_DIM, C_PANEL);
  tft.drawCentreString(USE_MMOL ? "mmol/L" : "mg/dL", 92, SG_Y + 60, 2);

  drawTrendArrow(198, SG_Y + 32, curDir, stale ? C_DIM : C_TEXT, C_PANEL);
  if (hasDelta) {
    tft.setTextColor(C_TEXT, C_PANEL);
    tft.drawCentreString(fmtDelta(deltaMg), 198, SG_Y + 60, 2);
  }
}

void dashH(int x0, int x1, int y, uint16_t c) { for (int x = x0; x < x1; x += 6) tft.drawFastHLine(x, y, 3, c); }

int yForMg(int mg, int ymax) {
  mg = constrain(mg, 40, ymax);
  return PLOT_Y + PLOT_H - 1 - (long)(mg - 40) * (PLOT_H - 1) / (ymax - 40);
}

void drawGraph() {
  tft.fillRect(0, GRAPH_Y, 240, 118, C_BG);
  int hrs = RANGE_H[rangeIdx];
  uint32_t span = hrs * 3600UL;
  uint32_t tEnd = utcNow(); if (!tEnd) tEnd = lastReadingEpoch;
  uint32_t tStart = tEnd - span;

  int maxv = 0, cnt = 0, inr = 0; long sum = 0;
  for (int i = 0; i < nReadings; i++) {
    if (readings[i].t < tStart) continue;
    cnt++; sum += readings[i].sgv;
    if (readings[i].sgv > maxv) maxv = readings[i].sgv;
    if (readings[i].sgv >= thrLow && readings[i].sgv <= thrHigh) inr++;
  }
  int ymax = 250;
  if (maxv > 250) ymax = min(400, ((maxv + 49) / 50) * 50);

  tft.fillRoundRect(4, GRAPH_Y + 1, 26, 12, 3, C_ACCENT);
  tft.setTextColor(C_BLACK, C_ACCENT);
  tft.drawCentreString(String(hrs) + "h", 17, GRAPH_Y + 3, 1);
  tft.setTextColor(C_DIM, C_BG);
  if (cnt > 0) tft.drawString("TIR " + String(inr * 100 / cnt) + "%  Avg " + fmtG(sum / cnt), 38, GRAPH_Y + 3, 1);
  else         tft.drawString("No data in range", 38, GRAPH_Y + 3, 1);

  tft.fillRect(PLOT_X, PLOT_Y, PLOT_W, PLOT_H, C_PANEL);
  int yH = yForMg(thrHigh, ymax), yL = yForMg(thrLow, ymax);
  tft.fillRect(PLOT_X, yH, PLOT_W, yL - yH, C_BAND);
  uint32_t step = (hrs == 1) ? 900 : 3600;
  for (uint32_t k = step; k < span; k += step) {
    int x = PLOT_X + PLOT_W - 1 - (long)k * (PLOT_W - 1) / span;
    for (int y = PLOT_Y; y < PLOT_Y + PLOT_H; y += 4) tft.drawPixel(x, y, C_GREY);
  }
  dashH(PLOT_X, PLOT_X + PLOT_W, yH, C_HIGH);
  dashH(PLOT_X, PLOT_X + PLOT_W, yL, C_LOW);
  tft.drawRect(PLOT_X, PLOT_Y, PLOT_W, PLOT_H, C_GREY);

  tft.setTextColor(C_HIGH, C_BG); tft.drawString(fmtG(thrHigh), PLOT_X + PLOT_W + 4, yH - 4, 1);
  tft.setTextColor(C_LOW,  C_BG); tft.drawString(fmtG(thrLow),  PLOT_X + PLOT_W + 4, yL - 4, 1);

  int rad = (hrs <= 3) ? 2 : 1;
  for (int i = 0; i < nReadings; i++) {
    if (readings[i].t < tStart) continue;
    uint32_t dt = readings[i].t - tStart; 
    if (dt > span) dt = span;
    int x = PLOT_X + (long)dt * (PLOT_W - 1) / span;
    int y = yForMg(readings[i].sgv, ymax);
    uint16_t c = readings[i].sgv < thrLow ? C_LOW : (readings[i].sgv > thrHigh ? C_HIGH : C_OK);
    tft.fillCircle(x, y, rad, c);
    if (i == nReadings - 1) tft.drawCircle(x, y, rad + 2, C_TEXT);
  }

  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("-" + String(hrs) + "h", PLOT_X, PLOT_Y + PLOT_H + 3, 1);
  tft.drawRightString("now", PLOT_X + PLOT_W, PLOT_Y + PLOT_H + 3, 1);
}

void drawTile(int col, int row, const char *label, const String &val, uint16_t vcol) {
  int x = 2 + col * 119, y = TILE_Y + row * 25;
  tft.fillRoundRect(x, y, 117, 23, 4, C_PANEL);
  tft.setTextColor(C_DIM, C_PANEL);
  tft.drawString(label, x + 6, y + 8, 1);
  tft.setTextColor(vcol, C_PANEL);
  tft.drawRightString(val, x + 111, y + 4, 2);
}

void drawTiles() {
  drawTile(0, 0, "ACTIVE INS", isnan(iobU) ? String("--") : String(iobU, 2) + " U", C_TEXT);
  drawTile(1, 0, "CARBS", isnan(cobG) ? String("--") : String((int)cobG) + " g", C_TEXT);

  String bv = "--";
  if (haveBolus) {
    uint32_t u = utcNow();
    long a = u ? (long)u - (long)lastBolusEpoch : 0;
    bv = String(lastBolusU, (lastBolusU == (int)lastBolusU) ? 0 : 1) + "U " + fmtAgo(a);
  }
  drawTile(0, 1, "BOLUS", bv, C_TEXT);

  String sv = "--"; uint16_t sc = C_TEXT;
  if (sensorAgeH >= 0) {
    int rem = SENSOR_LIFE_H - sensorAgeH;
    if (rem <= 0) { sv = "Expired"; sc = C_LOW; }
    else { sv = fmtDH(rem); if (rem < 24) sc = C_HIGH; }
  }
  drawTile(1, 1, "SENSOR", sv, sc);
}

void setStatusMsg(const String &m, uint16_t c, unsigned long ms) {
  statusMsg = m; statusMsgColor = c; statusMsgUntil = millis() + ms;
  lastStatusText = "";
}

void drawStatusLine() {
  String s; uint16_t col = C_DIM;
  if (millis() < statusMsgUntil) { s = statusMsg; col = statusMsgColor; }
  else if (haveReading) {
    long a = ageSec();
    s = "Last SG: " + (a < 60 ? String(a) + "s" : String(a / 60) + " min") + " ago";
    col = a > 600 ? C_HIGH : C_DIM;
  }
  if (s == lastStatusText && col == lastStatusColor) return;
  lastStatusText = s; lastStatusColor = col;
  tft.fillRect(0, STATUS_Y, 240, 17, C_BG);
  tft.setTextColor(col, C_BG);
  tft.drawCentreString(s, 120, STATUS_Y + 1, 2);
}

void renderAll() {
  tft.fillScreen(C_BG);
  drawTopBar();
  drawBanner(true);
  drawSG();
  drawGraph();
  drawTiles();
  lastStatusText = "";
  drawStatusLine();
}

// parsing
bool parseStatus(String &s) {
  JsonDocument filter;
  filter["serverTimeEpoch"] = true;
  filter["settings"]["thresholds"] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, s, DeserializationOption::Filter(filter));
  if (err) { Serial.printf("status parse: %s\n", err.c_str()); return false; }

  double ep = doc["serverTimeEpoch"].as<double>();
  if (ep > 0) {
    if (ep < 1e11) ep *= 1000.0;               // seconds -> ms
    clockOffsetMs = (int64_t)ep - nowMs();
    clockSynced = true;
  }
  if (USE_NS_THRESHOLDS) {
    JsonVariant t = doc["settings"]["thresholds"];
    if (!t.isNull()) {
      thrUrgentLow  = t["bgLow"]          | thrUrgentLow;
      thrLow        = t["bgTargetBottom"] | thrLow;
      thrHigh       = t["bgTargetTop"]    | thrHigh;
      thrUrgentHigh = t["bgHigh"]         | thrUrgentHigh;
    }
  }
  return clockSynced;
}

bool parseEntries(String &s) {
  JsonDocument filter;
  filter[0]["sgv"] = true;
  filter[0]["date"] = true;
  filter[0]["direction"] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, s, DeserializationOption::Filter(filter));
  if (err || !doc.is<JsonArray>() || doc.size() == 0) {
    Serial.printf("entries parse: %s\n", err.c_str());
    return false;
  }

  JsonArray arr = doc.as<JsonArray>();
  bool hasDates = !arr[0]["date"].isNull();
  uint32_t base = 0;
  if (!hasDates) {
    base = clockSynced ? utcNow() : 1700000000UL;
    synthTime = !clockSynced;
  } else {
    synthTime = false;
  }

  int n = 0;
  Reading tmp[MAX_PTS];                       // newest first
  String newestDir = "";
  int idx = 0;
  for (JsonObject e : arr) {
    if (e["sgv"].isNull()) { idx++; continue; }
    int sgv = e["sgv"].as<int>();
    if (sgv <= 0) { idx++; continue; }
    uint32_t t;
    if (hasDates) {
      double d = e["date"].as<double>();
      if (d <= 0) { idx++; continue; }
      t = (uint32_t)(d / 1000.0);
    } else {
      t = base - (uint32_t)idx * 300UL;
    }
    if (n == 0) newestDir = e["direction"] | "";
    if (n < MAX_PTS) { tmp[n].t = t; tmp[n].sgv = sgv; n++; }
    idx++;
  }
  if (n == 0) return false;

  nReadings = n;
  for (int i = 0; i < n; i++) readings[i] = tmp[n - 1 - i]; // oldest -> newest

  curSgv = tmp[0].sgv;
  curDir = newestDir;
  lastReadingEpoch = tmp[0].t;
  hasDelta = (n > 1 && (tmp[0].t - tmp[1].t) <= 600);
  deltaMg = hasDelta ? (tmp[0].sgv - tmp[1].sgv) : 0;
  haveReading = true;
  lastRxMs = nowMs();
  loadedHours = requestedHours;
  return true;
}

int ageOf(JsonVariant v) {
  if (v.isNull()) return -1;
  bool found = v["found"] | false;
  if (!found) return -1;
  return v["age"] | -1;
}

bool parseProps(String &s) {
  JsonDocument filter;
  filter["iob"]["iob"] = true;
  filter["cob"]["cob"] = true;
  filter["sage"] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, s, DeserializationOption::Filter(filter));
  if (err) { Serial.printf("props parse: %s\n", err.c_str()); return false; }

  JsonVariant iv = doc["iob"]["iob"];
  iobU = iv.isNull() ? NAN : iv.as<float>();
  JsonVariant cv = doc["cob"]["cob"];
  cobG = cv.isNull() ? NAN : cv.as<float>();

  int a = ageOf(doc["sage"]["Sensor Start"]);
  if (a < 0) a = ageOf(doc["sage"]["Sensor Change"]);
  sensorAgeH = a;
  return true;
}

bool parseBolus(String &s) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, s);
  if (err || !doc.is<JsonArray>()) { Serial.printf("bolus parse: %s\n", err.c_str()); return false; }
  if (doc.size() == 0) { haveBolus = false; return true; }
  lastBolusU = doc[0]["insulin"].as<float>();
  double d = doc[0]["date"].as<double>();
  lastBolusEpoch = (uint32_t)(d / 1000.0);
  haveBolus = lastBolusEpoch > 0;
  return true;
}

// ble
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer)    { deviceConnected = true; }
  void onDisconnect(BLEServer *pServer) { deviceConnected = false; }
};

class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    String chunk = pCharacteristic->getValue().c_str();
    if (chunk.length() == 0) return;
    xSemaphoreTake(rxMutex, portMAX_DELAY);
    if (rxBuffer.length() + chunk.length() > RX_BUFFER_MAX) {
      rxBuffer = "";
    } else {
      rxBuffer += chunk;
      if (rxBuffer.endsWith("\n")) newDataAvailable = true;
    }
    xSemaphoreGive(rxMutex);
  }
};

bool sendRequest(const String &path) {
  String sep = (path.indexOf('?') >= 0) ? "&" : "?";
  String reqPacket = "GET|" + String(NS_URL) + path + sep + "secret=" + secretHash + "\n";

  xSemaphoreTake(rxMutex, portMAX_DELAY);
  rxBuffer = "";
  newDataAvailable = false;
  xSemaphoreGive(rxMutex);

  int len = reqPacket.length();
  for (int i = 0; i < len; i += BLE_CHUNK) {
    if (!deviceConnected) return false;
    String sub = reqPacket.substring(i, min(i + BLE_CHUNK, len));
    pTxCharacteristic->setValue((uint8_t *)sub.c_str(), sub.length());
    pTxCharacteristic->notify();
    delay(20);
  }
  waitingForResponse = true;
  requestSentAt = millis();
  return true;
}

// poll state machine
bool needStatus() {
  if (!statusTried) return true;
  return (millis() - lastStatusTryMs) > (clockSynced ? 3600000UL : 300000UL);
}

void startStage(int s) {
  stage = (Stage)s;
  String path;
  switch (s) {
    case ST_STATUS:
      path = "/api/v1/status.json";
      statusTried = true; lastStatusTryMs = millis();
      break;
    case ST_ENTRIES:
      requestedHours = RANGE_H[rangeIdx];
      path = String("/api/v1/entries/sgv.json?count=") + String(requestedHours * 12 + 2);
      break;
    case ST_PROPS:
      path = "/api/v2/properties/iob,cob,sage";
      break;
    case ST_BOLUS:
      path = "/api/v1/treatments.json?find%5Binsulin%5D%5B%24gt%5D=0&count=10";
      break;
    default: return;
  }
  lastPath = path;
  if (!sendRequest(path)) { stage = ST_IDLE; nextPollAt = millis() + RETRY_DELAY_MS; }
}

void scheduleNext() {
  unsigned long d = POLL_INTERVAL_MS;
  if (clockSynced && haveReading) {
    long wait = 305 - ageSec();     // aim just after the next 5-min reading
    if (wait < 20) wait = 30;
    if (wait > 310) wait = 310;
    d = wait * 1000UL;
  }
  nextPollAt = millis() + d;
}

void finishCycle() {
  stage = ST_IDLE;
  waitingForResponse = false;
  scheduleNext();
  drawTopBar();
  drawTiles();
  drawBanner(true);
}

void failStage(const String &msg) {
  Stage prev = stage;
  waitingForResponse = false;
  stage = ST_IDLE;
  if (prev == ST_STATUS) { startStage(ST_ENTRIES); return; }   // non-fatal
  if (prev == ST_PROPS)  { startStage(ST_BOLUS); return; }     // keep old data, try bolus
  if (prev == ST_BOLUS)  { finishCycle(); return; }
  setStatusMsg(msg, C_WARN, 6000);
  drawStatusLine();
  nextPollAt = millis() + RETRY_DELAY_MS;
}

// work out what a response is from its content, so late/duplicate replies still work
int identifyPayload(const String &s) {
  if (s.indexOf("serverTimeEpoch") >= 0) return ST_STATUS;
  if (s.startsWith("[") && s.indexOf("\"sgv\"") >= 0) return ST_ENTRIES;
  if (s.startsWith("[") && s.indexOf("\"insulin\"") >= 0) return ST_BOLUS;
  if (s.indexOf("\"iob\"") >= 0 || s.indexOf("\"cob\"") >= 0 ||
      s.indexOf("\"sage\"") >= 0 || s.indexOf("\"cage\"") >= 0) return ST_PROPS;
  return ST_IDLE;   // unknown shape (e.g. "[]"): caller falls back to the expected stage
}

bool processPayload(int kind, String &s) {
  switch (kind) {
    case ST_STATUS:  return parseStatus(s);
    case ST_ENTRIES: return parseEntries(s);
    case ST_PROPS:   return parseProps(s);
    case ST_BOLUS:   return parseBolus(s);
    default:         return false;
  }
}

// setup / loop
void setup() {
  Serial.begin(115200);
  rxMutex = xSemaphoreCreateMutex();
  rxBuffer.reserve(RX_BUFFER_MAX);
  secretHash = sha1Hash(String(API_SECRET));

  // backlight via pwm (esp32 arduino core 3.x), off during init
  ledcAttach(TFT_BL_PIN, 5000, 8);
  setBacklight(0);

  pinMode(LED_RED, OUTPUT); pinMode(LED_GREEN, OUTPUT); pinMode(LED_BLUE, OUTPUT);
  setRGB(false, false, false);
  pinMode(TOUCH_IRQ, INPUT);

  tft.init();
  tft.setRotation(SCREEN_ROTATION);
  tft.invertDisplay(true);                // required on this panel
  tft.fillScreen(C_BG);
  setBacklight(BACKLIGHT_LEVEL);

  drawSplash("Statera", "Awaiting BLE companion...");

  BLEDevice::init("CYD_Nightscout");
  BLEDevice::setMTU(185);
  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  BLEService *pService = pServer->createService(SERVICE_UUID);

  pTxCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID_TX, BLECharacteristic::PROPERTY_NOTIFY);
  pTxCharacteristic->addDescriptor(new BLE2902());

  BLECharacteristic *pRx = pService->createCharacteristic(
      CHARACTERISTIC_UUID_RX, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  pRx->setCallbacks(new RxCallbacks());

  pService->start();
  BLEAdvertising *pAdv = BLEDevice::getAdvertising();
  pAdv->addServiceUUID(SERVICE_UUID);
  pAdv->setScanResponse(true);
  pAdv->setMinPreferred(0x06);
  pAdv->setMaxPreferred(0x12);
  BLEDevice::startAdvertising();
}

void loop() {
  unsigned long now = millis();

  // connection transitions
  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = true;
    waitingForResponse = false;
    stage = ST_IDLE;
    nextPollAt = now + FIRST_POLL_DELAY;
    if (haveReading) { drawTopBar(); drawBanner(true); }
    else drawSplash("Phone connected", "Fetching...");
  }
  if (!deviceConnected && oldDeviceConnected) {
    oldDeviceConnected = false;
    waitingForResponse = false;
    stage = ST_IDLE;
    delay(500);
    BLEDevice::startAdvertising();
    if (haveReading) { drawTopBar(); drawBanner(true); }
    else drawSplash("BLE disconnected", "Waiting for phone...");
  }

  // poll scheduling
  if (deviceConnected) {
    if (!waitingForResponse && stage == ST_IDLE && (long)(now - nextPollAt) >= 0) {
      startStage(needStatus() ? ST_STATUS : ST_ENTRIES);
    }
    if (waitingForResponse && (millis() - requestSentAt > RESPONSE_TIMEOUT)) {
      failStage("No response - retrying");
    }
  }

  // complete message received
  if (newDataAvailable) {
    bool ok = false;
    String err = "";
    Stage expected = stage;
    int kind = ST_IDLE;
    xSemaphoreTake(rxMutex, portMAX_DELAY);
    rxBuffer.trim();
    Serial.printf("RX %u bytes (expected stage %d)\n", (unsigned)rxBuffer.length(), (int)expected);
    Serial.println(rxBuffer.substring(0, 80));
    if (DEBUG_RAW && rxBuffer.length() > 80) {
      Serial.println("[...]");
      Serial.println(rxBuffer.substring(rxBuffer.length() - 80));
    }
    if (rxBuffer.startsWith("ERR")) {
      err = rxBuffer.substring(0, 40);
      Serial.println("ERR reply to: " + lastPath);
    } else {
      kind = identifyPayload(rxBuffer);
      if (kind == ST_IDLE) kind = (int)expected;
      ok = processPayload(kind, rxBuffer);
    }
    rxBuffer = "";
    newDataAvailable = false;
    xSemaphoreGive(rxMutex);

    bool matches = (kind == (int)expected);
    if (matches || expected == ST_IDLE) waitingForResponse = false;

    if (!ok) {
      Serial.printf("Rejected (kind %d)\n", kind);
      if (expected != ST_IDLE) failStage(err.length() ? err : String("Bad data from phone"));
    } else {
      if (kind == ST_ENTRIES) renderAll();
      else if (kind == ST_PROPS || kind == ST_BOLUS) { drawTiles(); drawBanner(true); }
      else if (kind == ST_STATUS) drawTopBar();

      if (matches) {                       // advance the chain
        if      (expected == ST_STATUS)  startStage(ST_ENTRIES);
        else if (expected == ST_ENTRIES) startStage(ST_PROPS);
        else if (expected == ST_PROPS)   startStage(ST_BOLUS);
        else if (expected == ST_BOLUS)   finishCycle();
      }
    }
  }

  // touch: tap anywhere to cycle graph range
  if (digitalRead(TOUCH_IRQ) == LOW && now - lastTouchMs > 500) {
    lastTouchMs = now;
    rangeIdx = (rangeIdx + 1) % 3;
    if (haveReading) {
      drawGraph();
      if (RANGE_H[rangeIdx] > loadedHours) {
        setStatusMsg("Loading " + String(RANGE_H[rangeIdx]) + "h...", C_ACCENT, 5000);
        drawStatusLine();
        if (stage == ST_IDLE) nextPollAt = now;
      }
    }
  }

  // 1 hz housekeeping
  if (haveReading && now - lastTick >= 1000) {
    lastTick = now;
    if (localMinuteOfDay() != lastClockMin) drawTopBar();
    bool stale = ageSec() > 600;
    if (stale != lastStale) drawSG();
    drawBanner();
    drawStatusLine();
  }

  delay(20);
}
