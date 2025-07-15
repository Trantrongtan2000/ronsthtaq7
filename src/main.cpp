/****************************************************************************************
 *  DỰ ÁN: GIÁM SÁT NHIỀU TÍN HIỆU GPIO, GỬI NTFY.SH VÀ LOG LÊN THINGSPEAK
 *  Phiên bản: 3.3 - Thống nhất logic xử lý trạng thái
 *
 *  Chức năng:
 *  - Sử dụng struct để quản lý cấu hình cho từng kênh một cách chuyên nghiệp.
 *  - Tái cấu trúc logic trong hàm loop() để xử lý tất cả các kênh một cách đồng nhất,
 *    loại bỏ lỗi gửi thông báo lặp lại.
 *  - Giữ nguyên tất cả các chức năng cốt lõi: đa kênh, logic ngược, OTA, ThingSpeak...
 *
 ****************************************************************************************/

// Thư viện cần thiết
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ElegantOTA.h>

// --- CẤU HÌNH CHÍNH ---
const char* NTPY_TOPIC = "ronsthtaq7";
const char* NTPY_SERVER_URL = "https://ntfy.sh";
const char* hostname = "myEsp32";

// --- CẤU HÌNH THINGSPEAK ---
const char* THINGSPEAK_API_KEY = "2NU97VXBB1B9J0DI";
const unsigned long THINGSPEAK_UPDATE_INTERVAL = 20000UL;

// --- CẤU HÌNH KÊNH GIÁM SÁT (QUAN TRỌNG NHẤT) ---
struct ChannelConfig {
    int pin;                // Chân GPIO
    bool invertedLogic;     // true: Cảnh báo khi HIGH, false: Cảnh báo khi LOW
    const char* alertTitle;       // Tiêu đề khi có cảnh báo
    const char* alertMessage;     // Nội dung khi có cảnh báo
    const char* alertTags;        // Icon khi có cảnh báo
    const char* normalTitle;      // Tiêu đề khi trở lại bình thường
    const char* normalMessage;    // Nội dung khi trở lại bình thường
    const char* normalTags;       // Icon khi trở lại bình thường
};

const ChannelConfig channels[] = {
    {1, true, "TRÀN NƯỚC HỆ THỐNG", "Tràn nước!!! Hãy lên kiểm tra ngay.", "error", "Hệ thống khôi phục", "...", "white_check_mark"},
    {2, false, "Giám sát hệ thống RO NSTH", "Đang chờ bơm nước RO.", "potable_water", "Giám sát hệ thống RO NSTH", "Nước RO đầy", "no_entry_sign"},
    {3, false, "Giám sát hệ thống RO NSTH", "Hết nước RO!!", "warning", "Giám sát hệ thống RO NSTH", "Đang chờ bơm nước RO.", "potable_water"}
};
const int NUM_PINS = sizeof(channels) / sizeof(channels[0]);

// Danh sách WiFi
struct WiFiCredentials { const char* ssid; const char* password; };
WiFiCredentials wifiList[] = { {"TAMANH STAFF", "2Bphoqu@ng"}, {"TRONG TAN", "trongtan2000"} };
const int numWifiNetworks = sizeof(wifiList) / sizeof(wifiList[0]);

// Cấu hình Analog và Cooldown
const int ANALOG_THRESHOLD_LOW = 500;
const int ANALOG_THRESHOLD_HIGH = 1800;
const unsigned long notificationCooldown = 5000;

// --- CÁC BIẾN TOÀN CỤC ---
int lastLogicalStates[NUM_PINS]; // 1 = BÌNH THƯỜNG, 0 = CẢNH BÁO
unsigned long lastNotificationTimes[NUM_PINS];
unsigned long lastThingSpeakTime = 0;

WebServer server(80);

// --- CÁC HÀM CHỨC NĂNG ---

/**
 * @brief Gửi một thông báo đến topic đã cấu hình trên ntfy.sh.
 */
