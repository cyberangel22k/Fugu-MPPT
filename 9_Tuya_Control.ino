#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <mbedtls/md.h>  // HMAC SHA256

const String TUYA_CLIENT_ID = "eg9m39nh7aq8x5dj9vst";
const String TUYA_CLIENT_SECRET = "c9e109f83c92427dbf07145c33dc57eb";
const String TUYA_DEVICE_ID = "eb8ac282f786d3f60eulea";
const String TUYA_API_REGION = "https://openapi.tuyaus.com";

String accessToken;
bool breakerState = false;
unsigned long lastTuyaControlMillis = 0;
const unsigned long TUYA_CONTROL_INTERVAL = 15 * 60 * 1000;

String generateTuyaSignature(String method, String url, String body = "") {
  String content = TUYA_CLIENT_ID + accessToken + String(millis());
  String toSign = method + "\n" + url + "\n\n" + body;
  toSign = content + toSign;

  byte hmacResult[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char*)TUYA_CLIENT_SECRET.c_str(), TUYA_CLIENT_SECRET.length());
  mbedtls_md_hmac_update(&ctx, (const unsigned char*)toSign.c_str(), toSign.length());
  mbedtls_md_hmac_finish(&ctx, hmacResult);
  mbedtls_md_free(&ctx);

  char hexResult[65];
  for (int i = 0; i < 32; i++) {
    sprintf(&hexResult[i * 2], "%02x", (unsigned int)hmacResult[i]);
  }
  return String(hexResult);
}

bool ensureAccessToken() {
  HTTPClient http;
  String url = TUYA_API_REGION + "/v1.0/token?grant_type=1";

  http.begin(url);
  http.addHeader("client_id", TUYA_CLIENT_ID);
  http.addHeader("sign_method", "HMAC-SHA256");

  int httpCode = http.GET();
  if (httpCode == 200) {
    String payload = http.getString();
    Serial.println("✅ Tuya access token obtained.");
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, payload);
    accessToken = doc["result"]["access_token"].as<String>();
    return true;
  } else {
    Serial.println("❌ Failed to get access token.");
    return false;
  }

  http.end();
}

bool getTuyaSwitchState() {
  if (!ensureAccessToken()) return false;

  HTTPClient http;
  String url = TUYA_API_REGION + "/v1.0/iot-03/devices/" + TUYA_DEVICE_ID + "/status";
  http.begin(url);
  http.addHeader("client_id", TUYA_CLIENT_ID);
  http.addHeader("access_token", accessToken);
  http.addHeader("sign", generateTuyaSignature("GET", url));
  http.addHeader("sign_method", "HMAC-SHA256");
  http.addHeader("t", String(millis()));
  http.addHeader("Content-Type", "application/json");

  int httpCode = http.GET();
  if (httpCode == 200) {
    String response = http.getString();
    Serial.println("📡 Tuya breaker status response: " + response);

    if (response.indexOf("\"code\":0") != -1 && response.indexOf("\"value\":true") != -1) {
      breakerState = true;
    } else if (response.indexOf("\"code\":0") != -1 && response.indexOf("\"value\":false") != -1) {
      breakerState = false;
    }
    return true;
  } else {
    Serial.print("❌ Failed to get breaker status. HTTP code: ");
    Serial.println(httpCode);
    return false;
  }

  http.end();
}

bool sendTuyaSwitchCommand(bool turnOn) {
  if (!ensureAccessToken()) return false;

  HTTPClient http;
  String url = TUYA_API_REGION + "/v1.0/iot-03/devices/" + TUYA_DEVICE_ID + "/commands";
  http.begin(url);

  http.addHeader("client_id", TUYA_CLIENT_ID);
  http.addHeader("access_token", accessToken);
  http.addHeader("sign", generateTuyaSignature("POST", url));
  http.addHeader("sign_method", "HMAC-SHA256");
  http.addHeader("t", String(millis()));
  http.addHeader("Content-Type", "application/json");

  String payload = "{\"commands\":[{\"code\":\"switch_1\",\"value\":" + String(turnOn ? "true" : "false") + "}]}";
  int httpCode = http.POST(payload);

  Serial.println("📨 Payload sent: " + payload);

  if (httpCode > 0) {
    Serial.print("📡 Breaker control HTTP status: ");
    Serial.println(httpCode);
    Serial.println("🔁 Full response: " + http.getString());
    return true;
  } else {
    Serial.println("❌ Failed to send breaker control.");
    return false;
  }

  http.end();
}

// Call this in setup()
void initTuya() {
  ensureAccessToken();
  getTuyaSwitchState();
}

// Call this in loop()
void updateTuyaBreaker(float voltageOutput, float powerInput) {
  unsigned long currentMillis = millis();
  if (currentMillis - lastTuyaControlMillis < TUYA_CONTROL_INTERVAL) return;
  lastTuyaControlMillis = currentMillis;

  Serial.println("🚦 updateTuyaBreaker() called");
  Serial.print("Voltage: "); Serial.println(voltageOutput);
  Serial.print("Power: "); Serial.println(powerInput);
  Serial.print("Current Breaker State: "); Serial.println(breakerState ? "ON" : "OFF");

  if (voltageOutput <= 12.8 && powerInput < 50 && breakerState == true) {
    Serial.println("🛑 Low battery + low input — turning OFF breaker.");
    if (sendTuyaSwitchCommand(false)) breakerState = false;
  }
  else if (voltageOutput >= 12.9 && powerInput > 100 && breakerState == false) {
    Serial.println("✅ Battery OK + solar available — turning ON breaker.");
    if (sendTuyaSwitchCommand(true)) breakerState = true;
  }
  else {
    Serial.println("⏭️  Conditions not met. No breaker command sent.");
  }
}
