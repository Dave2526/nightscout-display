// Nightscout-Anzeige für LilyGO T-Display-S3
// Holt BZ, Trend, Delta, IOB/COB per Nightscout-API (Token mit Rolle "readable").
// Tasten: links (GPIO0) kurz = Helligkeit, lang = Bild drehen
//         rechts (GPIO14) kurz = sofort abrufen, 5 s halten = WLAN/Token neu einrichten
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <time.h>
#include <math.h>

#include "lgfx_tdisplay_s3.h"
#include "certs.h"

#ifndef OTA_PASS
#define OTA_PASS ""
#endif

static const int PIN_POWER = 15;  // schaltet Display (und Akku-Betrieb) ein
static const int PIN_BTN_L = 0;
static const int PIN_BTN_R = 14;
static const char *AP_NAME = "NS-Display";
static const char *HOSTNAME = "ns-display";
static const char *DEFAULT_URL = "https://nightscout.example.org";
static const uint32_t FETCH_MS = 60 * 1000;
static const uint32_t STATUS_MS = 60 * 60 * 1000;
static const int GRAPH_MIN = 180;  // Verlauf: 3 Stunden
static const uint8_t BRIGHT[] = {255, 120, 40, 8};

LGFX lcd;
LGFX_Sprite spr(&lcd);
Preferences prefs;

String nsUrl, nsToken;
uint8_t brightIdx = 0;
uint8_t rot = 1;

struct Entry {
  time_t t;
  int16_t sgv;
};
Entry hist[48];
int histN = 0;
String direction;
bool haveDelta = false;
int delta = 0;
float iob = NAN, cob = NAN;
int thLow = 72, thBottom = 80, thTop = 180, thHigh = 250;
String lastError;
uint32_t lastFetch = 0, lastStatus = 0;
bool statusOk = false;

// ---------- Einstellungen ----------

void loadPrefs() {
  prefs.begin("nsd", true);
  nsUrl = prefs.getString("url", DEFAULT_URL);
  nsToken = prefs.getString("token", "");
  brightIdx = prefs.getUChar("bright", 0) % sizeof(BRIGHT);
  rot = prefs.getUChar("rot", 1);
  prefs.end();
}

void savePrefs() {
  prefs.begin("nsd", false);
  prefs.putString("url", nsUrl);
  prefs.putString("token", nsToken);
  prefs.putUChar("bright", brightIdx);
  prefs.putUChar("rot", rot);
  prefs.end();
}

// ---------- Anzeige-Hilfen ----------

uint16_t bgColor(int v) {
  if (v < thLow) return lcd.color565(255, 40, 40);
  if (v > thHigh) return lcd.color565(255, 120, 0);
  if (v < thBottom || v > thTop) return lcd.color565(255, 210, 0);
  return lcd.color565(40, 230, 110);
}

void message(const char *l1, const char *l2 = "", const char *l3 = "") {
  spr.fillScreen(TFT_BLACK);
  spr.setTextDatum(middle_center);
  spr.setTextColor(TFT_WHITE);
  spr.setFont(&fonts::FreeSansBold12pt7b);
  spr.drawString(l1, 160, 50);
  spr.setFont(&fonts::FreeSans9pt7b);
  spr.setTextColor(TFT_LIGHTGREY);
  spr.drawString(l2, 160, 95);
  spr.drawString(l3, 160, 125);
  spr.pushSprite(0, 0);
}

void arrow(float cx, float cy, float deg, float len, uint16_t col) {
  float a = deg * M_PI / 180, dx = cosf(a), dy = sinf(a);
  float tx = cx - dx * len / 2, ty = cy - dy * len / 2;  // Schwanz
  float hx = cx + dx * len / 2, hy = cy + dy * len / 2;  // Spitze
  float bx = hx - dx * 18, by = hy - dy * 18;            // Basis der Spitze
  spr.drawWideLine(tx, ty, bx, by, 4, col);
  spr.fillTriangle(hx, hy, bx - dy * 13, by + dx * 13, bx + dy * 13, by - dx * 13, col);
}

