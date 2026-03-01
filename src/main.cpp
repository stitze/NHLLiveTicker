#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <SPI.h>
#include <time.h>
#include <WiFiClientSecure.h>

const char *WIFI_SSID = "Galaxy S22 C172";
const char *WIFI_PASSWORD = "offe3495";

const unsigned long  FETCH_INTERVAL_MS = 60000;
const int            MATRIX_INTENSITY  = 3;
const int            SCROLL_SPEED_MS   = 40;
const int            MAX_LOOKBACK_DAYS = 7;

#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES   4
#define CS_PIN        5

MD_Parola        matrix(HARDWARE_TYPE, CS_PIN, MAX_DEVICES);
WiFiClientSecure secureClient;

String           displayText    = "Loading NHL...";
bool             pendingText    = false;
bool             fetchRequested = false;
SemaphoreHandle_t textMutex;
unsigned long    lastFetch      = 0;


void syncTime() {
  configTime(0, 0, "pool.ntp.org");
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();
  struct tm ti;
  while (!getLocalTime(&ti)) delay(500);
}

String dateAtEasternOffset(int dayOffset) {
  time_t t = time(nullptr) - (5 * 3600) + (dayOffset * 86400);
  struct tm eastern;
  gmtime_r(&t, &eastern);
  char buf[11];
  strftime(buf, sizeof(buf), "%Y-%m-%d", &eastern);
  return String(buf);
}

String todayNHL() {
  return dateAtEasternOffset(0);
}

int localUtcOffsetMinutes() {
  time_t now = time(nullptr);
  struct tm local_tm, utc_tm;
  localtime_r(&now, &local_tm);
  gmtime_r(&now, &utc_tm);
  int offset = (local_tm.tm_hour - utc_tm.tm_hour) * 60
             + (local_tm.tm_min  - utc_tm.tm_min);
  if (offset >  720) offset -= 1440;
  if (offset < -720) offset += 1440;
  return offset;
}

String utcTimestampToLocal(const String& utcStr) {
  int tIdx = utcStr.indexOf('T');
  if (tIdx < 0) return "??:??";
  int hh    = utcStr.substring(tIdx + 1, tIdx + 3).toInt();
  int mm    = utcStr.substring(tIdx + 4, tIdx + 6).toInt();
  int total = ((hh * 60 + mm + localUtcOffsetMinutes()) % 1440 + 1440) % 1440;
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", total / 60, total % 60);
  return String(buf);
}

void applyJsonFilter(JsonDocument& filter) {
  filter["gameDate"] = true;
  JsonObject g = filter["games"].add<JsonObject>();
  g["gameState"]                      = true;
  g["startTimeUTC"]                   = true;
  g["period"]                         = true;
  g["awayTeam"]["abbrev"]             = true;
  g["awayTeam"]["score"]              = true;
  g["homeTeam"]["abbrev"]             = true;
  g["homeTeam"]["score"]              = true;
  g["clock"]["timeRemaining"]         = true;
  g["periodDescriptor"]["periodType"] = true;
}

bool fetchScoreDocument(const String& date, JsonDocument& doc) {
  HTTPClient http;
  http.begin(secureClient, "https://api-web.nhle.com/v1/score/" + date);
  http.addHeader("User-Agent", "ESP32-NHL-Matrix/1.0");
  http.setTimeout(10000);

  int code = http.GET();
  Serial.println("GET " + date + " -> " + code);

  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  Serial.println("Payload bytes: " + String(payload.length()));
  if (payload.length() == 0) return false;

  JsonDocument filter;
  applyJsonFilter(filter);

  DeserializationError err = deserializeJson(doc, payload,
                               DeserializationOption::Filter(filter));
  if (err) {
    Serial.println("JSON error: " + String(err.c_str()));
    return false;
  }

  Serial.println("Games found: " + String(doc["games"].as<JsonArray>().size()));
  return true;
}

String suffixForFinishedGame(JsonObject game) {
  const char* periodType = game["periodDescriptor"]["periodType"] | "REG";
  if (strcmp(periodType, "OT") == 0) return " OT";
  if (strcmp(periodType, "SO") == 0) return " SO";
  return "";
}

