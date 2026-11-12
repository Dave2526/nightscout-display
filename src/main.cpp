// Nightscout-Anzeige für LilyGO T-Display-S3
// Werte per HTTPS aus Nightscout (Token mit Rolle "readable"), damit es in jedem WLAN geht.
// Bis zu 3 WLANs; findet es keins, öffnet sich der AP "NS-Display" und das neue WLAN kommt dazu.
// MQTT nur zur Steuerung/Status: <präfix>/status (LWT online/offline), Einzelwerte unter
//   <präfix>/sgv, /delta, /direction, /age_min, /iob, /cob, /error, /power, /brightness, /night,
//   /night_active, /ip, /ssid, /rssi, /version (retained)
//   Befehle: <präfix>/set/brightness 0-255|default, /set/power ON|OFF, /set/night ON|OFF|AUTO,
//   /set/refresh, /set/reset (alles zurück auf Standardwerte).
// Gespeicherte Standardwerte kommen aus der Weboberfläche (http://<hostname>/); MQTT-Befehle und
// Tasten gelten nur bis zum nächsten Einstecken. Retained-Befehle werden ignoriert.
// Tasten: links kurz = Helligkeit, lang = Bild drehen
//         rechts kurz = Statusseite an/aus (IP, WLAN, MQTT …), 5 s halten = WLAN/Token neu einrichten
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <espMqttClient.h>
#include <time.h>
#include <math.h>

#include "lgfx_tdisplay_s3.h"
#include "certs.h"

#ifndef OTA_PASS
#define OTA_PASS ""
#endif

#ifndef GIT_COMMIT
#define GIT_COMMIT "?"
#endif

static const char *VERSION = "2.5";
static const int PIN_POWER = 15;  // schaltet Display (und Akku-Betrieb) ein
static const int PIN_BTN_L = 0;
static const int PIN_BTN_R = 14;
static const char *AP_NAME = "NS-Display";
static const uint32_t FETCH_MS = 60 * 1000;
static const uint32_t STATUS_MS = 60 * 60 * 1000;
static const uint8_t BTN_LEVELS[] = {255, 120, 40, 8};
static const int HIST_MAX = 150;  // reicht für 12 h
static const int WIFI_N = 3;
// Farben je Bereich: zu niedrig, außerhalb Ziel, zu hoch, im Ziel
static const uint32_t DEF_COL[4] = {0xFF2828, 0xFFD200, 0xFF7800, 0x28E66E};
static const char *COL_NAME[4] = {"unter Low", "ausserhalb Zielbereich", "ueber High", "im Zielbereich"};

LGFX lcd;
LGFX_Sprite spr(&lcd);
Preferences prefs;
WebServer web(80);
WiFiMulti wifiMulti;
espMqttClient mqtt(espMqttClientTypes::UseInternalTask::NO);

// ---------- Einstellungen (gespeichert = Standardwerte nach dem Einstecken) ----------

struct Settings {
  String url, token;
  String hostname, ntp, tz;
  String mqttHost, mqttUser, mqttPass, prefix;
  uint16_t mqttPort;
  uint8_t bright, nightBright, nightFrom, nightTo, rot;
  bool nightOn;
  uint16_t graphMin;
  bool ownTh;      // eigene Grenzwerte statt der aus Nightscout
  int16_t th[4];   // Low, Ziel unten, Ziel oben, High
  uint32_t col[4];
  String ssid[WIFI_N], pass[WIFI_N];
} cfg;

void loadPrefs() {
  prefs.begin("nsd", true);
  cfg.url = prefs.getString("url", "https://nightscout.example.org");
  cfg.token = prefs.getString("token", "");
  cfg.hostname = prefs.getString("host", "ns-display");
  cfg.ntp = prefs.getString("ntp", "fritz.box");
  cfg.tz = prefs.getString("tz", "CET-1CEST,M3.5.0,M10.5.0/3");
  cfg.mqttHost = prefs.getString("mhost", "192.168.1.10");
  cfg.mqttPort = prefs.getUShort("mport", 1883);
  cfg.mqttUser = prefs.getString("muser", "");
  cfg.mqttPass = prefs.getString("mpass", "");
  cfg.prefix = prefs.getString("prefix", "/raum1/display");
  cfg.bright = prefs.getUChar("bright", 255);
  cfg.nightOn = prefs.getBool("night", false);
  cfg.nightBright = prefs.getUChar("nbright", 10);
  cfg.nightFrom = prefs.getUChar("nfrom", 22);
  cfg.nightTo = prefs.getUChar("nto", 7);
  cfg.rot = prefs.getUChar("rot", 1);
  cfg.graphMin = prefs.getUShort("graph", 180);
  cfg.ownTh = prefs.getBool("ownth", false);
  const int16_t defTh[4] = {72, 80, 180, 250};
  for (int i = 0; i < 4; i++) {
    cfg.th[i] = prefs.getShort(("th" + String(i)).c_str(), defTh[i]);
    cfg.col[i] = prefs.getULong(("col" + String(i)).c_str(), DEF_COL[i]);
  }
  for (int i = 0; i < WIFI_N; i++) {
    cfg.ssid[i] = prefs.getString(("ssid" + String(i)).c_str(), "");
    cfg.pass[i] = prefs.getString(("wpass" + String(i)).c_str(), "");
  }
  prefs.end();
}