void drawTrend(float cx, float cy, uint16_t col) {
  const String &d = direction;
  float deg;
  bool dbl = false;
  if (d == "DoubleUp") { deg = -90; dbl = true; }
  else if (d == "SingleUp") deg = -90;
  else if (d == "FortyFiveUp") deg = -45;
  else if (d == "Flat") deg = 0;
  else if (d == "FortyFiveDown") deg = 45;
  else if (d == "SingleDown") deg = 90;
  else if (d == "DoubleDown") { deg = 90; dbl = true; }
  else {
    spr.setFont(&fonts::FreeSansBold18pt7b);
    spr.setTextDatum(middle_center);
    spr.setTextColor(col);
    spr.drawString("?", cx, cy);
    return;
  }
  if (dbl) {
    arrow(cx - 14, cy, deg, 54, col);
    arrow(cx + 14, cy, deg, 54, col);
  } else {
    arrow(cx, cy, deg, 54, col);
  }
}

void drawGraph(int y0, int h, time_t now) {
  const int W = 320;
  const int vmin = 40, vmax = 300;
  auto ymap = [&](int v) {
    v = constrain(v, vmin, vmax);
    return y0 + h - 1 - (v - vmin) * (h - 1) / (vmax - vmin);
  };
  spr.fillRect(0, ymap(thTop), W, ymap(thBottom) - ymap(thTop) + 1, lcd.color565(0, 45, 20));
  for (int i = 1; i < 6; i++)  // Stundenstriche
    spr.drawFastVLine(W - i * W / 6, y0, h, lcd.color565(35, 35, 35));
  spr.drawFastHLine(0, ymap(thLow), W, lcd.color565(120, 20, 20));
  spr.drawFastHLine(0, ymap(thHigh), W, lcd.color565(120, 70, 0));
  for (int i = 0; i < histN; i++) {
    long ageMin = (now - hist[i].t) / 60;
    if (ageMin < 0 || ageMin > GRAPH_MIN) continue;
    int x = W - 4 - ageMin * (W - 8) / GRAPH_MIN;
    spr.fillCircle(x, ymap(hist[i].sgv), 2, bgColor(hist[i].sgv));
  }
}

void draw() {
  time_t now = time(nullptr);
  bool timeOk = now > 1700000000;
  struct tm lt;
  localtime_r(&now, &lt);

  spr.fillScreen(TFT_BLACK);
  spr.setFont(&fonts::FreeSansBold9pt7b);

  // Kopfzeile: Uhr | IOB/COB | Alter bzw. Fehler
  char buf[48];
  spr.setTextColor(TFT_LIGHTGREY);
  spr.setTextDatum(top_left);
  if (timeOk) {
    strftime(buf, sizeof(buf), "%H:%M", &lt);
    spr.drawString(buf, 4, 3);
  }
  String mid;
  if (!isnan(iob)) { snprintf(buf, sizeof(buf), "IOB %.1f", iob); mid += buf; }
  if (!isnan(cob)) { snprintf(buf, sizeof(buf), "   COB %.0f", cob); mid += buf; }
  spr.setTextDatum(top_center);
  spr.setTextColor(lcd.color565(110, 170, 255));
  spr.drawString(mid, 165, 3);

  long ageMin = (histN && timeOk) ? (now - hist[0].t) / 60 : -1;
  bool stale = ageMin < 0 || ageMin >= 11;
  spr.setTextDatum(top_right);
  if (lastError.length()) {
    spr.setTextColor(TFT_RED);
    spr.drawString(lastError, 316, 3);
  } else if (ageMin >= 0) {
    spr.setTextColor(stale ? TFT_RED : TFT_LIGHTGREY);
    snprintf(buf, sizeof(buf), "%ld min", ageMin);
    spr.drawString(buf, 316, 3);
  }

  if (!histN) {
    spr.setFont(&fonts::FreeSans12pt7b);
    spr.setTextDatum(middle_center);
    spr.setTextColor(TFT_LIGHTGREY);
    spr.drawString(WiFi.isConnected() ? "Warte auf Daten" : "Kein WLAN", 160, 75);
    spr.pushSprite(0, 0);
    return;
  }

  int v = hist[0].sgv;
  uint16_t col = stale ? TFT_DARKGREY : bgColor(v);

  // Wert in 7-Segment-Optik
  spr.setFont(&fonts::Font7);
  spr.setTextSize(1.9);
  spr.setTextDatum(middle_center);
  spr.setTextColor(col);
  spr.drawNumber(v, 108, 74);
  spr.setTextSize(1);

  drawTrend(268, 55, col);
  if (haveDelta) {
    spr.setFont(&fonts::FreeSansBold12pt7b);
    spr.setTextDatum(middle_center);
    spr.setTextColor(stale ? TFT_DARKGREY : TFT_WHITE);
    snprintf(buf, sizeof(buf), "%+d", delta);
    spr.drawString(buf, 268, 106);
  }
  if (stale) spr.drawWideLine(20, 74, 196, 74, 3, TFT_RED);  // veraltet: durchgestrichen

  drawGraph(126, 44, now);
  spr.pushSprite(0, 0);
}

