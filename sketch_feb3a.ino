#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "RTClib.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>


#define RST_PIN       4
#define SS_PIN        5
#define BUTTON_PIN    32
#define GREEN_LED_PIN 15
#define RED_LED_PIN   2
#define BUZZER_PIN    27
#define SDA_PIN       21
#define SCL_PIN       22
#define SCK_PIN       18
#define MOSI_PIN      23
#define MISO_PIN      19


/***** INITIALISATION OF INITIAL VARIABLES****/
MFRC522 mfrc522(SS_PIN, RST_PIN);
WebServer server(80);
RTC_DS3231 rtc;

DateTime now;

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

String server_route = "https://rfid.pythonanywhere.com/upload";

const char* CSV_PATH  = "/uids.csv";
const char* JSON_PATH = "/data.json";

unsigned long lastSync = 0;
bool wifiConnected = false;


const char* AP_SSID = "ESP32-RFID";
const char* AP_PASS = "12345678";

StaticJsonDocument<4096> main_users;
String lastScannedUID = "";
String pendingUID = "";
bool SaveMode = false;



void showMessage(String l1, String l2 = "", String l3 = "") {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 10);
  display.println(l1);
  if (l2.length()) display.println(l2);
  if (l3.length()) display.println(l3);
  display.display();
}


void startAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
}

void connectToWiFiIfConfigured() {
  showMessage("UPLOADING FILES...");
  if (!LittleFS.exists("/wifi.json")) {
    startAP();
    return;
  }

  File f = LittleFS.open("/wifi.json", FILE_READ);
  if (!f) {
    startAP();
    return;
  }

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, f)) {
    f.close();
    startAP();
    return;
  }
  f.close();

  const char* ssid = doc["ssid"];
  const char* pass = doc["pass"];

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(300);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("\nConnected to WiFi");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi failed. Falling back to AP mode.");
    startAP();
  }
}