void savePrefs() {
  prefs.begin("nsd", false);
  prefs.putString("url", cfg.url);
  prefs.putString("token", cfg.token);
  prefs.putString("host", cfg.hostname);
  prefs.putString("ntp", cfg.ntp);
  prefs.putString("tz", cfg.tz);
  prefs.putString("mhost", cfg.mqttHost);
  prefs.putUShort("mport", cfg.mqttPort);
  prefs.putString("muser", cfg.mqttUser);
  prefs.putString("mpass", cfg.mqttPass);
  prefs.putString("prefix", cfg.prefix);
  prefs.putUChar("bright", cfg.bright);
  prefs.putBool("night", cfg.nightOn);
  prefs.putUChar("nbright", cfg.nightBright);
  prefs.putUChar("nfrom", cfg.nightFrom);
  prefs.putUChar("nto", cfg.nightTo);
  prefs.putUChar("rot", cfg.rot);
  prefs.putUShort("graph", cfg.graphMin);
  prefs.putBool("ownth", cfg.ownTh);
  for (int i = 0; i < 4; i++) {
    prefs.putShort(("th" + String(i)).c_str(), cfg.th[i]);
    prefs.putULong(("col" + String(i)).c_str(), cfg.col[i]);
  }
  for (int i = 0; i < WIFI_N; i++) {
    prefs.putString(("ssid" + String(i)).c_str(), cfg.ssid[i]);
    prefs.putString(("wpass" + String(i)).c_str(), cfg.pass[i]);
  }
  prefs.end();
}

// ---------- Laufzeitzustand (geht beim Ausstecken verloren) ----------

int brightOverride = -1;  // -1 = Standard/Nachtmodus
int8_t nightOverride = -1;  // -1 = nach Uhrzeit, 0 = aus, 1 = an
bool powerOn = true;
uint8_t rot;
uint8_t appliedBright = 0;

struct Entry {
  time_t t;
  int16_t sgv;
};
Entry hist[HIST_MAX];
int histN = 0;
String direction;
bool haveDelta = false;
int delta = 0;
float iob = NAN, cob = NAN;
int thLow = 72, thBottom = 80, thTop = 180, thHigh = 250;  // gültige Grenzwerte
int nsTh[4] = {72, 80, 180, 250};                           // zuletzt aus Nightscout

void applyThresholds() {
  const int *t = cfg.ownTh ? nullptr : nsTh;
  thLow = t ? t[0] : cfg.th[0];
  thBottom = t ? t[1] : cfg.th[1];
  thTop = t ? t[2] : cfg.th[2];
  thHigh = t ? t[3] : cfg.th[3];
}
String lastError, lastErrorDetail;  // Detail nur für MQTT/Weboberfläche
uint32_t lastFetch = 0, lastStatus = 0;
time_t lastFetchOk = 0;
bool statusOk = false;
bool stateDirty = false;
bool statusPage = false;
uint32_t statusSince = 0;
String mqttInfo = "noch nicht verbunden";

bool timeValid() { return time(nullptr) > 1700000000; }

bool nightActive() {
  if (nightOverride >= 0) return nightOverride;
  if (!cfg.nightOn || !timeValid()) return false;
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  int h = lt.tm_hour;
  return cfg.nightFrom <= cfg.nightTo ? (h >= cfg.nightFrom && h < cfg.nightTo)
                                      : (h >= cfg.nightFrom || h < cfg.nightTo);
}

uint8_t wantedBright() {
  if (!powerOn) return 0;
  if (brightOverride >= 0) return brightOverride;
  return nightActive() ? cfg.nightBright : cfg.bright;
}

void applyBright() {
  uint8_t b = wantedBright();
  if (b != appliedBright) {
    appliedBright = b;
    lcd.setBrightness(b);
    stateDirty = true;
  }
}

// ---------- Anzeige ----------

// RGB 0xRRGGBB -> Display-Farbe, f = Helligkeitsanteil in Prozent
uint16_t rgb(uint32_t c, int f = 100) {
  return lcd.color565(((c >> 16) & 0xFF) * f / 100, ((c >> 8) & 0xFF) * f / 100, (c & 0xFF) * f / 100);
}

