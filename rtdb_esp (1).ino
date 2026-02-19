#include <WiFi.h>
#include <HTTPUpdate.h>
#include <WebServer.h>
#include <Firebase_ESP_Client.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <esp_task_wdt.h>

// ─── CONFIG ───────────────────────────────────────────────────────────────────
#define WIFI_SSID           "MATH JUNIOR-2.4G"
#define WIFI_PASSWORD       "mj123mj123"
#define API_KEY             "AIzaSyBOyzONDFL2qHui_Rx4j8-pWivheg5MVdE"
#define FIREBASE_PROJECT_ID "esp32-3f190"
#define DATABASE_URL        "https://esp32-3f190-default-rtdb.asia-southeast1.firebasedatabase.app/"
#define USER_EMAIL          "esp@yourdomain.com"
#define USER_PASSWORD       "456789"
#define FIRMWARE_BUILD      "OTA-Poller-v1"  // Increment on every binary export
#define WDT_TIMEOUT_SEC     60
// ──────────────────────────────────────────────────────────────────────────────

Preferences  preferences;
WebServer    server(80);

FirebaseData streamData;  // RTDB stream only
FirebaseData writeData;   // RTDB writes only
FirebaseData fsData;      // Firestore reads only
FirebaseAuth auth;
FirebaseConfig config;

// ─── STATE ────────────────────────────────────────────────────────────────────
float  current_version = 0.0;
float  last_acted      = -1.0;   // last version we fully processed
String last_not_found  = "";     // suppress repeated "not found" messages

// ota_pending is set BEFORE executeOTA() is called — prevents race condition
// where stream reconnect check fires between handleVersionLogic() and executeOTA()
volatile bool  ota_pending      = false;
volatile bool  stream_triggered = false;
volatile float stream_new_value = -1.0;

String ota_url     = "";
float  ota_version = 0.0;

unsigned long last_reconnect_ms = 0;  // throttle stream reconnect to once per 30s
// ──────────────────────────────────────────────────────────────────────────────

// ─── STREAM CALLBACK ──────────────────────────────────────────────────────────
// Fires only when /firmware/target_version changes — keep this fast, no blocking
void streamCallback(FirebaseStream data) {
  float val = -1.0;

  if      (data.dataType() == "int"    ||
           data.dataType() == "number") val = (float)data.intData();
  else if (data.dataType() == "float")  val = data.floatData();
  else if (data.dataType() == "string") val = data.stringData().toFloat();

  if (val >= 0) {
    stream_new_value = val;
    stream_triggered = true;
  }
}

void streamTimeoutCallback(bool timeout) {
  if (timeout) Serial.println("[STREAM] Timeout — library will auto-reconnect.");
}
// ──────────────────────────────────────────────────────────────────────────────

void beginStream() {
  Firebase.RTDB.beginStream(&streamData, "/firmware/target_version");
  Firebase.RTDB.setStreamCallback(&streamData, streamCallback, streamTimeoutCallback);
  Serial.println("[STREAM] Listening on /firmware/target_version");
  esp_task_wdt_reset();
}

// ─── VERSION LOGIC ────────────────────────────────────────────────────────────
void handleVersionLogic(float targetVer) {
  esp_task_wdt_reset();

  if (targetVer < 0) return;

  // Already handled this exact version — stay silent
  if (targetVer == last_acted) {
    esp_task_wdt_reset();
    return;
  }

  // Already on this version — nothing to do
  if (targetVer == current_version) {
    Serial.printf("[VERSION] Already on v%.1f — up to date.\n", current_version);
    last_acted = targetVer;
    esp_task_wdt_reset();
    return;
  }

  // New version detected — search Firestore
  Serial.printf("[VERSION] Update needed: v%.1f -> v%.1f — checking Firestore...\n",
                current_version, targetVer);

  String searchKey = "v" + String(targetVer, 1);
  searchKey.replace(".", "_");

  esp_task_wdt_reset();

  bool fetched = Firebase.Firestore.getDocument(
    &fsData, FIREBASE_PROJECT_ID, "", "firmware/latest", ""
  );

  esp_task_wdt_reset();

  if (!fetched) {
    Serial.println("[ERROR] Firestore fetch failed: " + fsData.errorReason());
    // Don't mark last_acted — retry on next stream trigger
    esp_task_wdt_reset();
    return;
  }

  String payload = fsData.payload();
  esp_task_wdt_reset();

  if (payload.indexOf("\"" + searchKey + "\"") != -1) {
    // Version found — extract download URL
    int uStart = payload.indexOf("http",
                   payload.indexOf("url",
                     payload.indexOf("\"" + searchKey + "\"")));
    int uEnd   = payload.indexOf("\"", uStart);
    ota_url    = payload.substring(uStart, uEnd);
    ota_url.replace("\\", "");
    ota_version    = targetVer;
    last_acted     = targetVer;
    last_not_found = "";

    Serial.printf("[OTA] v%.1f found. URL: %s\n", targetVer, ota_url.c_str());

    // Set flag here — BEFORE returning to loop() — to prevent race condition
    ota_pending = true;

  } else {
    // Not in Firestore — report once only, then silence
    String keyStr = String(targetVer, 1);
    if (last_not_found != keyStr) {
      last_not_found = keyStr;
      Serial.printf("[ERROR] v%.1f not found in Firestore. Add it to the database.\n", targetVer);
    }
    // Don't set last_acted — retry is possible if user adds the entry later
  }

  esp_task_wdt_reset();
}
// ──────────────────────────────────────────────────────────────────────────────