// ---------- Nightscout ----------

bool getJson(const String &path, JsonDocument &doc, JsonDocument *filter = nullptr) {
  String url = nsUrl + path + (path.indexOf('?') >= 0 ? "&" : "?") + "token=" + nsToken;
  WiFiClientSecure tls;
  WiFiClient plain;
  HTTPClient http;
  http.useHTTP10(true);  // kein chunked encoding -> direkt aus dem Stream parsen
  http.setTimeout(10000);
  bool ok;
  if (nsUrl.startsWith("https")) {
    tls.setCACert(LE_ROOTS);
    ok = http.begin(tls, url);
  } else {
    ok = http.begin(plain, url);
  }
  if (!ok) { lastError = "URL?"; return false; }
  int code = http.GET();
  if (code != 200) {
    lastError = code == 401 ? "Token?" : code < 0 ? "Verbindung" : "HTTP " + String(code);
    http.end();
    return false;
  }
  DeserializationError err = filter
      ? deserializeJson(doc, http.getStream(), DeserializationOption::Filter(*filter))
      : deserializeJson(doc, http.getStream());
  http.end();
  if (err) { lastError = "JSON"; return false; }
  return true;
}

void fetchStatus() {
  JsonDocument filter, doc;
  filter["settings"]["thresholds"] = true;
  if (!getJson("/api/v1/status.json", doc, &filter)) return;
  JsonObject th = doc["settings"]["thresholds"];
  thLow = th["bgLow"] | thLow;
  thBottom = th["bgTargetBottom"] | thBottom;
  thTop = th["bgTargetTop"] | thTop;
  thHigh = th["bgHigh"] | thHigh;
  statusOk = true;
}

void fetchData() {
  lastError = "";
  if (!statusOk || millis() - lastStatus > STATUS_MS) {
    lastStatus = millis();
    fetchStatus();
  }

  JsonDocument filter, doc;
  filter[0]["sgv"] = true;
  filter[0]["date"] = true;
  filter[0]["direction"] = true;
  if (!getJson("/api/v1/entries/sgv.json?count=48", doc, &filter)) return;
  JsonArray arr = doc.as<JsonArray>();
  histN = 0;
  for (JsonObject e : arr) {
    if (histN >= (int)(sizeof(hist) / sizeof(hist[0]))) break;
    int sgv = e["sgv"] | 0;
    if (sgv < 20) continue;  // Sensorfehler-Codes
    hist[histN].t = (time_t)((e["date"] | 0.0) / 1000.0);
    hist[histN].sgv = sgv;
    if (histN == 0) direction = (const char *)(e["direction"] | "");
    histN++;
  }
  haveDelta = histN > 1 && hist[0].t - hist[1].t <= 7 * 60;
  if (haveDelta) delta = hist[0].sgv - hist[1].sgv;

  JsonDocument p;
  iob = cob = NAN;
  if (getJson("/api/v2/properties/iob,cob", p)) {
    if (p["iob"]["iob"].is<float>()) iob = p["iob"]["iob"];
    if (p["cob"]["cob"].is<float>()) cob = p["cob"]["cob"];
  } else {
    lastError = "";  // IOB/COB sind optional
  }
}

// ---------- WLAN / Einrichtung ----------