uint16_t bgColor(int v) {
  int i = v < thLow ? 0 : v > thHigh ? 2 : (v < thBottom || v > thTop) ? 1 : 3;
  return rgb(cfg.col[i]);
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
  int span = cfg.graphMin;
  spr.fillRect(0, ymap(thTop), W, ymap(thBottom) - ymap(thTop) + 1, rgb(cfg.col[3], 18));
  for (int m = 60; m < span; m += 60)  // Stundenstriche
    spr.drawFastVLine(W - 4 - m * (W - 8) / span, y0, h, lcd.color565(35, 35, 35));
  spr.drawFastHLine(0, ymap(thLow), W, rgb(cfg.col[0], 50));
  spr.drawFastHLine(0, ymap(thHigh), W, rgb(cfg.col[2], 50));
  int r = span <= 240 ? 2 : 1;
  for (int i = 0; i < histN; i++) {
    long ageMin = (now - hist[i].t) / 60;
    if (ageMin < 0 || ageMin > span) continue;
    int x = W - 4 - ageMin * (W - 8) / span;
    spr.fillCircle(x, ymap(hist[i].sgv), r, bgColor(hist[i].sgv));
  }
}

void drawStatus() {
  char buf[64];
  spr.fillScreen(TFT_BLACK);
  spr.setTextDatum(top_left);
  spr.setFont(&fonts::FreeSansBold9pt7b);
  spr.setTextColor(TFT_WHITE);
  spr.drawString("Status", 6, 3);
  spr.setTextDatum(top_right);
  spr.setTextColor(TFT_DARKGREY);
  spr.drawString(String("v") + VERSION + "  " + GIT_COMMIT, 314, 3);

  spr.setTextDatum(top_left);
  spr.setFont(&fonts::FreeSansBold12pt7b);
  spr.setTextColor(WiFi.isConnected() ? lcd.color565(40, 230, 110) : TFT_RED);
  spr.drawString(WiFi.isConnected() ? WiFi.localIP().toString() : String("kein WLAN"), 6, 24);

  int y = 54;
  auto row = [&](const char *label, const String &val, uint16_t col) {
    spr.setFont(&fonts::FreeSans9pt7b);
    spr.setTextColor(TFT_DARKGREY);
    spr.drawString(label, 6, y);
    spr.setTextColor(col);
    spr.drawString(val, 92, y);
    y += 19;
  };
  row("Name", cfg.hostname, TFT_LIGHTGREY);
  snprintf(buf, sizeof(buf), " (%d dBm)", WiFi.RSSI());
  row("WLAN", WiFi.isConnected() ? WiFi.SSID() + buf : String("-"), TFT_LIGHTGREY);
  row("MQTT", mqtt.connected() ? cfg.prefix : (cfg.mqttHost.isEmpty() ? String("aus") : mqttInfo),
      mqtt.connected() ? TFT_LIGHTGREY : TFT_ORANGE);
  String ns = "-";
  if (lastFetchOk) {
    struct tm lt;
    localtime_r(&lastFetchOk, &lt);
    strftime(buf, sizeof(buf), "ok %H:%M:%S", &lt);
    ns = buf;
  }
  if (lastError.length()) ns = lastError;
  row("Nightscout", ns, lastError.length() ? TFT_RED : TFT_LIGHTGREY);
  uint32_t up = millis() / 60000;
  snprintf(buf, sizeof(buf), "%lud %luh %lum, %u kB frei", (unsigned long)(up / 1440),
           (unsigned long)(up / 60 % 24), (unsigned long)(up % 60), ESP.getFreeHeap() / 1024);
  row("Laufzeit", buf, TFT_LIGHTGREY);
  spr.pushSprite(0, 0);
}

