#include "ota.h"
#include "config.h"
#include "display.h"
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>
#include <esp_ota_ops.h>
#include <time.h>

static int           g_lastCheckedDay  = -1;
static volatile bool g_forcePending    = false;

void otaTriggerCheck() {
    g_forcePending = true;
}

bool otaIsForced() {
    return g_forcePending;
}

// Queries the GitHub Releases API for the latest release.
// Returns true and sets downloadUrl if a newer .bin asset is available.
// outUpToDate is set to true when versions match (vs false for network/parse errors).
static bool fetchLatestRelease(String &downloadUrl, bool &outUpToDate) {
    outUpToDate = false;
    WiFiClientSecure client;
    client.setInsecure();

    const char *host = "api.github.com";
    if (!client.connect(host, 443, 12000)) {
        Serial.println("OTA: GitHub API connect failed");
        return false;
    }

    String path = "/repos/" OTA_GITHUB_OWNER "/" OTA_GITHUB_REPO "/releases/latest";
    client.println("GET " + path + " HTTP/1.1");
    client.print("Host: "); client.println(host);
    client.println("User-Agent: RempyRadar/1.0");
    client.println("Accept: application/vnd.github+json");
    client.println("X-GitHub-Api-Version: 2022-11-28");
    client.println("Connection: close");
    client.println();
    client.flush();

    unsigned long deadline = millis() + 10000;
    while (!client.available() && millis() < deadline) {
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    if (!client.available()) {
        Serial.println("OTA: GitHub API timeout");
        client.stop();
        return false;
    }

    String response;
    deadline = millis() + 15000;
    while (millis() < deadline) {
        while (client.available()) {
            response += (char)client.read();
            deadline = millis() + 3000;
        }
        if (!client.connected() && response.length() > 0) break;
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    client.stop();

    int headerEnd = response.indexOf("\r\n\r\n");
    if (headerEnd < 0) { Serial.println("OTA: no header end"); return false; }
    String body = response.substring(headerEnd + 4);
    response = "";

    int jsonStart = body.indexOf('{');
    if (jsonStart < 0) { Serial.println("OTA: no JSON body"); return false; }
    if (jsonStart > 0) body = body.substring(jsonStart);

    StaticJsonDocument<128> filter;
    filter["tag_name"] = true;
    JsonObject af = filter.createNestedArray("assets").createNestedObject();
    af["name"]                 = true;
    af["browser_download_url"] = true;

    // 4096 bytes routes to PSRAM via heap_caps_malloc_extmem_enable(4096)
    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
    body = "";

    if (err) {
        Serial.printf("OTA: JSON error: %s\n", err.c_str());
        return false;
    }

    const char *tag = doc["tag_name"] | "";
    Serial.printf("OTA: current=%s latest=%s\n", FIRMWARE_VERSION, tag);

    if (strlen(tag) == 0) {
        Serial.println("OTA: no release found (no releases published yet, or API error)");
        return false;  // outUpToDate stays false → caller shows "NET ERROR"
    }

    if (strcmp(tag, FIRMWARE_VERSION) == 0) {
        Serial.println("OTA: already up to date");
        outUpToDate = true;
        return false;
    }

    for (JsonObject asset : doc["assets"].as<JsonArray>()) {
        String name = asset["name"] | "";
        if (name.endsWith(".bin")) {
            downloadUrl = asset["browser_download_url"] | "";
            if (downloadUrl.length() > 0) {
                Serial.printf("OTA: asset: %s\n", name.c_str());
                return true;
            }
        }
    }

    Serial.println("OTA: no .bin asset in release");
    return false;
}

void otaInit() {
    // Confirms the running firmware is valid. If the device rebooted into new OTA firmware
    // and this is called, the bootloader will not roll back on the next boot.
    esp_ota_mark_app_valid_cancel_rollback();
    Serial.printf("OTA: running %s\n", FIRMWARE_VERSION);
}

void otaCheckIfDue() {
    bool forced = g_forcePending;
    if (forced) g_forcePending = false;

    if (!forced) {
        struct tm t;
        if (!getLocalTime(&t, 100)) return;
        if (t.tm_hour != 3) return;
        if (t.tm_mday == g_lastCheckedDay) return;
        g_lastCheckedDay = t.tm_mday;
    }

    Serial.println(forced ? "OTA: manual check triggered" : "OTA: nightly check starting");
    displaySetOtaStatus("CHECKING...");

    String downloadUrl;
    bool upToDate = false;
    if (!fetchLatestRelease(downloadUrl, upToDate)) {
        displaySetOtaStatus(upToDate ? "UP TO DATE" : "NET ERROR", 8000);
        return;
    }

    displaySetOtaStatus("UPDATE FOUND!");
    vTaskDelay(1500 / portTICK_PERIOD_MS);  // brief pause so user can read it

    Serial.printf("OTA: downloading from %s\n", downloadUrl.c_str());
    displaySetOtaStatus("DOWNLOADING...");

    WiFiClientSecure otaClient;
    otaClient.setInsecure();
    httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    httpUpdate.rebootOnUpdate(true);

    t_httpUpdate_return ret = httpUpdate.update(otaClient, downloadUrl);

    // HTTP_UPDATE_OK is never reached — httpUpdate reboots the device on success.
    if (ret == HTTP_UPDATE_FAILED) {
        Serial.printf("OTA: download failed [%d]: %s\n",
            httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
        displaySetOtaStatus("DOWNLOAD FAILED", 10000);
    }
}
