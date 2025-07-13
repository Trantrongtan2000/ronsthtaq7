#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <AsyncOta.h> // Thư viện AsyncOta của Mikael Tulldahl
#include <HTTPClient.h> // Thư viện này cần thiết cho ntfy.sh

// --- Cấu hình ntfy.sh ---
const char* NTPY_TOPIC = "ronsthtaq7"; 
const char* NTPY_SERVER_URL = "https://ntfy.sh/";

// --- Cấu hình Chân GPIO ---
const int INPUT_PIN = 1; // Chân D1 trên ESP32-C3 thường là GPIO1

// --- Cấu hình WiFi ---
const char *hostname = "myEsp32";
struct WiFiCredentials {
    const char* ssid;
    const char* password;
};

WiFiCredentials wifiList[] = {
    {"TRONG TAN", "trongtan2000"},
    {"TAMANH STAFF", "2Bphoquang"},
    {"TAMANH STAFF", "2bphoquang"}
};
const int numWifiNetworks = sizeof(wifiList) / sizeof(wifiList[0]);

// --- Các biến trạng thái ---
int lastPinState = LOW;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;
bool notificationSent = false;

// Khởi tạo đối tượng Web Server
AsyncWebServer server(80);

// Hàm gửi thông báo đến ntfy.sh
void sendNtfyNotification(String title, String message, String tags = "") {
    if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        
        String fullUrl = String(NTPY_SERVER_URL) + String(NTPY_TOPIC);
        http.begin(fullUrl);

        http.addHeader("Title", title);
        http.addHeader("Priority", "default");
        if (tags != "") {
            http.addHeader("Tags", tags);
        } else {
            http.addHeader("Tags", "bell");
        }

        Serial.print("Sending to ntfy.sh URL: ");
        Serial.println(fullUrl);
        Serial.print("Message: ");
        Serial.println(message);

        int httpResponseCode = http.POST(message);

        if (httpResponseCode > 0) {
            Serial.print("HTTP Response code: ");
            Serial.println(httpResponseCode);
            String response = http.getString();
            Serial.println("ntfy.sh Response: " + response);
        } else {
            Serial.print("Error sending ntfy.sh notification. HTTP error code: ");
            Serial.println(httpResponseCode);
        }
        http.end();
    } else {
        Serial.println("WiFi not connected. Cannot send ntfy.sh notification.");
    }
}

void connectToWiFi() {
    Serial.println("Attempting to connect to WiFi...");
    WiFi.mode(WIFI_STA);

    for (int i = 0; i < numWifiNetworks; i++) {
        Serial.printf("Trying to connect to '%s'...\n", wifiList[i].ssid);
        WiFi.begin(wifiList[i].ssid, wifiList[i].password);

        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 40) {
            delay(500);
            Serial.print(".");
            attempts++;
        }
        Serial.println();

        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("Connected to '%s'!\n", wifiList[i].ssid);
            Serial.print("IP address: ");
            Serial.println(WiFi.localIP());

            // Gửi thông báo khi kết nối mạng thành công
            String ipMessage = "ESP32-C3 đã kết nối mạng tại " + WiFi.localIP().toString();
            sendNtfyNotification("ESP32-C3 Online", ipMessage, "wifi");

            return;
        } else {
            Serial.printf("Failed to connect to '%s'.\n", wifiList[i].ssid);
            WiFi.disconnect();
        }
    }

    Serial.println("Could not connect to any specified WiFi network. Restarting...");
    delay(5000);
    ESP.restart();
}

// --- Callback cho sự kiện OTA ---
// (Các hàm này vẫn tồn tại nhưng sẽ không được gọi tự động bởi thư viện AsyncOta của Mikael Tulldahl)
void onOTAStart() {
    Serial.println("OTA Update: Start");
    // sendNtfyNotification("ESP32-C3 OTA", "Bắt đầu cập nhật firmware qua OTA.", "wrench");
}

void onOTAEnd() {
    Serial.println("OTA Update: End");
    // sendNtfyNotification("ESP32-C3 OTA", "Cập nhật firmware OTA hoàn tất. Thiết bị sẽ khởi động lại.", "tada");
}

void onOTAProgress(size_t current, size_t total) {
    Serial.printf("OTA Update: Progress: %u of %u bytes\n", current, total);
}

void onOTAError(int error) {
    Serial.printf("OTA Update: Error[%u]: ", error);
    String errorMessage = "Unknown Error";
    if (error == 1) errorMessage = "Auth Failed";
    else if (error == 2) errorMessage = "Begin Failed";
    else if (error == 3) errorMessage = "Connect Failed";
    else if (error == 4) errorMessage = "Receive Failed";
    else if (error == 5) errorMessage = "End Failed";
    Serial.println(errorMessage);
    // sendNtfyNotification("ESP32-C3 OTA Error", "Cập nhật firmware OTA thất bại: " + errorMessage, "skull");
}

void setup() {
    Serial.begin(115200);
    Serial.println("\nBooting...");

    pinMode(INPUT_PIN, INPUT_PULLUP);
    lastPinState = digitalRead(INPUT_PIN);

    connectToWiFi();

    if (!MDNS.begin(hostname)) {
        Serial.println("Error setting up mDNS responder!");
    } else {
        Serial.printf("mDNS responder started at http://%s.local\n", hostname);
    }

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        String html = "<html><body>";
        html += "<h1>ESP32-C3 OTA Update & ntfy.sh Alert</h1>";
        html += "<p>Connected to WiFi. IP: " + WiFi.localIP().toString() + "</p>";
        html += "<p>Access via mDNS: http://" + String(hostname) + ".local</p>";
        html += "<p>To update firmware, visit: <a href=\"/ota\">http://" + String(hostname) + ".local/ota</a></p>";
        html += "<p>GPIO" + String(INPUT_PIN) + " is being monitored for signal changes.</p>";
        html += "</body></html>";
        request->send(200, "text/html", html);
    });

    // --- SỬA ĐỔI QUAN TRỌNG: Loại bỏ các dòng đăng ký callback không tương thích ---
    // Thư viện AsyncOta của Mikael Tulldahl chỉ cần AsyncOTA.begin(&server);
    // Nó không hỗ trợ đăng ký callback chi tiết như các thư viện OTA khác.
    AsyncOTA.begin(&server); // Đây là dòng duy nhất cần thiết cho AsyncOta

    Serial.println("OTA update endpoint available at /ota");

    server.begin();
    Serial.println("HTTP server started");
    Serial.println("Access web interface at http://" + WiFi.localIP().toString() + " or http://" + String(hostname) + ".local");
}

void loop() {
    int currentReading = digitalRead(INPUT_PIN);

    if (currentReading != lastPinState) {
        lastDebounceTime = millis();
    }

    if ((millis() - lastDebounceTime) > debounceDelay) {
        if (currentReading != lastPinState) {
            if (currentReading == LOW) {
                if (!notificationSent) {
                    String message = "ESP32-C3: Tín hiệu được phát hiện trên chân D1 (GPIO" + String(INPUT_PIN) + ")!";
                    String title = "Cảnh báo từ ESP32-C3";
                    Serial.println(message);
                    
                    sendNtfyNotification(title, message, "warning");
                    notificationSent = true;
                }
            } else {
                notificationSent = false;
                Serial.println("Tín hiệu trên chân D1 (GPIO" + String(INPUT_PIN) + ") đã trở lại bình thường.");
            }
            lastPinState = currentReading;
        }
    }
    
    delay(10);
}