void draw() {
  if (statusPage) {
    drawStatus();
    return;
  }
  time_t now = time(nullptr);
  bool timeOk = timeValid();
  struct tm lt;
  localtime_r(&now, &lt);

  spr.fillScreen(TFT_BLACK);

  // Verbindungsstatus: oben WLAN, unten MQTT (grau = nicht verbunden)
  spr.fillCircle(5, 7, 3, WiFi.isConnected() ? TFT_GREEN : TFT_RED);
  spr.fillCircle(5, 16, 3, mqtt.connected() ? TFT_GREEN : TFT_DARKGREY);

  // Kopfzeile: Uhr | IOB/COB | Alter bzw. Fehler
  char buf[48];
  spr.setFont(&fonts::FreeSansBold9pt7b);
  spr.setTextColor(TFT_LIGHTGREY);
  spr.setTextDatum(top_left);
  if (timeOk) {
    strftime(buf, sizeof(buf), "%H:%M", &lt);
    spr.drawString(buf, 13, 3);
  }
  String mid;
  if (!isnan(iob)) { snprintf(buf, sizeof(buf), "IOB %.1f", iob); mid += buf; }
  if (!isnan(cob)) { snprintf(buf, sizeof(buf), "   COB %.0f", cob); mid += buf; }
  spr.setTextDatum(top_center);
  spr.setTextColor(lcd.color565(110, 170, 255));
  spr.drawString(mid, 168, 3);

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
  String url = cfg.url + path + (path.indexOf('?') >= 0 ? "&" : "?") + "token=" + cfg.token;
  WiFiClientSecure tls;
  WiFiClient plain;
  HTTPClient http;
  http.useHTTP10(true);  // kein chunked encoding -> direkt aus dem Stream parsen
  http.setTimeout(10000);
  bool ok;
  if (cfg.url.startsWith("https")) {
    tls.setCACert(LE_ROOTS);
    ok = http.begin(tls, url);
  } else {
    ok = http.begin(plain, url);
  }
  if (!ok) { lastError = "URL?"; return false; }
  int code = http.GET();
  if (code != 200) {
    lastError = code == 401 ? "Token?" : code < 0 ? "Verbindung" : "HTTP " + String(code);
    if (code < 0) {
      char buf[100];
      int e = tls.lastError(buf, sizeof(buf));
      lastErrorDetail = String(code) + " " + http.errorToString(code) + (e ? " / TLS " + String(e) + " " + buf : "");
    }
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
  nsTh[0] = th["bgLow"] | nsTh[0];
  nsTh[1] = th["bgTargetBottom"] | nsTh[1];
  nsTh[2] = th["bgTargetTop"] | nsTh[2];
  nsTh[3] = th["bgHigh"] | nsTh[3];
  applyThresholds();
  statusOk = true;
}

void fetchData() {
  lastError = "";
  lastErrorDetail = "";
  if (!statusOk || millis() - lastStatus > STATUS_MS) {
    lastStatus = millis();
    fetchStatus();
  }

  JsonDocument filter, doc;
  filter[0]["sgv"] = true;
  filter[0]["date"] = true;
  filter[0]["direction"] = true;
  int count = min(HIST_MAX, cfg.graphMin / 5 + 6);
  if (!getJson("/api/v1/entries/sgv.json?count=" + String(count), doc, &filter)) return;
  histN = 0;
  for (JsonObject e : doc.as<JsonArray>()) {
    if (histN >= HIST_MAX) break;
    int sgv = e["sgv"] | 0;
    if (sgv < 20) continue;  // Sensorfehler-Codes
    hist[histN].t = (time_t)((e["date"] | 0.0) / 1000.0);
    hist[histN].sgv = sgv;
    if (histN == 0) direction = (const char *)(e["direction"] | "");
    histN++;
  }
  haveDelta = histN > 1 && hist[0].t - hist[1].t <= 7 * 60;
  if (haveDelta) delta = hist[0].sgv - hist[1].sgv;
  lastFetchOk = time(nullptr);

  JsonDocument p;
  iob = cob = NAN;
  if (getJson("/api/v2/properties/iob,cob", p)) {
    if (p["iob"]["iob"].is<float>()) iob = p["iob"]["iob"];
    if (p["cob"]["cob"].is<float>()) cob = p["cob"]["cob"];
  } else {
    lastError = "";  // IOB/COB sind optional
  }
  stateDirty = true;
}

// ---------- MQTT (nur Steuerung/Status) ----------

String topicStatus, topicSet;
uint32_t mqttRetryAt = 0, mqttBackoff = 5000;
bool fetchNow = false;

void pub(const char *name, const String &val) {
  mqtt.publish((cfg.prefix + "/" + name).c_str(), 0, true, val.c_str());
}

String fmt1(float v) { return isnan(v) ? "" : String(v, 2); }

void publishState() {
  if (!mqtt.connected()) return;
  bool fresh = histN && timeValid();
  pub("sgv", histN ? String(hist[0].sgv) : "");
  pub("delta", histN && haveDelta ? String(delta) : "");
  pub("direction", histN ? direction : "");
  pub("age_min", fresh ? String((long)((time(nullptr) - hist[0].t) / 60)) : "");
  pub("iob", fmt1(iob));
  pub("cob", fmt1(cob));
  pub("error", lastError);
  pub("error_detail", lastErrorDetail);
  { time_t t = time(nullptr); struct tm lt; localtime_r(&t, &lt); char b[24]; strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &lt); pub("time", b); }
  pub("power", powerOn ? "ON" : "OFF");
  pub("brightness", String(appliedBright));
  pub("night", nightOverride < 0 ? "AUTO" : nightOverride ? "ON" : "OFF");
  pub("night_active", nightActive() ? "ON" : "OFF");
  pub("ip", WiFi.localIP().toString());
  pub("gateway", WiFi.gatewayIP().toString());
  pub("dns", WiFi.dnsIP().toString());
  pub("subnet", WiFi.subnetMask().toString());
  pub("bssid", WiFi.BSSIDstr());
  pub("ssid", WiFi.SSID());
  pub("rssi", String(WiFi.RSSI()));
  pub("version", String(VERSION) + " " + GIT_COMMIT);
}