void portal(bool force) {
  message("Einrichtung", "WLAN \"NS-Display\" verbinden,", "dann 192.168.4.1 aufrufen");
  WiFiManager wm;
  WiFiManagerParameter pUrl("nsurl", "Nightscout-URL", nsUrl.c_str(), 120);
  WiFiManagerParameter pTok("nstoken", "Zugriffstoken (Rolle readable)", nsToken.c_str(), 100);
  wm.addParameter(&pUrl);
  wm.addParameter(&pTok);
  wm.setTitle("Nightscout-Display");
  wm.setConfigPortalTimeout(300);
  wm.setConnectTimeout(20);
  bool ok = force ? wm.startConfigPortal(AP_NAME) : wm.autoConnect(AP_NAME);

  String u = pUrl.getValue(), t = pTok.getValue();
  u.trim();
  t.trim();
  while (u.endsWith("/")) u.remove(u.length() - 1);
  if (u.length() && (u != nsUrl || t != nsToken)) {
    nsUrl = u;
    nsToken = t;
    savePrefs();
  }
  if (!ok) ESP.restart();
}

void resetAll() {
  message("Zurueckgesetzt", "Neustart ...");
  WiFiManager wm;
  wm.resetSettings();
  nsToken = "";
  savePrefs();
  delay(1500);
  ESP.restart();
}

// ---------- Tasten ----------

struct Button {
  int pin;
  uint32_t downAt = 0;
  bool longDone = false;
  explicit Button(int p) : pin(p) {}
};
Button btnL{PIN_BTN_L}, btnR{PIN_BTN_R};

// 0 = nichts, 1 = kurz (beim Loslassen), 2 = lang (einmalig nach holdMs)
int pollButton(Button &b, uint32_t holdMs) {
  bool down = digitalRead(b.pin) == LOW;
  uint32_t ms = millis();
  if (down) {
    if (!b.downAt) b.downAt = ms;
    if (!b.longDone && ms - b.downAt >= holdMs) { b.longDone = true; return 2; }
    return 0;
  }
  int r = (b.downAt && !b.longDone && ms - b.downAt > 30) ? 1 : 0;
  b.downAt = 0;
  b.longDone = false;
  return r;
}

// ---------- Ablauf ----------

void setup() {
  pinMode(PIN_POWER, OUTPUT);
  digitalWrite(PIN_POWER, HIGH);
  pinMode(PIN_BTN_L, INPUT_PULLUP);
  pinMode(PIN_BTN_R, INPUT_PULLUP);
  Serial.begin(115200);

  loadPrefs();
  lcd.init();
  lcd.setRotation(rot);
  lcd.setBrightness(BRIGHT[brightIdx]);
  spr.setPsram(true);
  spr.setColorDepth(16);
  spr.createSprite(320, 170);
  message("Nightscout", "Verbinde WLAN ...");

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  portal(false);
  if (nsToken.isEmpty()) portal(true);
  WiFi.setAutoReconnect(true);

  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "fritz.box", "pool.ntp.org");

  ArduinoOTA.setHostname(HOSTNAME);
  if (strlen(OTA_PASS)) ArduinoOTA.setPassword(OTA_PASS);
  ArduinoOTA.onStart([]() { message("Update ...", "nicht ausschalten"); });
  ArduinoOTA.begin();

  message("Nightscout", "Hole Daten ...");
  fetchData();
  lastFetch = millis();
  draw();
}

void loop() {
  ArduinoOTA.handle();

  int l = pollButton(btnL, 1500);
  if (l == 1) {
    brightIdx = (brightIdx + 1) % sizeof(BRIGHT);
    lcd.setBrightness(BRIGHT[brightIdx]);
    savePrefs();
  } else if (l == 2) {
    rot = rot == 1 ? 3 : 1;
    lcd.setRotation(rot);
    savePrefs();
    draw();
  }

  int r = pollButton(btnR, 5000);
  if (r == 2) resetAll();
  bool force = r == 1;

  if (force || millis() - lastFetch >= FETCH_MS) {
    lastFetch = millis();
    if (WiFi.isConnected()) fetchData();
    else lastError = "WLAN";
    draw();
  }

  static uint32_t lastDraw = 0;
  if (millis() - lastDraw >= 1000) {
    lastDraw = millis();
    draw();
  }
  delay(10);
}