// ─── OTA ──────────────────────────────────────────────────────────────────────
void executeOTA() {
  Serial.printf("\n[OTA] Starting: v%.1f -> v%.1f\n", current_version, ota_version);
  Serial.println("[OTA] URL: " + ota_url);

  server.stop();
  delay(300);

  // Disable watchdog before the blocking download
  // Reason: httpUpdate.update() blocks for 20-90s during binary download+flash
  // WDT firing mid-flash causes corrupted firmware and crash loops
  esp_task_wdt_delete(NULL);
  esp_task_wdt_deinit();

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(90);
  httpUpdate.rebootOnUpdate(false);

  t_httpUpdate_return ret = httpUpdate.update(client, ota_url);

  if (ret == HTTP_UPDATE_OK) {
    Serial.printf("[OTA] Success — saving v%.1f and rebooting...\n", ota_version);

    preferences.begin("ota_prefs", false);
    preferences.putString("version", String(ota_version, 1));
    preferences.end();

    Firebase.RTDB.setFloat(&writeData, "/firmware/active_version", ota_version);
    delay(300);
    ESP.restart();

  } else {
    Serial.printf("[OTA] Failed (code %d): %s\n",
      httpUpdate.getLastError(),
      httpUpdate.getLastErrorString().c_str());

    // Re-enable WDT since device continues running
    esp_task_wdt_init(WDT_TIMEOUT_SEC, true);
    esp_task_wdt_add(NULL);
    esp_task_wdt_reset();

    ota_pending = false;
    Serial.println("[OTA] Restarting after failure...");
    delay(2000);
    ESP.restart();
  }
}
// ──────────────────────────────────────────────────────────────────────────────

// ─── SETUP ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n=============================");
  Serial.println("[BUILD] " + String(FIRMWARE_BUILD));
  Serial.println("=============================\n");

  esp_task_wdt_init(WDT_TIMEOUT_SEC, true);
  esp_task_wdt_add(NULL);
  esp_task_wdt_reset();

  // Load version saved from last OTA
  preferences.begin("ota_prefs", false);
  current_version = preferences.getString("version", "1.0").toFloat();
  preferences.end();
  Serial.printf("[SYSTEM] Current version: v%.1f\n", current_version);

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WiFi] Connecting");
  unsigned long wStart = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
    esp_task_wdt_reset();
    if (millis() - wStart > 20000) {
      Serial.println("\n[WiFi] Timeout — restarting...");
      ESP.restart();
    }
  }
  Serial.println("\n[WiFi] Connected: " + WiFi.localIP().toString());
  esp_task_wdt_reset();

  // Firebase
  config.api_key      = API_KEY;
  config.database_url = DATABASE_URL;
  auth.user.email     = USER_EMAIL;
  auth.user.password  = USER_PASSWORD;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  Serial.print("[Firebase] Authenticating");
  unsigned long fStart = millis();
  while (!Firebase.ready()) {
    delay(300);
    Serial.print(".");
    esp_task_wdt_reset();
    if (millis() - fStart > 30000) {
      Serial.println("\n[Firebase] Auth timeout — restarting...");
      ESP.restart();
    }
  }
  Serial.println("\n[Firebase] Ready");
  esp_task_wdt_reset();

  // Report current version as a number to RTDB
  Firebase.RTDB.setFloat(&writeData, "/firmware/active_version", current_version);
  Serial.printf("[Firebase] active_version reported: %.1f\n", current_version);
  esp_task_wdt_reset();

  beginStream();

  server.on("/", []() {
    server.send(200, "text/plain",
      "Build:   " + String(FIRMWARE_BUILD) + "\n" +
      "Version: " + String(current_version, 1) + "\n");
  });
  server.begin();

  Serial.println("\n--- SYSTEM READY ---");
  Serial.printf("Version : v%.1f\n", current_version);
  Serial.println("Build   : " + String(FIRMWARE_BUILD));
  Serial.println("Waiting for target_version changes...\n");
  esp_task_wdt_reset();
}
// ──────────────────────────────────────────────────────────────────────────────

// ─── LOOP ─────────────────────────────────────────────────────────────────────
void loop() {
  server.handleClient();
  esp_task_wdt_reset();

  // Stream reconnect — only when NOT doing OTA, max once every 30s
  if (!ota_pending) {
    unsigned long now = millis();
    if (!streamData.httpConnected() && (now - last_reconnect_ms > 30000)) {
      last_reconnect_ms = now;
      Serial.println("[STREAM] Reconnecting...");
      beginStream();
    }
  }

  // New stream value arrived — process it
  if (stream_triggered && !ota_pending) {
    stream_triggered = false;
    handleVersionLogic(stream_new_value);
  }

  // OTA queued — run it
  if (ota_pending) {
    executeOTA();
  }

  delay(20);
  esp_task_wdt_reset();
}
// ──────────────────────────────────────────────────────────────────────────────