void onMqttMessage(const espMqttClientTypes::MessageProperties &props, const char *topic,
                   const uint8_t *payload, size_t len, size_t index, size_t total) {
  if (props.retain || index != 0 || len != total) return;  // retained = Altlast, nicht übernehmen
  String cmd = String(topic).substring(topicSet.length());
  String val;
  val.concat((const char *)payload, len);
  val.trim();
  String up = val;
  up.toUpperCase();

  if (cmd == "brightness") {
    brightOverride = up == "DEFAULT" ? -1 : constrain(val.toInt(), 0, 255);
    powerOn = true;
  } else if (cmd == "power") {
    powerOn = up != "OFF" && up != "0";
  } else if (cmd == "night") {
    nightOverride = up == "ON" ? 1 : up == "OFF" ? 0 : -1;
    brightOverride = -1;
  } else if (cmd == "refresh") {
    fetchNow = true;
  } else if (cmd == "reset") {
    brightOverride = -1;
    nightOverride = -1;
    powerOn = true;
  }
  stateDirty = true;
}

void mqttSetup() {
  if (cfg.mqttHost.isEmpty()) return;
  topicStatus = cfg.prefix + "/status";
  topicSet = cfg.prefix + "/set/";
  mqtt.setServer(cfg.mqttHost.c_str(), cfg.mqttPort);
  if (cfg.mqttUser.length()) mqtt.setCredentials(cfg.mqttUser.c_str(), cfg.mqttPass.c_str());
  mqtt.setClientId(cfg.hostname.c_str());
  mqtt.setWill(topicStatus.c_str(), 0, true, "offline");
  mqtt.onConnect([](bool) {
    mqtt.publish(topicStatus.c_str(), 0, true, "online");
    mqtt.subscribe((topicSet + "#").c_str(), 0);
    mqttBackoff = 5000;
    stateDirty = true;
  });
  mqtt.onMessage(onMqttMessage);
  mqtt.onDisconnect([](espMqttClientTypes::DisconnectReason r) {
    mqttInfo = String("Fehler ") + (int)r + " (" + cfg.mqttHost + ")";
  });
}

void mqttLoop() {
  if (cfg.mqttHost.isEmpty()) return;
  mqtt.loop();
  if (mqtt.connected() || !WiFi.isConnected() || millis() < mqttRetryAt) return;
  // in fremden WLANs ist der Broker nicht erreichbar -> seltener versuchen, Anzeige läuft weiter
  mqttRetryAt = millis() + mqttBackoff;
  mqttBackoff = min<uint32_t>(mqttBackoff * 2, 5 * 60 * 1000);
  mqtt.connect();
}

// ---------- Weboberfläche (Standardwerte) ----------

String esc(const String &s) {
  String o;
  for (char c : s) {
    if (c == '&') o += "&amp;";
    else if (c == '"') o += "&quot;";
    else if (c == '<') o += "&lt;";
    else o += c;
  }
  return o;
}

String field(const char *label, const char *name, const String &val, const char *type = "text",
             const char *hint = "") {
  return String("<label>") + label + "<input type=\"" + type + "\" name=\"" + name + "\" value=\"" +
         esc(val) + "\"><small>" + hint + "</small></label>";
}