void handleWifiPage() {
    String html = R"rawliteral(
  <!DOCTYPE html>
  <html>
  <head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>WiFi Setup</title>
  <style>
    body { font-family: system-ui, -apple-system, sans-serif; background: #0f172a; color: #e5e7eb; margin: 0; padding: 16px; display: flex; justify-content: center; min-height: 100vh; }
    .card { width: 100%; max-width: 400px; background: #020617; border-radius: 12px; padding: 24px; box-shadow: 0 10px 30px rgba(0,0,0,0.5); border: 1px solid #1f2933; }
    h2 { margin: 0 0 20px 0; color: #93c5fd; text-align: center; }
    label { display: block; margin-bottom: 8px; font-size: 14px; color: #94a3b8; font-weight: 500; }
    input { width: 100%; padding: 12px; margin-bottom: 20px; border-radius: 8px; border: 1px solid #334155; background: #1e293b; color: #f8fafc; font-size: 16px; box-sizing: border-box; }
    input:focus { outline: none; border-color: #3b82f6; }
    button { width: 100%; padding: 14px; background: #2563eb; color: white; border: none; border-radius: 8px; font-weight: 600; font-size: 16px; cursor: pointer; transition: background 0.2s; }
    button:hover { background: #1d4ed8; }
    a { display: block; text-align: center; margin-top: 20px; color: #64748b; text-decoration: none; font-size: 14px; }
    a:hover { color: #93c5fd; }
  </style>
  </head>
  <body>
    <div class="card">
      <h2>Connect to WiFi</h2>
      <form action="/saveWifi" method="POST">
        <label>SSID Name</label>
        <input name="ssid" placeholder="Enter WiFi Name" required>

        <label>Password</label>
        <input name="pass" type="password" placeholder="Enter WiFi Password">

        <button type="submit">Save & Connect</button>
      </form>
      <a href="/">Cancel & Back to Home</a>
    </div>
  </body>
  </html>
  )rawliteral";
    server.send(200, "text/html", html);
}

void handleSaveWifi() {
  if (!server.hasArg("ssid") || !server.hasArg("pass")) {
    server.send(400, "text/plain", "Missing credentials");
    return;
  }

  StaticJsonDocument<256> doc;
  doc["ssid"] = server.arg("ssid");
  doc["pass"] = server.arg("pass");

  File f = LittleFS.open("/wifi.json", FILE_WRITE);
  serializeJson(doc, f);
  f.close();

  server.send(200, "text/plain", "Saved. Rebooting...");
  delay(1000);
  ESP.restart();
}


bool uploadCSV(const String& path) {
  Serial.println("\n[UPLOAD] Starting CSV upload...");

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[UPLOAD]  WiFi not connected");
    showMessage("[UPLOAD]  WiFi not connected");
    delay(3000);
    return false;
  }

  Serial.print("[UPLOAD] WiFi OK. IP: ");
  Serial.println(WiFi.localIP());

  File file = LittleFS.open(path, FILE_READ);
  if (!file) {
    Serial.print("[UPLOAD]  Failed to open file: ");
    showMessage("[UPLOAD]  Failed to open file:", path);
    delay(3000);
    Serial.println(path);
    return false;
  }

  size_t size = file.size();
  Serial.print("[UPLOAD] File opened. Size: ");
  Serial.print(size);
  Serial.println(" bytes");

  if (size == 0) {
    Serial.println("[UPLOAD]  File is empty. Nothing to upload.");
    file.close();
    return false;
  }

  Serial.print("[UPLOAD] Server route: ");
  Serial.println(server_route);


  WiFiClientSecure client;
  client.setInsecure(); // <--- KEY LINE: Tells ESP32 to ignore SSL certificate checks

  HTTPClient http;

  if (!http.begin(client, server_route)) {
    Serial.println("[UPLOAD]  http.begin() failed");
    showMessage("[UPLOAD]  http.begin() failed");
    delay(3000);
    file.close();
    return false;
  }

  http.addHeader("Content-Type", "text/csv");

  Serial.println("[UPLOAD] Sending POST request...");
  int code = http.sendRequest("POST", &file, size);

  Serial.print("[UPLOAD] HTTP response code: ");
  Serial.println(code);

  if (code > 0) {
    Serial.println("[UPLOAD] Server response:");
    Serial.println(http.getString());
  } else {
    Serial.print("[UPLOAD]  HTTP error: ");
    Serial.println(http.errorToString(code));
  }

  http.end();
  file.close();

  return (code == 200);
}


bool isUploaded(const String& name) {
  if (!LittleFS.exists("/sync.json")) return false;

  File f = LittleFS.open("/sync.json", FILE_READ);
  StaticJsonDocument<512> doc;
  deserializeJson(doc, f);
  f.close();

  return doc[name] == true;
}

void markUploaded(const String& name) {
  StaticJsonDocument<512> doc;

  if (LittleFS.exists("/sync.json")) {
    File f = LittleFS.open("/sync.json", FILE_READ);
    deserializeJson(doc, f);
    f.close();
  }

  doc[name] = true;

  File f = LittleFS.open("/sync.json", FILE_WRITE);
  serializeJson(doc, f);
  f.close();
}

void markPending(const String& name) {
  StaticJsonDocument<512> doc;

  if (LittleFS.exists("/sync.json")) {
    File f = LittleFS.open("/sync.json", FILE_READ);
    deserializeJson(doc, f);
    f.close();
  }

  doc[name] = false;

  File f = LittleFS.open("/sync.json", FILE_WRITE);
  serializeJson(doc, f);
  f.close();
}


void wipeWifiAndRestart() {
  Serial.println("\n[SYSTEM] Sync complete! Deleting WiFi credentials...");
  
  if (LittleFS.exists("/wifi.json")) {
    LittleFS.remove("/wifi.json");
    Serial.println("[SYSTEM] WiFi credentials deleted.");
  }
  
  Serial.println("[SYSTEM] Rebooting into AP Mode...");
  delay(1000);
  ESP.restart();
}

void syncAllCSVs() {
  if (WiFi.status() != WL_CONNECTED) return;

  Serial.println("\n[SYNC] Checking for files to upload...");

  File root = LittleFS.open("/");
  File file = root.openNextFile();
  
  bool anyError = false;   // Track if any upload fails
  bool didWork = false;    // Track if we actually tried to upload something

  while (file) {
    String rawName = String(file.name());
    String filePath;
    String cleanName;

    // FIX: Standardize path to ensure it starts with "/"
    if (rawName.startsWith("/")) {
      filePath = rawName;
      cleanName = rawName.substring(1);
    } else {
      filePath = "/" + rawName;
      cleanName = rawName;
    }

    // Only process CSV files that are NOT logs or system files
    if (!file.isDirectory() && filePath.endsWith(".csv") && !filePath.equals("/uids.csv")) {
      
      // Check if this file has been uploaded yet
      if (!isUploaded(cleanName)) {
        didWork = true; // We found work to do
        Serial.print("[SYNC] Found pending file: ");
        Serial.println(filePath);

        if (uploadCSV(filePath)) {
          markUploaded(cleanName);
          Serial.println("[SYNC]  Upload success");
        } else {
          Serial.println("[SYNC]  Upload failed");
          anyError = true; // Mark that we had an error
        }
      }
    }
    file = root.openNextFile();
  }

  if (!anyError) {
    Serial.println("[SYNC] All files synced successfully.");
    showMessage("[SYNC] Successfully synced All files. Entering AP mode...");
    delay(3000);
  } else {
    Serial.println("[SYNC] Some files failed to upload. Retrying next loop.");
    showMessage("[SYNC] Failed to upload some files. Entering AP mode...");
    delay(3000);
  }

  wipeWifiAndRestart();
}


String twoDigit(int x) {
  if (x < 10) return "0" + String(x);
  return String(x);
}

String getDate() {
  return twoDigit(now.day()) + "/" + twoDigit(now.month()) + "/" + String(now.year());
}

String getTime() {
  return twoDigit(now.hour()) + ":" + twoDigit(now.minute()) + ":" + twoDigit(now.second());
}

String uidToString(MFRC522::Uid *uid) {
  String s = "";
  for (byte i = 0; i < uid->size; i++) {
    if (uid->uidByte[i] < 0x10) s += "0";
    s += String(uid->uidByte[i], HEX);
    if (i != uid->size - 1) s += ":";
  }
  s.toUpperCase();
  return s;
}

void loadJson() {
  if (!LittleFS.exists(JSON_PATH)) return;

  File file = LittleFS.open(JSON_PATH, "r");
  deserializeJson(main_users, file);
  file.close();
}

void addUser(String uid, String name, String mID) {
  StaticJsonDocument<1024> doc;

  if (LittleFS.exists(JSON_PATH)) {
    File f = LittleFS.open(JSON_PATH, "r");
    deserializeJson(doc, f);
    f.close();
  }

  JsonObject user = doc[uid].to<JsonObject>();
  user["name"] = name;
  user["mID"]  = mID;

  File f = LittleFS.open(JSON_PATH, "w");
  serializeJson(doc, f);
  f.close();

  loadJson();
}

void saveUID(const String &uid) {
  File file = LittleFS.open(CSV_PATH, FILE_APPEND);
  if (!file) return;
  file.print(main_users[uid]["mID"].as<const char*>());
  file.print(",");
  file.print(main_users[uid]["name"].as<const char*>());
  file.print(",");
  file.print(getDate());
  file.print(",");
  file.println(getTime());
  file.close();
}

String getSafeDateForFilename() {
  return String(now.year()) + "-" + twoDigit(now.month()) + "-" + twoDigit(now.day()) + "_Attendance.csv";
}

void MarkAttendance(const String &uid) {

  String path = "/" + getSafeDateForFilename();
  markPending(getSafeDateForFilename());

  if (!LittleFS.exists(path)) {
    File file = LittleFS.open(path, FILE_WRITE);
    if (!file) return;
    file.println("MEMBER_ID,NAME,DATE,TIME");
    file.close();
  }

  File file = LittleFS.open(path, FILE_APPEND);
  if (!file) return;

  file.print(main_users[uid]["mID"].as<const char*>());
  file.print(",");
  file.print(main_users[uid]["name"].as<const char*>());
  file.print(",");
  file.print(getDate());
  file.print(",");
  file.println(getTime());
  file.close();
  
}

void deleteUser(const String& uid) {
  StaticJsonDocument<1024> doc;

  if (!LittleFS.exists(JSON_PATH)) return;

  File f = LittleFS.open(JSON_PATH, "r");
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return;

  doc.remove(uid);

  f = LittleFS.open(JSON_PATH, "w");
  serializeJson(doc, f);
  f.close();

  loadJson(); // refresh RAM copy
}

void handleDeleteUserPage() {
  String html = R"rawliteral(
  <!DOCTYPE html>
  <html>
  <head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Delete Users</title>
  <style>
    body{font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;background:#0f172a;color:#e5e7eb;margin:0;padding:16px}
    .card{max-width:720px;margin:auto;background:#020617;border-radius:12px;padding:20px;box-shadow:0 10px 30px rgba(0,0,0,.35)}
    h2{margin:0 0 12px 0;color:#93c5fd}
    .row{display:flex;gap:8px;flex-wrap:wrap;align-items:center;justify-content:space-between;padding:10px;border:1px solid #1f2933;border-radius:8px;margin:8px 0;background:#020617}
    .meta{font-size:13px;opacity:.85}
    form{margin:0}
    button{padding:8px 12px;border-radius:6px;border:1px solid #7f1d1d;background:#7f1d1d;color:#fff}
    button:hover{filter:brightness(1.1)}
    a{color:#93c5fd;text-decoration:none;display:inline-block;margin-top:10px}
    .empty{opacity:.7}
  </style>
  </head>
  <body>
    <div class="card">
      <h2>Delete Users</h2>
  )rawliteral";

  if (main_users.size() == 0) {
    html += "<p class='empty'>No users found.</p><a href='/'>Back</a></div></body></html>";
    server.send(200, "text/html", html);
    return;
  }

  for (JsonPair kv : main_users.as<JsonObject>()) {
    String uid = kv.key().c_str();
    const char* name = kv.value()["name"] | "N/A";
    const char* mID  = kv.value()["mID"]  | "N/A";

    html += "<div class='row'><div class='meta'><b>" + uid + "</b><br>";
    html += String(name) + " | " + String(mID) + "</div>";

    html += R"rawliteral(
      <form action="/confirmDelete" method="POST">
        <input type="hidden" name="uid" value=")rawliteral" + uid + R"rawliteral(">
        <button type="submit">Delete</button>
      </form>
    )rawliteral";

    html += "</div>";
  }

  html += R"rawliteral(
    <a href="/">Back</a>
  </div>
  </body>
  </html>
  )rawliteral";

  server.send(200, "text/html", html);
}

void handleConfirmDelete() {
  if (!server.hasArg("uid")) {
    server.send(400, "text/plain", "Missing UID");
    return;
  }

  String uid = server.arg("uid");

  if (!main_users.containsKey(uid)) {
    server.send(404, "text/plain", "User not found");
    return;
  }

  deleteUser(uid);

  String html = R"rawliteral(
  <!DOCTYPE html>
  <html>
  <head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>User Deleted</title>
  <style>
    body{font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;background:#0f172a;color:#e5e7eb;margin:0;padding:16px}
    .card{max-width:520px;margin:auto;background:#020617;border-radius:12px;padding:20px;box-shadow:0 10px 30px rgba(0,0,0,.35);text-align:center}
    h2{margin:0 0 8px 0;color:#f87171}
    p{opacity:.85;margin-bottom:16px}
    a{display:inline-block;text-decoration:none;color:#e5e7eb;background:#111827;padding:10px 14px;border-radius:8px;margin:6px;border:1px solid #1f2933}
    a.primary{background:#2563eb;border-color:#2563eb}
    a:hover{filter:brightness(1.05)}
  </style>
  </head>
  <body>
    <div class="card">
      <h2>User Deleted</h2>
      <p>The selected user has been removed from the system.</p>
      <a class="primary" href="/deleteUser">Back to list</a>
      <a href="/">Home</a>
    </div>
  </body>
  </html>
  )rawliteral";

  server.send(200, "text/html", html);
}


void handleRoot() {
  SaveMode = false;
  String html = R"rawliteral(
  <!DOCTYPE html>
  <html>
  <head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32 RFID</title>
  <style>
    body{font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;background:#0f172a;color:#e5e7eb;margin:0;padding:16px}
    .card{max-width:520px;margin:auto;background:#020617;border-radius:12px;padding:20px;box-shadow:0 10px 30px rgba(0,0,0,.35)}
    h2{margin:0 0 12px 0;color:#93c5fd}
    a{display:block;text-decoration:none;color:#e5e7eb;background:#111827;padding:12px;border-radius:8px;margin:8px 0;border:1px solid #1f2933}
    a:hover{background:#0b1220}
    .muted{opacity:.7;font-size:12px;margin-top:12px}
  </style>
  </head>
  <body>
    <div class="card">
      <h2>ESP32 RFID</h2>
      <a href="/download">Download CSV</a>
      <a href="/wifi">Configure WiFi</a>
      <a href="/json">View JSON</a>
      <a href="/addUser">Add User</a>
      <a href="/deleteUser">Delete User</a>
      <div class="muted">AP mode dashboard</div>
    </div>
  </body>
  </html>
  )rawliteral";

  server.send(200, "text/html", html);
}

void handleAddUser() {
  SaveMode = true;
  pendingUID = "";

  String html = R"rawliteral(
  <!DOCTYPE html>
  <html>
  <head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Add User</title>
  <style>
    body{font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;background:#0f172a;color:#e5e7eb;margin:0;padding:16px}
    .card{max-width:520px;margin:auto;background:#020617;border-radius:12px;padding:20px;box-shadow:0 10px 30px rgba(0,0,0,.35)}
    h2{margin:0 0 8px 0;color:#93c5fd}
    p{opacity:.8}
    label{display:block;margin:12px 0 6px}
    input{width:100%;padding:12px;border-radius:8px;border:1px solid #1f2933;background:#020617;color:#e5e7eb}
    button{margin-top:12px;width:100%;padding:12px;border-radius:8px;border:none;background:#2563eb;color:#fff;font-weight:600}
    button:hover{filter:brightness(1.05)}
    .note{margin-top:12px;padding:10px;border-left:3px solid #2563eb;background:#020617}
    a{color:#93c5fd;text-decoration:none;display:inline-block;margin-top:10px}
  </style>
  </head>
  <body>
    <div class="card">
      <h2>Add User</h2>
      <p>1) Scan RFID card<br>2) Enter details and submit</p>
      <form action="/saveUser" method="POST">
        <label>Name</label>
        <input type="text" name="name" required>
        <label>Member ID</label>
        <input type="text" name="mID" required>
        <button type="submit">Save User</button>
      </form>
      <div class="note">Scan the card before submitting</div>
      <a href="/">Back</a>
    </div>
  </body>
  </html>
  )rawliteral";

  server.send(200, "text/html", html);
}


void handleSaveUser() {
  auto sendPage = [&](const String& title, const String& msg, bool success) {
    String color = success ? "#22c55e" : "#f87171";
    String html = R"rawliteral(
  <!DOCTYPE html>
  <html>
  <head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>)rawliteral" + title + R"rawliteral(</title>
  <style>
    body{font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;background:#0f172a;color:#e5e7eb;margin:0;padding:16px}
    .card{max-width:520px;margin:auto;background:#020617;border-radius:12px;padding:20px;box-shadow:0 10px 30px rgba(0,0,0,.35);text-align:center}
    h2{margin:0 0 8px 0;color:)rawliteral" + color + R"rawliteral(}
    p{opacity:.85;margin-bottom:16px}
    a{display:inline-block;text-decoration:none;color:#e5e7eb;background:#111827;padding:10px 14px;border-radius:8px;margin:6px;border:1px solid #1f2933}
    a.primary{background:#2563eb;border-color:#2563eb}
    a:hover{filter:brightness(1.05)}
  </style>
  </head>
  <body>
    <div class="card">
      <h2>)rawliteral" + title + R"rawliteral(</h2>
      <p>)rawliteral" + msg + R"rawliteral(</p>
      <a class="primary" href="/">Home</a>
      <a href="/addUser">Add Another</a>
    </div>
  </body>
  </html>
  )rawliteral";

      server.send(success ? 200 : 400, "text/html", html);
  };

  if (!server.hasArg("name") || !server.hasArg("mID")) {
    sendPage("Missing Fields", "Please fill in all required fields.", false);
    return;
  }

  if (pendingUID == "") {
    sendPage("No Card Scanned", "Scan the RFID card before submitting the form.", false);
    return;
  }

  if (main_users.containsKey(pendingUID)) {
    sendPage("Duplicate UID", "This card is already registered.", false);
    return;
  }

  addUser(pendingUID, server.arg("name"), server.arg("mID"));
  SaveMode = false;
  pendingUID = "";

  sendPage("User Added", "The user was added successfully.", true);
}

void handleJson() {
  File file = LittleFS.open(JSON_PATH, FILE_READ);
  if (!file) {
    server.send(404, "text/plain", "JSON not found");
    return;
  }
  server.streamFile(file, "application/json");
  file.close();
}


void handleDownload() {
  String html = R"rawliteral(
  <!DOCTYPE html>
  <html>
  <head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Download CSV</title>
  <style>
    body{font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;background:#0f172a;color:#e5e7eb;margin:0;padding:16px}
    .card{max-width:720px;margin:auto;background:#020617;border-radius:12px;padding:20px;box-shadow:0 10px 30px rgba(0,0,0,.35)}
    h2{color:#93c5fd;margin-bottom:12px}
    .row{display:flex;justify-content:space-between;align-items:center;padding:10px;border:1px solid #1f2933;border-radius:8px;margin:8px 0}
    .left{display:flex;flex-direction:column}
    .name{font-weight:600}
    .status{font-size:12px;color:#94a3b8}
    a.btn{background:#2563eb;color:#fff;padding:6px 10px;border-radius:6px;text-decoration:none;margin-left:6px}
    a.del{background:#7f1d1d}
  </style>
  </head>
  <body>
  <div class="card">
  <h2>Attendance CSV Files</h2>
  )rawliteral";

  File root = LittleFS.open("/");
  File file = root.openNextFile();

  bool found = false;

  while (file) {
    String rawName = String(file.name());
    
    String path; 
    String cleanName;

    if (rawName.startsWith("/")) {
      path = rawName;
      cleanName = rawName.substring(1);
    } else {
      path = "/" + rawName;
      cleanName = rawName;
    }
    
    if (!file.isDirectory() && path.indexOf("Attendance") >= 0 && path.endsWith(".csv")) {
      
      bool uploaded = isUploaded(cleanName);
      String status = uploaded ? "Uploaded" : "Pending";
      found = true;

      html += "<div class='row'>";
      html +=   "<div class='left'>";
      html +=     "<div class='name'>" + cleanName + "</div>";
      html +=     "<div class='status'>" + status + "</div>";
      html +=   "</div>";
      html +=   "<div>";
      html +=     "<a class='btn' href='/get?file=" + cleanName + "'>Download</a>";
      html +=     "<a class='btn del' href='/deleteCsv?file=" + cleanName + "'>Delete</a>";
      html +=   "</div>";
      html += "</div>";
    }
    file = root.openNextFile();
  }

  if (!found) {
    html += "<p>No attendance files found.</p>";
  }

  html += R"rawliteral(
  <a href="/">Back</a>
  </div>
  </body>
  </html>
  )rawliteral";

  server.send(200, "text/html", html);
}


void handleGetCSV() {
  if (!server.hasArg("file")) {
    server.send(400, "text/plain", "Missing file");
    return;
  }

  String path = "/" + server.arg("file");

  if (!LittleFS.exists(path)) {
    server.send(404, "text/plain", "File not found");
    return;
  }

  File file = LittleFS.open(path, FILE_READ);
  server.streamFile(file, "text/csv");
  file.close();
}

void handleDeleteCSV() {
  if (!server.hasArg("file")) {
    server.send(400, "text/plain", "Missing file");
    return;
  }

  String path = "/" + server.arg("file");

  if (!LittleFS.exists(path)) {
    server.send(404, "text/plain", "File not found");
    return;
  }

  LittleFS.remove(path);
  server.sendHeader("Location", "/download");
  server.send(303);
}

void verifyUID(String uid) {
  if (main_users.containsKey(uid)) {
    const char* name = main_users[uid]["name"];
    const char* mID  = main_users[uid]["mID"];
    showMessage("Welcome", name, mID);
    digitalWrite(GREEN_LED_PIN, HIGH);
    tone(BUZZER_PIN, 2000, 120);
    MarkAttendance(uid);
    delay(300);
    digitalWrite(GREEN_LED_PIN, LOW);
  } else {
    showMessage("Access Denied", uid);
    digitalWrite(RED_LED_PIN, HIGH);
    tone(BUZZER_PIN, 400, 200);
    delay(300);
    digitalWrite(RED_LED_PIN, LOW);
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  Wire.begin(SDA_PIN, SCL_PIN);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  showMessage("Booting...");

  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);
  mfrc522.PCD_Init();

  if (!rtc.begin()){
    Serial.println("Could not connect with RTC.");
    while (1);
  }

  if (rtc.lostPower()){
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  if (!LittleFS.begin(false)) {
    Serial.println("LittleFS failed");
    while (1);
  }

  if (!LittleFS.exists(CSV_PATH)) {
    File f = LittleFS.open(CSV_PATH, FILE_WRITE);
    f.println("UID");
    f.close();
  }

  if (!LittleFS.exists(JSON_PATH)) {
    File f = LittleFS.open(JSON_PATH, FILE_WRITE);
    f.print("{}");
    f.close();
  }

  loadJson();

  connectToWiFiIfConfigured();
  //startAP();

  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/download", handleDownload);
  server.on("/json", handleJson);
  server.on("/addUser", handleAddUser);
  server.on("/saveUser", HTTP_POST, handleSaveUser);
  server.on("/deleteUser", handleDeleteUserPage);
  server.on("/confirmDelete", HTTP_POST, handleConfirmDelete);
  server.on("/get", handleGetCSV);
  server.on("/deleteCsv", handleDeleteCSV);
  server.on("/wifi", handleWifiPage);
  server.on("/saveWifi", HTTP_POST, handleSaveWifi);

  server.begin();

  showMessage("READY! Scan Card", "AP IP: ", WiFi.softAPIP().toString());
}

void loop() {
  now = rtc.now();
  server.handleClient();

  if (WiFi.status() == WL_CONNECTED && millis() - lastSync > 10000) {
    Serial.println("Called for saving CSV");
    syncAllCSVs();   
    lastSync = millis();
  }

  if (!mfrc522.PICC_IsNewCardPresent()) return;
  if (!mfrc522.PICC_ReadCardSerial()) return;

  
 
  lastScannedUID = uidToString(&mfrc522.uid);
  Serial.println(lastScannedUID);

  

  if (!SaveMode) {
    verifyUID(lastScannedUID);
  } else {
    pendingUID = lastScannedUID;
    showMessage("Scanned UID:", pendingUID);
    digitalWrite(GREEN_LED_PIN, HIGH);
    tone(BUZZER_PIN, 1200, 80);
    delay(200);
    digitalWrite(GREEN_LED_PIN, LOW);
  }

  mfrc522.PICC_HaltA();
  delay(800);
}
