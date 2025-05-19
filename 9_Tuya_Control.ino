#include <HTTPClient.h>
#include "mbedtls/md.h"

// Tuya credentials
const String TUYA_CLIENT_ID = "your-client-id";
const String TUYA_CLIENT_SECRET = "your-client-secret";
const String TUYA_DEVICE_ID = "your-device-id";
const String TUYA_API_REGION = "https://openapi.tuyaus.com";  // Western America Data Center

// Token and state
String tuya_access_token = "";
unsigned long tuya_token_acquired_time = 0;
const unsigned long TUYA_TOKEN_VALIDITY = 7100 * 1000UL; // ~2 hours
bool breakerState = true; // true = ON, false = OFF

// HMAC-SHA256 using ESP32's mbedTLS
String hmacSHA256(const String &message, const String &key) {
  byte hmacResult[32];

  mbedtls_md_context_t ctx;
  const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, md_info, 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char*)key.c_str(), key.length());
  mbedtls_md_hmac_update(&ctx, (const unsigned char*)message.c_str(), message.length());
  mbedtls_md_hmac_finish(&ctx, hmacResult);
  mbedtls_md_free(&ctx);

  String hex = "";
  for (int i = 0; i < 32; ++i) {
    if (hmacResult[i] < 0x10) hex += "0";
    hex += String(hmacResult[i], HEX);
  }

  return hex;
}

// Request Tuya access token
bool getTuyaAccessToken() {
  String t = String(millis());
  String toSign = TUYA_CLIENT_ID + t;
  String sign = hmacSHA256(toSign, TUYA_CLIENT_SECRET);

  HTTPClient http;
  String url = TUYA_API_REGION + "/v1.0/token?grant_type=1";
  http.begin(url);
  http.addHeader("client_id", TUYA_CLIENT_ID);
  http.addHeader("sign", sign);
  http.addHeader("t", t);
  http.addHeader("sign_method", "HMAC-SHA256");

  int httpCode = http.GET();
  if (httpCode == 200) {
    String payload = http.getString();
    int tokenIndex = payload.indexOf("\"access_token\":\"") + 16;
    int tokenEnd = payload.indexOf("\"", tokenIndex);
    tuya_access_token = payload.substring(tokenIndex, tokenEnd);
    tuya_token_acquired_time = millis();
    Serial.println("✅ Tuya access token obtained.");
    return true;
  } else {
    Serial.print("❌ Failed to get Tuya token. HTTP code: ");
    Serial.println(httpCode);
    return false;
  }
}

// get switch state
bool getTuyaSwitchState() {
  if (!ensureAccessToken()) return false;

  HTTPClient http;
  String url = String(apiUrl) + "/v1.0/iot-03/devices/" + DEVICE_ID + "/status";

  http.begin(url);
  http.addHeader("client_id", CLIENT_ID);
  http.addHeader("access_token", accessToken);
  http.addHeader("sign", generateTuyaSignature("GET", url, ""));
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

// Send switch command
bool sendTuyaSwitchCommand(bool turnOn) {
  String t = String(millis());
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

  String payload = "{\"commands\":[{\"code\":\"switch_1\",\"value\":" + String(turnOn ? "true" : "false") + "}]}";
  int httpCode = http.POST(payload);
  Serial.print("📡 Breaker control HTTP status: "); Serial.println(httpCode);
  Serial.println("🔁 Response: " + http.getString());

  return httpCode == 200;
}

// Tuya breaker control logic
void controlTuyaBreaker(float voltageOutput, float powerInput) {
  Serial.println("🚦 controlTuyaBreaker() called");
  Serial.print("Voltage: "); Serial.println(voltageOutput);
  Serial.print("Power: "); Serial.println(powerInput);
  Serial.print("Current Breaker State: "); Serial.println(breakerState ? "ON" : "OFF");
  if ((millis() - tuya_token_acquired_time > TUYA_TOKEN_VALIDITY) || tuya_access_token == "") {
    if (!getTuyaAccessToken()) return;
  }

  // Turn OFF: voltage ≤ 12.8V and power < 50W
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