void handleRoot() {
  String h;
  h.reserve(6000);
  h += F("<!doctype html><html lang=de><meta charset=utf-8>"
         "<meta name=viewport content='width=device-width,initial-scale=1'><title>");
  h += esc(cfg.hostname);
  h += F("</title><style>body{font-family:sans-serif;max-width:560px;margin:auto;padding:12px;"
         "background:#111;color:#ddd}fieldset{border:1px solid #444;margin:12px 0}label{display:block;"
         "margin:8px 0}input{display:block;width:100%;box-sizing:border-box;padding:6px;background:#222;"
         "color:#eee;border:1px solid #555}input[type=checkbox]{display:inline;width:auto}"
         "small{color:#888}button{padding:10px 20px}td{padding:2px 8px}</style><h2>");
  h += esc(cfg.hostname);
  h += F("</h2><table>");
  char buf[64];
  h += "<tr><td>Version</td><td>" + String(VERSION) + " (" + GIT_COMMIT + ")</td></tr>";
  h += "<tr><td>WLAN</td><td>" + esc(WiFi.SSID()) + " (" + WiFi.RSSI() + " dBm), " +
       WiFi.localIP().toString() + "</td></tr>";
  h += "<tr><td>MQTT</td><td>" + String(mqtt.connected() ? "verbunden" : "nicht verbunden") + "</td></tr>";
  if (lastFetchOk) {
    struct tm lt;
    localtime_r(&lastFetchOk, &lt);
    strftime(buf, sizeof(buf), "%d.%m. %H:%M:%S", &lt);
    h += "<tr><td>Letzter Abruf</td><td>" + String(buf) + "</td></tr>";
  }
  if (lastError.length()) h += "<tr><td>Fehler</td><td>" + esc(lastError) + "</td></tr>";
  h += F("</table><form method=post action=/save>");

  h += F("<fieldset><legend>WLANs (der Reihe nach probiert)</legend>");
  for (int i = 0; i < WIFI_N; i++) {
    String n = String(i + 1);
    h += field(("WLAN " + n).c_str(), ("ssid" + n).c_str(), cfg.ssid[i], "text",
               cfg.ssid[i].length() && cfg.ssid[i] == WiFi.SSID() ? "verbunden" : "leer = nicht benutzt");
    h += field(("Passwort " + n).c_str(), ("wpass" + n).c_str(), "", "password",
               cfg.pass[i].length() ? "gesetzt &ndash; leer lassen = unverändert" : "");
  }
  h += F("</fieldset><fieldset><legend>Nightscout</legend>");
  h += field("URL", "url", cfg.url);
  h += field("Token (Rolle readable)", "token", "", "password",
             cfg.token.length() ? "gesetzt &ndash; leer lassen = unverändert" : "noch nicht gesetzt");
  h += field("Verlauf (Minuten)", "graph", String(cfg.graphMin), "number", "60 bis 720");
  h += F("</fieldset><fieldset><legend>Grenzwerte (mg/dl)</legend>");
  h += cfg.ownTh ? F("<small>eigene Werte</small>") : F("<small>aus Nightscout</small>");
  const char *thName[4] = {"Low", "Ziel unten", "Ziel oben", "High"};
  const int cur[4] = {thLow, thBottom, thTop, thHigh};
  for (int i = 0; i < 4; i++)
    h += field(thName[i], ("th" + String(i)).c_str(), String(cur[i]), "number");
  h += F("<button formaction=/nsth>Aus Nightscout laden</button>");
  h += F("</fieldset><fieldset><legend>Farben</legend>");
  for (int i = 0; i < 4; i++) {
    snprintf(buf, sizeof(buf), "#%06lX", (unsigned long)cfg.col[i]);
    h += field(COL_NAME[i], ("col" + String(i)).c_str(), buf, "color");
  }
  h += F("<button formaction=/defcol>Standardfarben</button>");
  h += F("</fieldset><fieldset><legend>Anzeige</legend>");
  h += field("Helligkeit", "bright", String(cfg.bright), "number", "0 bis 255");
  h += String("<label><input type=checkbox name=night value=1") + (cfg.nightOn ? " checked" : "") +
       "> Nachtmodus</label>";
  h += field("Nacht ab (Stunde)", "nfrom", String(cfg.nightFrom), "number");
  h += field("Nacht bis (Stunde)", "nto", String(cfg.nightTo), "number");
  h += field("Helligkeit nachts", "nbright", String(cfg.nightBright), "number", "0 = aus");
  h += String("<label><input type=checkbox name=rot value=3") + (cfg.rot == 3 ? " checked" : "") +
       "> Bild um 180&deg; gedreht</label>";
  h += F("</fieldset><fieldset><legend>MQTT (nur Steuerung)</legend>");
  h += field("Broker", "mhost", cfg.mqttHost, "text", "leer = kein MQTT");
  h += field("Port", "mport", String(cfg.mqttPort), "number");
  h += field("Benutzer", "muser", cfg.mqttUser);
  h += field("Passwort", "mpass", "", "password", "leer lassen = unverändert");
  h += field("Topic-Präfix", "prefix", cfg.prefix, "text", "z.B. /raum1/display oder /raum2/display");
  h += F("</fieldset><fieldset><legend>System</legend>");
  h += field("Hostname", "host", cfg.hostname);
  h += field("NTP-Server", "ntp", cfg.ntp, "text", "zusätzlich immer pool.ntp.org");
  h += field("Zeitzone (POSIX)", "tz", cfg.tz, "text", "Deutschland: CET-1CEST,M3.5.0,M10.5.0/3");
  h += F("</fieldset><button>Speichern und neu starten</button></form></html>");
  web.send(200, "text/html; charset=utf-8", h);
}