void sendNtfyNotification(String title, String message, String tags) {
    if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        String fullUrl = String(NTPY_SERVER_URL) + "/" + String(NTPY_TOPIC);
        http.begin(fullUrl);
        http.addHeader("Title", title);
        http.addHeader("Priority", "default");
        http.addHeader("Tags", tags.isEmpty() ? "bell" : tags);
        int httpResponseCode = http.POST(message);
        if (httpResponseCode < 0) {
            Serial.printf("[ntfy] Lỗi khi gửi thông báo: %s\n", http.errorToString(httpResponseCode).c_str());
        }
        http.end();
    }
}

/**
 * @brief Gửi trạng thái logic của tất cả các kênh lên ThingSpeak.
 */
void sendToThingSpeak(int states[]) {
    if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        String url = "https://api.thingspeak.com/update?api_key=";
        url += THINGSPEAK_API_KEY;
        for (int i = 0; i < NUM_PINS; i++) {
            url += "&field" + String(i + 1) + "=" + String(states[i]);
        }
        http.begin(url);
        int httpResponseCode = http.GET();
        if (httpResponseCode < 0) {
            Serial.printf("[ThingSpeak] Lỗi khi gửi dữ liệu, mã lỗi: %d (%s)\n", httpResponseCode, http.errorToString(httpResponseCode).c_str());
        }
        http.end();
    }
}

/**
 * @brief Duyệt qua danh sách wifiList và cố gắng kết nối.
 */
void connectToWiFi() {
    Serial.println("Bắt đầu kết nối WiFi...");
    WiFi.mode(WIFI_STA);
    for (int i = 0; i < numWifiNetworks; i++) {
        Serial.printf("Đang thử kết nối vào mạng '%s'...\n", wifiList[i].ssid);
        WiFi.begin(wifiList[i].ssid, wifiList[i].password);
        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 20) {
            delay(500);
            Serial.print(".");
            attempts++;
        }
        Serial.println();
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("Đã kết nối vào mạng '%s'!\n", wifiList[i].ssid);
            Serial.print("Địa chỉ IP: ");
            Serial.println(WiFi.localIP());
            sendNtfyNotification("ESP32-C3 Online", "Thiết bị giám sát đa kênh đã online", "desktop_computer");
            return;
        } else {
            Serial.printf("Không thể kết nối vào '%s'.\n", wifiList[i].ssid);
            WiFi.disconnect(true);
            delay(100);
        }
    }
    Serial.println("Không kết nối được WiFi. Khởi động lại sau 5 giây...");
    delay(5000);
    ESP.restart();
}


