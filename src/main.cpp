/****************************************************************************************
 * DỰ ÁN: GIÁM SÁT NHIỀU TÍN HIỆU GPIO, GỬI NTFY.SH VÀ LOG LÊN THINGSPEAK
 * Phiên bản: 4.4 - Hoàn chỉnh tính năng giám sát, log cảnh báo và xuất báo cáo CSV
 *
 ****************************************************************************************/

#include <Arduino.h>

// Thư viện cần thiết
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ElegantOTA.h>
#include "time.h" // Thư viện để làm việc với thời gian NTP

// --- CẤU HÌNH NTP (Đồng bộ thời gian thực) ---
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 7 * 3600; // Offset cho múi giờ GMT+7 (Hồ Chí Minh)
const int daylightOffset_sec = 0;    // Không có giờ mùa hè

// --- CẤU HÌNH NTFY.SH ---
const char* NTPY_TOPIC = "ronsthtaq7";
const char* NTPY_SERVER_URL = "https://ntfy.sh";
const char* hostname = "myEsp32";

// --- CẤU HÌNH THINGSPEAK ---
const char* THINGSPEAK_API_KEY = "2NU97VXBB1B9J0DI";
const unsigned long THINGSPEAK_UPDATE_INTERVAL = 20000UL; // 20 giây

// --- CẤU HÌNH KÊNH GIÁM SÁT (QUAN TRỌNG NHẤT) ---
struct ChannelConfig {
    int pin;                 // Chân GPIO
    bool invertedLogic;      // true: Cảnh báo khi HIGH, false: Cảnh báo khi LOW
    const char* alertTitle;      // Tiêu đề khi có cảnh báo
    const char* alertMessage;    // Nội dung khi có cảnh báo
    const char* alertTags;       // Icon khi có cảnh báo
    const char* normalTitle;     // Tiêu đề khi trở lại bình thường
    const char* normalMessage;   // Nội dung khi trở lại bình thường
    const char* normalTags;      // Icon khi trở lại bình thường
};

const ChannelConfig channels[] = {
    {21, true, "TRÀN NƯỚC HỆ THỐNG", "Tràn nước!!! Hãy lên kiểm tra ngay.", "error", "Hệ thống khôi phục!", "...", "white_check_mark"},
    {22, false, "Giám sát hệ thống RO NSTH", "Nước RO đầy", "no_entry_sign", "Giám sát hệ thống RO NSTH", "Đang chờ bơm nước RO.", "potable_water"},
    {23, false, "Giám sát hệ thống RO NSTH", "Hết nước RO!!", "warning", "Giám sát hệ thống RO NSTH", "Đèn báo hết nước RO đã tắt. Hãy kiểm tra lại hệ thống và bơm nước RO.", "potable_water"}
};
const int NUM_PINS = sizeof(channels) / sizeof(channels[0]);

// Danh sách WiFi
struct WiFiCredentials { const char* ssid; const char* password; };
WiFiCredentials wifiList[] = { {"TAMANH STAFF", "2Bphoqu@ng"}, {"TRONG TAN", "trongtan2000"} };
const int numWifiNetworks = sizeof(wifiList) / sizeof(wifiList[0]);

// Cấu hình Cooldown
const unsigned long notificationCooldown = 5000; // 5 giây giữa các thông báo cùng kênh

// --- CÁC BIẾN TOÀN CỤC ---
int lastLogicalStates[NUM_PINS]; // 1 = BÌNH THƯỜNG, 0 = CẢNH BÁO
unsigned long lastNotificationTimes[NUM_PINS];
unsigned long lastThingSpeakTime = 0;

// Biến lưu trữ lịch sử cảnh báo
struct AlertLog {
    unsigned long bootTimeMillis; // Thời gian kể từ lúc boot (dùng cho debug/nội bộ)
    time_t realTime;              // Thời gian thực (nếu đã đồng bộ NTP)
    int channelIndex;             // Kênh nào gây ra sự kiện
    bool isAlert;                 // true: cảnh báo, false: bình thường trở lại
};

#define MAX_ALERT_LOGS 20 // Lưu tối đa 20 sự kiện cảnh báo/khôi phục
AlertLog alertLogs[MAX_ALERT_LOGS];
int alertLogCount = 0; // Số lượng sự kiện đã ghi