void handleSave() {
  auto arg = [](const char *n) {
    String v = web.arg(n);
    v.trim();
    return v;
  };
  auto num = [&](const char *n, int lo, int hi, int def) {
    String v = arg(n);
    return v.length() ? constrain(v.toInt(), lo, hi) : def;
  };
  String u = arg("url");
  while (u.endsWith("/")) u.remove(u.length() - 1);
  if (u.length()) cfg.url = u;
  if (arg("token").length()) cfg.token = arg("token");
  cfg.graphMin = num("graph", 60, 720, cfg.graphMin);
  cfg.bright = num("bright", 0, 255, cfg.bright);
  cfg.nightOn = web.hasArg("night");
  cfg.nightFrom = num("nfrom", 0, 23, cfg.nightFrom);
  cfg.nightTo = num("nto", 0, 23, cfg.nightTo);
  cfg.nightBright = num("nbright", 0, 255, cfg.nightBright);
  cfg.rot = web.hasArg("rot") ? 3 : 1;
  cfg.mqttHost = arg("mhost");
  cfg.mqttPort = num("mport", 1, 65535, cfg.mqttPort);
  cfg.mqttUser = arg("muser");
  if (arg("mpass").length()) cfg.mqttPass = arg("mpass");
  if (cfg.mqttUser.isEmpty()) cfg.mqttPass = "";
  String p = arg("prefix");
  while (p.endsWith("/")) p.remove(p.length() - 1);
  if (p.length()) cfg.prefix = p;
  if (arg("host").length()) cfg.hostname = arg("host");
  if (arg("ntp").length()) cfg.ntp = arg("ntp");
  if (arg("tz").length()) cfg.tz = arg("tz");
  for (int i = 0; i < WIFI_N; i++) {
    String n = String(i + 1), ss = arg(("ssid" + n).c_str()), pw = arg(("wpass" + n).c_str());
    if (ss != cfg.ssid[i]) cfg.pass[i] = "";  // anderes Netz: altes Passwort gilt nicht
    cfg.ssid[i] = ss;
    if (pw.length()) cfg.pass[i] = pw;
    if (ss.isEmpty()) cfg.pass[i] = "";
  }
  int t[4];
  bool asc = true;
  for (int i = 0; i < 4; i++) {
    t[i] = num(("th" + String(i)).c_str(), 30, 400, 0);
    if (i && t[i] < t[i - 1]) asc = false;
  }
  if (asc && t[0] && (t[0] != thLow || t[1] != thBottom || t[2] != thTop || t[3] != thHigh)) {
    cfg.ownTh = true;  // nur wer etwas ändert, bekommt eigene Grenzwerte
    for (int i = 0; i < 4; i++) cfg.th[i] = t[i];
  }
  for (int i = 0; i < 4; i++) {
    String c = arg(("col" + String(i)).c_str());
    if (c.length() == 7 && c[0] == '#') cfg.col[i] = strtoul(c.c_str() + 1, nullptr, 16);
  }
  savePrefs();
  web.send(200, "text/html; charset=utf-8",
           "<!doctype html><meta charset=utf-8><meta http-equiv=refresh content='8;url=/'>"
           "Gespeichert, starte neu ...");
  delay(500);
  ESP.restart();
}

// ---------- WLAN-Einrichtung ----------

// neues WLAN vorne einsortieren (ältestes fällt raus, wenn alle 3 belegt sind)
void rememberWifi(const String &ssid, const String &pw) {
  if (ssid.isEmpty()) return;
  int at = WIFI_N - 1;
  for (int i = 0; i < WIFI_N; i++)
    if (cfg.ssid[i] == ssid) { at = i; break; }
  if (at == 0 && cfg.pass[0] == pw) return;
  for (int i = at; i > 0; i--) {
    cfg.ssid[i] = cfg.ssid[i - 1];
    cfg.pass[i] = cfg.pass[i - 1];
  }
  cfg.ssid[0] = ssid;
  cfg.pass[0] = pw;
  savePrefs();
}

bool connectKnown() {
  bool any = false;
  for (int i = 0; i < WIFI_N; i++)
    if (cfg.ssid[i].length()) {
      wifiMulti.addAP(cfg.ssid[i].c_str(), cfg.pass[i].c_str());
      any = true;
    }
  if (!any) return false;
  for (int tries = 0; tries < 3; tries++)
    if (wifiMulti.run(10000) == WL_CONNECTED) return true;
  return false;
}

void portal() {
  message("Einrichtung", "WLAN \"NS-Display\" verbinden,", "dann 192.168.4.1 aufrufen");
  WiFiManager wm;
  WiFiManagerParameter pUrl("nsurl", "Nightscout-URL", cfg.url.c_str(), 120);
  WiFiManagerParameter pTok("nstoken", "Zugriffstoken (Rolle readable)", cfg.token.c_str(), 100);
  wm.addParameter(&pUrl);
  wm.addParameter(&pTok);
  wm.setTitle("Nightscout-Display");
  wm.setHostname(cfg.hostname.c_str());
  wm.setConfigPortalTimeout(300);
  wm.setConnectTimeout(20);
  bool ok = wm.startConfigPortal(AP_NAME);

  String u = pUrl.getValue(), t = pTok.getValue();
  u.trim();
  t.trim();
  while (u.endsWith("/")) u.remove(u.length() - 1);
  if (u.length() && (u != cfg.url || t != cfg.token)) {
    cfg.url = u;
    cfg.token = t;
    savePrefs();
  }
  if (!ok) ESP.restart();  // Zeit abgelaufen: neu versuchen (vielleicht ist ein bekanntes WLAN zurück)
  rememberWifi(WiFi.SSID(), WiFi.psk());
}