// --- CHƯƠNG TRÌNH CHÍNH ---
void setup() {
    Serial.begin(115200);
    Serial.println("\n\n==============================================");
    Serial.println("   DỰ ÁN GIÁM SÁT - PHIÊN BẢN 3.3 (LOGIC MỚI)");
    Serial.println("==============================================");

    connectToWiFi();

    for (int i = 0; i < NUM_PINS; i++) {
        pinMode(channels[i].pin, INPUT);
        int physicalState = (analogRead(channels[i].pin) < ANALOG_THRESHOLD_LOW) ? 0 : 1; // 0=LOW, 1=HIGH
        // Chuyển đổi trạng thái vật lý thành trạng thái logic (1=Bình thường, 0=Cảnh báo)
        lastLogicalStates[i] = (channels[i].invertedLogic) ? !physicalState : physicalState;
        lastNotificationTimes[i] = 0;
        Serial.printf("Kênh %d (GPIO %d) - Trạng thái logic ban đầu: %s\n", i + 1, channels[i].pin, (lastLogicalStates[i] == 1 ? "BÌNH THƯỜNG" : "CẢNH BÁO"));
    }

    if (!MDNS.begin(hostname)) { Serial.println("Lỗi cài đặt mDNS!"); }

    server.on("/", []() {
        String html = "<html><head><meta charset='UTF-8'></head><body style='font-family: sans-serif; text-align: center;'>";
        html += "<h1>ESP32-C3 Giám sát và OTA</h1>";
        html += "<h2>(Giám sát " + String(NUM_PINS) + " kênh)</h2>";
        html += "<p><b>Địa chỉ IP:</b> " + WiFi.localIP().toString() + "</p>";
        html += "<table border='1' style='margin: auto;'><tr><th>Kênh (GPIO)</th><th>Trạng thái</th></tr>";
        for (int i = 0; i < NUM_PINS; i++) {
            String statusText = (lastLogicalStates[i] == 0) ? "<b style='color:red;'>" + String(channels[i].alertTitle) + "</b>" : String(channels[i].normalTitle);
            html += "<tr><td>" + String(channels[i].pin) + "</td><td>" + statusText + "</td></tr>";
        }
        html += "</table>";
        html += "<p style='margin-top: 20px;'><a href='/update' style='padding: 10px 20px; background-color: #007bff; color: white; text-decoration: none; border-radius: 5px;'>Cập nhật Firmware (OTA)</a></p>";
        html += "</body></html>";
        server.send(200, "text/html; charset=utf-8", html);
    });

    ElegantOTA.begin(&server);
    server.begin();
    Serial.println("Máy chủ HTTP và OTA đã bắt đầu.");
    Serial.println("----------------------------------------------");
}

void loop() {
    server.handleClient();
    ElegantOTA.loop();

    // --- Logic giám sát thống nhất cho tất cả các kênh ---
    for (int i = 0; i < NUM_PINS; i++) {
        const auto& channel = channels[i];
        int currentAnalogValue = analogRead(channel.pin);
        int currentLogicalState = lastLogicalStates[i];

        // Xác định trạng thái vật lý hiện tại
        int physicalState = -1; // -1 nghĩa là đang ở vùng không xác định
        if (currentAnalogValue < ANALOG_THRESHOLD_LOW) {
            physicalState = 0; // LOW
        } else if (currentAnalogValue > ANALOG_THRESHOLD_HIGH) {
            physicalState = 1; // HIGH
        }

        if (physicalState != -1) {
            // Chuyển đổi trạng thái vật lý thành trạng thái logic (1=Bình thường, 0=Cảnh báo)
            currentLogicalState = (channel.invertedLogic) ? !physicalState : physicalState;

            // Chỉ hành động KHI CÓ SỰ THAY ĐỔI trạng thái logic
            if (currentLogicalState != lastLogicalStates[i]) {
                if (millis() - lastNotificationTimes[i] > notificationCooldown) {
                    if (currentLogicalState == 0) { // Chuyển sang trạng thái CẢNH BÁO
                        Serial.printf("Kênh %d (GPIO %d) chuyển sang CẢNH BÁO. Gửi thông báo...\n", i + 1, channel.pin);
                        sendNtfyNotification(channel.alertTitle, channel.alertMessage, channel.alertTags);
                    } else { // Chuyển sang trạng thái BÌNH THƯỜNG
                        Serial.printf("Kênh %d (GPIO %d) trở về BÌNH THƯỜNG. Gửi thông báo...\n", i + 1, channel.pin);
                        sendNtfyNotification(channel.normalTitle, channel.normalMessage, channel.normalTags);
                    }
                    lastNotificationTimes[i] = millis();
                }
                // QUAN TRỌNG: Cập nhật trạng thái logic đã lưu để tránh lặp lại
                lastLogicalStates[i] = currentLogicalState;
            }
        }
    }

    // Gửi dữ liệu lên ThingSpeak định kỳ
    if (millis() - lastThingSpeakTime > THINGSPEAK_UPDATE_INTERVAL) {
        Serial.println("Đã đến lúc gửi trạng thái các kênh lên ThingSpeak...");
        sendToThingSpeak(lastLogicalStates);
        lastThingSpeakTime = millis();
    }

    delay(20);
}