WebServer server(80);

// --- CÁC HÀM HỖ TRỢ ---

// Hàm hỗ trợ lấy thời gian hiện tại được định dạng
String getFormattedTime(time_t rawtime) {
    if (rawtime == 0) return "Chưa đồng bộ"; // Hoặc một thông báo khác nếu thời gian chưa được đồng bộ
    struct tm* timeinfo;
    timeinfo = localtime(&rawtime);
    char buffer[80];
    strftime(buffer, sizeof(buffer), "%H:%M:%S %d/%m/%Y", timeinfo);
    return String(buffer);
}

// Hàm ghi log sự kiện cảnh báo/khôi phục
void logAlertEvent(int channelIdx, bool isAlert) {
    if (alertLogCount >= MAX_ALERT_LOGS) {
        // Nếu log đầy, dịch chuyển các log cũ lên để chừa chỗ cho log mới (FIFO)
        for (int i = 0; i < MAX_ALERT_LOGS - 1; i++) {
            alertLogs[i] = alertLogs[i + 1];
        }
        alertLogCount = MAX_ALERT_LOGS - 1; // Giảm số lượng để thêm cái cuối cùng
    }

    alertLogs[alertLogCount].bootTimeMillis = millis();
    time(&alertLogs[alertLogCount].realTime); // Lấy thời gian thực
    alertLogs[alertLogCount].channelIndex = channelIdx;
    alertLogs[alertLogCount].isAlert = isAlert;
    alertLogCount++;
}

// Hàm tạo nội dung báo cáo dưới dạng CSV
String generateReportCsv() {
    String csv = "Channel_GPIO,Event_Type,Real_Time\n"; // Header CSV
    for (int i = 0; i < alertLogCount; i++) {
        String eventType;
        if (alertLogs[i].isAlert) {
            eventType = channels[alertLogs[i].channelIndex].alertTitle;
        } else {
            eventType = channels[alertLogs[i].channelIndex].normalTitle;
        }
        String realTimeStr = getFormattedTime(alertLogs[i].realTime);
        
        // Thay thế dấu phẩy trong nội dung bằng dấu chấm phẩy để tránh lỗi định dạng CSV
        eventType.replace(",", ";"); 
        realTimeStr.replace(",", ";");

        csv += "GPIO " + String(channels[alertLogs[i].channelIndex].pin) + ",";
        csv += eventType + ",";
        csv += realTimeStr + "\n";
    }
    return csv;
}

// --- CÁC HÀM CHỨC NĂNG CHÍNH ---

// Hàm gửi thông báo lên ntfy.sh
void sendNtfyNotification(String title, String message, String tags) {
    if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        String fullUrl = String(NTPY_SERVER_URL) + "/" + String(NTPY_TOPIC);
        http.begin(fullUrl);
        http.addHeader("Title", title);
        http.addHeader("Priority", "default"); // Có thể thay đổi Priority nếu cần (high, urgent)
        http.addHeader("Tags", tags.isEmpty() ? "bell" : tags); // Sử dụng icon "bell" nếu không có tag
        int httpResponseCode = http.POST(message);
        if (httpResponseCode < 0) {
            Serial.printf("[ntfy] Lỗi khi gửi thông báo: %s\n", http.errorToString(httpResponseCode).c_str());
        }
        http.end();
    }
}

// Hàm gửi dữ liệu lên ThingSpeak
void sendToThingSpeak(int states[]) {
    if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        String url = "https://api.thingspeak.com/update?api_key=";
        url += THINGSPEAK_API_KEY;
        for (int i = 0; i < NUM_PINS; i++) {
            url += "&field" + String(i + 1) + "=" + String(states[i]);
        }
        http.begin(url);
        int httpResponseCode = http.GET(); // ThingSpeak thường dùng GET để cập nhật
        if (httpResponseCode < 0) {
            Serial.printf("[ThingSpeak] Lỗi khi gửi dữ liệu, mã lỗi: %d (%s)\n", httpResponseCode, http.errorToString(httpResponseCode).c_str());
        }
        http.end();
    }
}