String formatGame(JsonObject game, bool showStartTime) {
  const char* away  = game["awayTeam"]["abbrev"];
  const char* home  = game["homeTeam"]["abbrev"];
  int         awayS = game["awayTeam"]["score"] | 0;
  int         homeS = game["homeTeam"]["score"] | 0;
  const char* state = game["gameState"];

  bool isScheduled = strcmp(state, "FUT") == 0 || strcmp(state, "PRE") == 0;
  bool isLive      = strcmp(state, "LIVE") == 0 || strcmp(state, "CRIT") == 0;

  if (showStartTime && isScheduled) {
    String t = utcTimestampToLocal(game["startTimeUTC"] | String("??:??"));
    return String(away) + " vs " + home + " (" + t + ")";
  }

  String result = String(away) + " " + awayS + "-" + homeS + " " + home;

  if (isLive) {
    int         period = game["period"] | 0;
    const char* clock  = game["clock"]["timeRemaining"] | "?";
    result += " P" + String(period) + " " + clock;
  } else {
    result += suffixForFinishedGame(game);
  }

  return result;
}

String buildScrollText(JsonDocument& doc, bool showStartTime) {
  const char* gameDate = doc["gameDate"] | "";
  String date = (strlen(gameDate) > 0) ? String(gameDate) : todayNHL();
  String text = date + "   ";
  for (JsonObject game : doc["games"].as<JsonArray>()) {
    text += formatGame(game, showStartTime) + "   ";
  }
  return text;
}

bool hasFinishedGames(JsonDocument& doc) {
  for (JsonObject game : doc["games"].as<JsonArray>()) {
    const char* state = game["gameState"];
    if (strcmp(state, "OFF") == 0 || strcmp(state, "FINAL") == 0) return true;
  }
  return false;
}

String lastGameDayText() {
  for (int d = -1; d >= -MAX_LOOKBACK_DAYS; d--) {
    String date = dateAtEasternOffset(d);
    Serial.println("Checking: " + date);
    JsonDocument doc;
    if (!fetchScoreDocument(date, doc)) continue;
    if (doc["games"].as<JsonArray>().size() == 0) continue;
    if (!hasFinishedGames(doc)) continue;
    return buildScrollText(doc, false);
  }
  return "No recent games found";
}

String buildFullDisplayText() {
  JsonDocument doc;

  if (!fetchScoreDocument(todayNHL(), doc)) return "API error";

  Serial.println("Free heap: " + String(ESP.getFreeHeap()));

  JsonArray games = doc["games"].as<JsonArray>();

  if (games.size() == 0) {
    return "No games today   Last results: " + lastGameDayText();
  }

  if (!hasFinishedGames(doc)) {
    return buildScrollText(doc, true) + "  Last results: " + lastGameDayText();
  }

  return buildScrollText(doc, true);
}

void fetchTask(void* param) {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    String newText = buildFullDisplayText();

    xSemaphoreTake(textMutex, portMAX_DELAY);
    displayText = newText;
    pendingText = true;
    xSemaphoreGive(textMutex);

    Serial.println("Display (" + String(newText.length()) + "): " + newText);
  }
}

TaskHandle_t fetchTaskHandle;

void triggerFetch() {
  xTaskNotifyGive(fetchTaskHandle);
}

void connectWifi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) delay(500);
  Serial.println("WiFi connected: " + WiFi.localIP().toString());
}

void setup() {
  Serial.begin(115200);

  secureClient.setInsecure();
  textMutex = xSemaphoreCreateMutex();

  matrix.begin();
  matrix.setIntensity(MATRIX_INTENSITY);
  matrix.setTextAlignment(PA_LEFT);
  matrix.displayScroll(displayText.c_str(), PA_LEFT, PA_SCROLL_LEFT, SCROLL_SPEED_MS);

  connectWifi();
  syncTime();

  xTaskCreatePinnedToCore(
    fetchTask,       // Task-Funktion
    "fetchTask",     // Name
    16384,           // Stack (16 KB – nötig für TLS + JSON)
    nullptr,         // Parameter
    1,               // Priorität
    &fetchTaskHandle,
    0                // Core 0 (Loop läuft auf Core 1)
  );

  triggerFetch();
}

void loop() {
  if (matrix.displayAnimate()) {
    xSemaphoreTake(textMutex, portMAX_DELAY);
    bool hasPending = pendingText;
    xSemaphoreGive(textMutex);

    if (hasPending) {
      xSemaphoreTake(textMutex, portMAX_DELAY);
      matrix.displayScroll(displayText.c_str(), PA_LEFT, PA_SCROLL_LEFT, SCROLL_SPEED_MS);
      pendingText = false;
      xSemaphoreGive(textMutex);
    } else {
      matrix.displayReset();
    }
  }

  if (millis() - lastFetch >= FETCH_INTERVAL_MS) {
    lastFetch = millis();
    if (WiFi.status() == WL_CONNECTED) {
      triggerFetch();
    } else {
      WiFi.reconnect();
    }
  }
}