void resetAll() {
  message("Zurueckgesetzt", "Neustart ...");
  WiFiManager wm;
  wm.resetSettings();
  cfg.token = "";
  for (int i = 0; i < WIFI_N; i++) cfg.ssid[i] = cfg.pass[i] = "";
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
Button btnL(PIN_BTN_L), btnR(PIN_BTN_R);

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

void nextBrightness() {
  // aus = einschalten, sonst nächste Stufe unterhalb der aktuellen
  if (!powerOn) { powerOn = true; return; }
  int cur = appliedBright, next = BTN_LEVELS[0];
  for (uint8_t l : BTN_LEVELS)
    if (l < cur) { next = l; break; }
  brightOverride = next;
}

// ---------- Ablauf ----------

void setup() {
  pinMode(PIN_POWER, OUTPUT);
  digitalWrite(PIN_POWER, HIGH);
  pinMode(PIN_BTN_L, INPUT_PULLUP);
  pinMode(PIN_BTN_R, INPUT_PULLUP);
  Serial.begin(115200);

  loadPrefs();
  applyThresholds();
  rot = cfg.rot;
  lcd.init();
  lcd.setRotation(rot);
  lcd.setBrightness(appliedBright = cfg.bright);
  spr.setPsram(true);
  spr.setColorDepth(16);
  spr.createSprite(320, 170);
  message("Nightscout", "Verbinde WLAN ...");

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(cfg.hostname.c_str());
  if (!connectKnown()) portal();
  if (cfg.token.isEmpty()) portal();
  WiFi.setAutoReconnect(true);

  configTzTime(cfg.tz.c_str(), cfg.ntp.c_str(), "pool.ntp.org");

  ArduinoOTA.setHostname(cfg.hostname.c_str());
  if (strlen(OTA_PASS)) ArduinoOTA.setPassword(OTA_PASS);
  ArduinoOTA.onStart([]() { message("Update ...", "nicht ausschalten"); });
  ArduinoOTA.begin();

  web.on("/", HTTP_GET, handleRoot);
  web.on("/save", HTTP_POST, handleSave);
  web.on("/nsth", HTTP_POST, []() {
    cfg.ownTh = false;
    savePrefs();
    statusOk = false;  // beim nächsten Abruf frisch aus Nightscout
    fetchNow = true;
    applyThresholds();
    web.sendHeader("Location", "/");
    web.send(303);
  });
  web.on("/defcol", HTTP_POST, []() {
    for (int i = 0; i < 4; i++) cfg.col[i] = DEF_COL[i];
    savePrefs();
    web.sendHeader("Location", "/");
    web.send(303);
  });
  web.begin();

  mqttSetup();

  message("Nightscout", "Hole Daten ...");
  fetchData();
  lastFetch = millis();
  applyBright();
  draw();
}

void loop() {
  static uint32_t wifiLostAt = 0;
  if (WiFi.isConnected()) {
    wifiLostAt = 0;
  } else if (!wifiLostAt) {
    wifiLostAt = millis();
  } else if (millis() - wifiLostAt > 30000) {
    wifiMulti.run(10000);  // bekannte WLANs durchprobieren
    wifiLostAt = millis();
  }

  ArduinoOTA.handle();
  web.handleClient();
  mqttLoop();

  int l = pollButton(btnL, 1500);
  if (l == 1) {
    nextBrightness();
  } else if (l == 2) {
    rot = rot == 1 ? 3 : 1;
    lcd.setRotation(rot);
  }

  int r = pollButton(btnR, 5000);
  if (r == 2) resetAll();
  if (r == 1) {
    statusPage = !statusPage;
    statusSince = millis();
    draw();
  }
  if (statusPage && millis() - statusSince > 30000) statusPage = false;  // von selbst zurück

  if (fetchNow || millis() - lastFetch >= FETCH_MS) {
    fetchNow = false;
    lastFetch = millis();
    if (WiFi.isConnected()) fetchData();
    else lastError = "WLAN";
    draw();
  }

  applyBright();
  if (stateDirty) {
    stateDirty = false;
    publishState();
  }

  static uint32_t lastDraw = 0;
  if (millis() - lastDraw >= 1000) {
    lastDraw = millis();
    draw();
  }
  delay(5);
}