// Hàm kết nối WiFi
void connectToWiFi() {
    Serial.println("Bắt đầu kết nối WiFi...");
    WiFi.mode(WIFI_STA); // Đặt chế độ Station (kết nối tới AP)
    for (int i = 0; i < numWifiNetworks; i++) {
        Serial.printf("Đang thử kết nối vào mạng '%s'...\n", wifiList[i].ssid);
        WiFi.begin(wifiList[i].ssid, wifiList[i].password);
        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 20) { // Thử 20 lần (10 giây)
            delay(500);
            Serial.print(".");
            attempts++;
        }
        Serial.println(); // Xuống dòng sau dấu chấm chấm
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("Đã kết nối vào mạng '%s'!\n", wifiList[i].ssid);
            Serial.print("Địa chỉ IP: ");
            IPAddress ip = WiFi.localIP();
            Serial.println(ip);

            // Gửi thông báo khởi động và IP lên ntfy.sh
            String message = "Thiết bị đã khởi động và kết nối thành công!\nIP: " + ip.toString();
            sendNtfyNotification("ESP32 Giám Sát Online", message, "desktop_computer");
            
            return; // Thoát hàm nếu đã kết nối thành công
        } else {
            Serial.printf("Không thể kết nối vào '%s'.\n", wifiList[i].ssid);
            WiFi.disconnect(true); // Ngắt kết nối hiện tại để thử mạng khác
            delay(100);
        }
    }
    Serial.println("Không kết nối được WiFi. Khởi động lại sau 5 giây...");
    delay(5000);
    ESP.restart(); // Khởi động lại ESP32 nếu không kết nối được WiFi nào
}

// --- CÁC HÀM XỬ LÝ WEBSERVER ---

// Hàm xử lý reset log cảnh báo
void handleResetLogs() {
    alertLogCount = 0; // Reset số lượng log về 0
    Serial.println("Đã reset log cảnh báo.");
    // Chuyển hướng về trang chính để cập nhật hiển thị
    server.sendHeader("Location", "/"); 
    server.send(303); // Mã 303 See Other cho biết tài nguyên đã được di chuyển tạm thời
}

// Hàm xử lý yêu cầu tải file báo cáo
void handleDownloadReport() {
    String csvContent = generateReportCsv();
    // Tạo tên file báo cáo dựa trên IP của thiết bị và thời gian hiện tại
    String filename = "bao_cao_giam_sat_";
    filename += WiFi.localIP().toString();
    
    // Thêm thời gian vào tên file (optional nhưng tốt cho việc quản lý file)
    time_t now;
    time(&now);
    struct tm* timeinfo = localtime(&now);
    char timeBuffer[30];
    // Định dạng YYYYMMDD_HHMMSS
    strftime(timeBuffer, sizeof(timeBuffer), "%Y%m%d_%H%M%S", timeinfo);
    filename += "_" + String(timeBuffer) + ".csv";


    server.sendHeader("Content-Type", "text/csv"); // Đặt kiểu nội dung là CSV
    server.sendHeader("Content-Disposition", "attachment; filename=" + filename); // Yêu cầu trình duyệt tải xuống file
    server.sendHeader("Connection", "close"); // Đảm bảo trình duyệt đóng kết nối sau khi tải xong
    server.send(200, "text/csv", csvContent); // Gửi nội dung CSV về trình duyệt
    Serial.println("Đã gửi file báo cáo CSV.");
}

