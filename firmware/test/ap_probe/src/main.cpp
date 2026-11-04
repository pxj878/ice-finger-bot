// 最小 AP 测试：只开热点，别的什么都不做
// 用来判断「搜不到热点」是硬件/驱动级问题，还是主固件初始化顺序问题
#include <Arduino.h>
#include <WiFi.h>

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== MINIMAL AP TEST ===");

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP("ZZTEST", "12345678", 6, 0, 4);
  delay(800);
  Serial.printf("softAP=%d ssid=%s ip=%s mac=%s\n", (int)ok, WiFi.softAPSSID().c_str(),
                WiFi.softAPIP().toString().c_str(), WiFi.softAPmacAddress().c_str());
  Serial.println("=== AP UP, 保持不动 ===");
}

void loop() { delay(1000); }
