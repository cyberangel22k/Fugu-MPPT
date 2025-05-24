#include <HTTPClient.h>
#include "mbedtls/md.h"
#include <ArduinoJson.h>
#include <sys/time.h>
#include <time.h>

// Tuya credentials
const String TUYA_CLIENT_ID = "eg9m39nh7aq8x5dj9vst";
const String TUYA_CLIENT_SECRET = "c9e109f83c92427dbf07145c33dc57eb";
const String TUYA_DEVICE_ID = "eb8ac282f786d3f60eulea";
const String TUYA_API_REGION = "https://openapi.tuyaus.com";

// Token and breaker state
String tuya_access_token = "";
unsigned long tuya_token_acquired_time = 0;
const unsigned long TUYA_TOKEN_VALIDITY = 7100 * 1000UL; // ~2 hours
bool breakerState = true;

// HMAC-SHA256 function
String hmacSHA256(String message, String key) {
  byte hmacResult[32];
  mbedtls_md_context_t ctx;
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, info, 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char*)key.c_str(), key.length());
  mbedtls_md_hmac_update(&ctx, (const unsigned char*)message.c_str(), message.length());
  mbedtls_md_hmac_finish(&ctx, hmacResult);
  mbedtls_md_free(&ctx);

  char hexResult[65];
  for (int i = 0; i < 32; i++) sprintf(hexResult + i * 2, "%02x", hmacResult[i]);
  hexResult[64] = 0;
  return String(hexResult);
}

// Millisecond timestamp
String getTuyaTimestamp() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  unsigned long long ms = (unsigned long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
  return String(ms);
}

// Fetch Tuya access token
bool getTuyaAccessToken() {
  String t = getTuyaTimestamp();
  String toSign = TUYA_CLIENT_ID + t;
  String sign = hmacSHA256(toSign, TUYA_CLIENT_SECRET);

  HTTPClient http;
  String url = TUYA_API_REGION + "/v1.0/token?grant_type=1";
  http.begin(url);
  http.addHeader("client_id", TUYA_CLIENT_ID);
  http.addHeader("sign_method", "HMAC-SHA256");
  http.addHeader("sign", sign);
  http.addHeader("t", t);

  int httpCode = http.GET();
  if (httpCode == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(2048);
    if (deserializeJson(doc, payload) == DeserializationError::Ok) {
      if (!doc["result"]["access_token"].isNull()) {
        tuya_access_token = doc["result"]["access_token"].as<String>();
        tuya_token_acquired_time = millis();
        Serial.println("✅ Token acquired: " + tuya_access_token);
        return true;
      }
    }
    Serial.println("❌ Invalid JSON structure or missing access_token.");
  } else {
    Serial.println("❌ Token HTTP error: " + String(httpCode));
  }
  return false;
}

// Get breaker state from Tuya
bool getTuyaBreakerStatus() {
  String t = String((unsigned long)(time(nullptr)) * 1000);
  String toSign = TUYA_CLIENT_ID + tuya_access_token + t;
  String sign = hmacSHA256(toSign, TUYA_CLIENT_SECRET);

  HTTPClient http;
  String url = TUYA_API_REGION + "/v1.0/devices/" + TUYA_DEVICE_ID + "/status";
  http.begin(url);
  http.addHeader("client_id", TUYA_CLIENT_ID);
  http.addHeader("access_token", tuya_access_token);
  http.addHeader("sign", sign);
  http.addHeader("t", t);
  http.addHeader("sign_method", "HMAC-SHA256");

  int httpCode = http.GET();
  if (httpCode == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(2048);
    if (deserializeJson(doc, payload) == DeserializationError::Ok) {
      for (JsonObject item : doc["result"].as<JsonArray>()) {
        if (item["code"] == "switch") {
          breakerState = item["value"].as<bool>();
          Serial.print("📥 Breaker state from cloud: ");
          Serial.println(breakerState ? "ON" : "OFF");
          return true;
        }
      }
    }
    Serial.println("❌ Failed to parse breaker status JSON.");
  } else {
    Serial.println("❌ Breaker status HTTP error: " + String(httpCode));
  }
  return false;
}

// Send breaker command
bool sendTuyaSwitchCommand(bool turnOn) {
  unsigned long now = time(nullptr) * 1000;
  String t = String(now);
  String toSign = TUYA_CLIENT_ID + tuya_access_token + t;
  String sign = hmacSHA256(toSign, TUYA_CLIENT_SECRET);

  HTTPClient http;
  String url = TUYA_API_REGION + "/v1.0/devices/" + TUYA_DEVICE_ID + "/commands";
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("client_id", TUYA_CLIENT_ID);
  http.addHeader("access_token", tuya_access_token);
  http.addHeader("sign", sign);
  http.addHeader("t", t);
  http.addHeader("sign_method", "HMAC-SHA256");

  String payload = "{\"commands\":[{\"code\":\"switch\",\"value\":" + String(turnOn ? "true" : "false") + "}]}";
  int httpCode = http.POST(payload);
  String response = http.getString();

  Serial.print("📡 Breaker control HTTP status: ");
  Serial.println(httpCode);
  Serial.println("🔁 Response: " + response);

  if (httpCode == 200 && response.indexOf("\"code\":1010") != -1) {
    Serial.println("🔄 Token invalid — refreshing and retrying...");
    if (getTuyaAccessToken()) return sendTuyaSwitchCommand(turnOn);
    return false;
  }

  return httpCode == 200 && response.indexOf("\"success\":true") != -1;
}

// Main breaker control logic
void controlTuyaBreaker(float voltageOutput, float powerInput) {
  Serial.println("🚦 controlTuyaBreaker() called");
  Serial.print("Voltage: "); Serial.println(voltageOutput);
  Serial.print("Power: "); Serial.println(powerInput);
  Serial.print("Current Breaker State: "); Serial.println(breakerState ? "ON" : "OFF");

  unsigned long now = millis();
  if ((now < tuya_token_acquired_time) || (now - tuya_token_acquired_time > TUYA_TOKEN_VALIDITY) || tuya_access_token == "") {
    if (!getTuyaAccessToken()) {
      Serial.println("❌ Cannot proceed without valid token.");
      return;
    }
    getTuyaBreakerStatus(); // fetch correct state after new token
  }

  if (voltageOutput <= 13.1 && powerInput < 30 && breakerState == true) {
    Serial.println("🛑 Low battery + low input — turning OFF breaker.");
    if (sendTuyaSwitchCommand(false)) breakerState = false;
  } else if (voltageOutput >= 13.2 && powerInput > 50 && breakerState == false) {
    Serial.println("✅ Battery OK + solar available — turning ON breaker.");
    if (sendTuyaSwitchCommand(true)) breakerState = true;
  } else {
    Serial.println("⏭️  Conditions not met. No breaker command sent.");
  }
}