// --- CHƯƠNG TRÌNH CHÍNH ---
void setup() {
    Serial.begin(115200); // Khởi tạo Serial để debug qua cổng USB/UART
    Serial.println("\n\n==============================================");
    Serial.println("    DỰ ÁN GIÁM SÁT - PHIÊN BẢN 4.4 (DIGITAL)");
    Serial.println("==============================================");

    connectToWiFi(); // Kết nối ESP32 với mạng WiFi

    // Cấu hình thời gian NTP để có thời gian thực (sau khi có WiFi)
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    Serial.println("Đang đồng bộ thời gian NTP...");
    time_t now;
    time(&now); // Lấy thời gian hiện tại
    Serial.printf("Thời gian hiện tại: %s\n", ctime(&now)); // In thời gian ra Serial Monitor
    
    // Khởi tạo các chân GPIO và trạng thái logic ban đầu
    for (int i = 0; i < NUM_PINS; i++) {
        pinMode(channels[i].pin, INPUT_PULLUP); // Cấu hình chân GPIO là INPUT_PULLUP
        int physicalState = digitalRead(channels[i].pin); // Đọc trạng thái vật lý ban đầu
        // Chuyển đổi trạng thái vật lý sang trạng thái logic dựa trên invertedLogic
        lastLogicalStates[i] = (channels[i].invertedLogic) ? !physicalState : physicalState;
        lastNotificationTimes[i] = 0; // Khởi tạo thời gian thông báo cuối cùng
    }

    // Khởi tạo log cảnh báo rỗng
    alertLogCount = 0; 
    
    // Khởi tạo mDNS (Multicast DNS) để có thể truy cập qua hostname (myEsp32.local)
    if (!MDNS.begin(hostname)) { Serial.println("Lỗi cài đặt mDNS!"); }

    // Cấu hình Webserver cho trang chính "/"
    server.on("/", []() {
        String html = "<html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'></head><body style='font-family: sans-serif; text-align: center; background-color: #f0f0f0; color: #333;'>";
        html += "<div style='max-width: 800px; margin: 20px auto; padding: 20px; background-color: #fff; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1);'>";
        html += "<h1 style='color: #0056b3;'>ESP32 Giám sát và OTA</h1>";
        html += "<h2>(Giám sát " + String(NUM_PINS) + " kênh)</h2>";
        html += "<p><b>Địa chỉ IP:</b> " + WiFi.localIP().toString() + "</p>";
        
        // Bảng trạng thái hiện tại
        html += "<h3 style='margin-top: 25px; color: #0056b3;'>Trạng thái Kênh hiện tại</h3>";
        html += "<table border='1' style='margin: 15px auto; border-collapse: collapse; width: 90%;'>";
        html += "<tr style='background-color: #e9ecef;'><th>Kênh (GPIO)</th><th>Trạng thái hiện tại</th></tr>";
        for (int i = 0; i < NUM_PINS; i++) {
            String statusText = (lastLogicalStates[i] == 0) ? "<b style='color:red;'>&#9888; CẢNH BÁO: " + String(channels[i].alertTitle) + "</b>" : "<b style='color:green;'>&#10003; BÌNH THƯỜNG: " + String(channels[i].normalTitle) + "</b>";
            html += "<tr><td style='padding: 8px; border: 1px solid #ddd;'>GPIO " + String(channels[i].pin) + "</td><td style='padding: 8px; border: 1px solid #ddd;'>" + statusText + "</td></tr>";
        }
        html += "</table>";

        // BẢNG THỐNG KÊ LOG CẢNH BÁO
        html += "<h3 style='margin-top: 25px; color: #0056b3;'>Lịch sử cảnh báo/khôi phục gần đây (" + String(alertLogCount) + "/" + String(MAX_ALERT_LOGS) + ")</h3>";
        if (alertLogCount == 0) {
            html += "<p>Chưa có sự kiện nào được ghi nhận.</p>";
        } else {
            html += "<table border='1' style='margin: 15px auto; font-size: 0.9em; border-collapse: collapse; width: 90%;'>";
            html += "<tr style='background-color: #e9ecef;'><th>Kênh</th><th>Sự kiện</th><th>Thời điểm thực</th></tr>";
            for (int i = 0; i < alertLogCount; i++) {
                String eventText = alertLogs[i].isAlert ? 
                                   "<b style='color:red;'>CẢNH BÁO: " + String(channels[alertLogs[i].channelIndex].alertTitle) + "</b>" : 
                                   "<b style='color:green;'>KHÔI PHỤC: " + String(channels[alertLogs[i].channelIndex].normalTitle) + "</b>";
                
                String realTimeStr = getFormattedTime(alertLogs[i].realTime);

                html += "<tr><td style='padding: 8px; border: 1px solid #ddd;'>GPIO " + String(channels[alertLogs[i].channelIndex].pin) + 
                        "</td><td style='padding: 8px; border: 1px solid #ddd;'>" + eventText + 
                        "</td><td style='padding: 8px; border: 1px solid #ddd;'>" + realTimeStr + "</td></tr>";
            }
            html += "</table>";
        }

        // CÁC NÚT ĐIỀU KHIỂN
        html += "<p style='margin-top: 30px;'>";
        // NÚT RESET LOG
        html += "<form action='/reset_logs' method='post' style='display:inline-block; margin-right: 15px;'><button type='submit' style='padding: 10px 20px; background-color: #dc3545; color: white; border: none; border-radius: 5px; cursor: pointer; font-size: 1em;'>Reset Log Cảnh Báo</button></form>";
        // NÚT TẢI BÁO CÁO
        html += "<form action='/download_report' method='get' style='display:inline-block;'><button type='submit' style='padding: 10px 20px; background-color: #28a745; color: white; border: none; border-radius: 5px; cursor: pointer; font-size: 1em;'>Tải Báo Cáo CSV</button></form>";
        html += "</p>";

        // Nút OTA
        html += "<p style='margin-top: 20px;'><a href='/update' style='padding: 10px 20px; background-color: #007bff; color: white; text-decoration: none; border-radius: 5px; font-size: 1em;'>Cập nhật Firmware (OTA)</a></p>";
        html += "</div></body></html>";
        server.send(200, "text/html; charset=utf-8", html);
    });

    // Định nghĩa các route cho Webserver
    server.on("/reset_logs", HTTP_POST, handleResetLogs); 
    server.on("/download_report", HTTP_GET, handleDownloadReport); 

    ElegantOTA.begin(&server); // Khởi tạo ElegantOTA
    server.begin(); // Bắt đầu Webserver
    Serial.println("Máy chủ HTTP và OTA đã bắt đầu.");
    Serial.println("----------------------------------------------");
}

void loop() {
    server.handleClient(); // Xử lý các yêu cầu đến Webserver
    ElegantOTA.loop();     // Xử lý các tác vụ của ElegantOTA

    // Kiểm tra trạng thái của từng kênh GPIO
    for (int i = 0; i < NUM_PINS; i++) {
        const auto& channel = channels[i];
        int physicalState = digitalRead(channel.pin); // Đọc trạng thái vật lý của chân
        // Chuyển đổi trạng thái vật lý sang trạng thái logic mong muốn
        int currentLogicalState = (channel.invertedLogic) ? !physicalState : physicalState;

        // Nếu trạng thái logic thay đổi và đã đủ thời gian cooldown
        if (currentLogicalState != lastLogicalStates[i]) {
            if (millis() - lastNotificationTimes[i] > notificationCooldown) {
                if (currentLogicalState == 0) { // Nếu chuyển sang trạng thái CẢNH BÁO
                    Serial.printf("Kênh %d (GPIO %d) chuyển sang CẢNH BÁO. Gửi thông báo...\n", i + 1, channel.pin);
                    sendNtfyNotification(channel.alertTitle, channel.alertMessage, channel.alertTags);
                    logAlertEvent(i, true); // Ghi log sự kiện cảnh báo
                } else { // Nếu chuyển về trạng thái BÌNH THƯỜNG
                    Serial.printf("Kênh %d (GPIO %d) trở về BÌNH THƯỜNG. Gửi thông báo...\n", i + 1, channel.pin);
                    sendNtfyNotification(channel.normalTitle, channel.normalMessage, channel.normalTags);
                    logAlertEvent(i, false); // Ghi log sự kiện khôi phục
                }
                lastNotificationTimes[i] = millis(); // Cập nhật thời gian thông báo cuối cùng cho kênh này
            }
            lastLogicalStates[i] = currentLogicalState; // Cập nhật trạng thái logic cuối cùng
        }
    }

    // Gửi dữ liệu lên ThingSpeak định kỳ
    if (millis() - lastThingSpeakTime > THINGSPEAK_UPDATE_INTERVAL) {
        Serial.println("Đã đến lúc gửi trạng thái các kênh lên ThingSpeak...");
        sendToThingSpeak(lastLogicalStates);
        lastThingSpeakTime = millis(); // Cập nhật thời gian gửi ThingSpeak cuối cùng
    }

    delay(20); // Dừng ngắn để cho ESP32 xử lý các tác vụ khác và tránh vòng lặp quá nhanh
}