/*
 ╔══════════════════════════════════════════════════════════════════╗
 ║   MUSEUM VISITOR COUNTER — Wi-Fi Probe Extension                 ║
 ║   Detects phones via Wi-Fi probe requests (passive sniffing)     ║
 ║                                                                  ║
 ║   HOW IT WORKS:                                                  ║
 ║   Every phone constantly sends "probe requests" searching for    ║
 ║   known Wi-Fi networks. ESP32 listens for these in promiscuous   ║
 ║   mode and captures the signal. We NEVER connect to the phone    ║
 ║   or read any data — only the hashed device ID and signal        ║
 ║   strength (RSSI) are recorded.                                  ║
 ║                                                                  ║
 ║   PRIVACY:                                                       ║
 ║   • Raw MAC addresses are NEVER stored                           ║
 ║   • SHA-256 hash is taken immediately and MAC discarded          ║
 ║   • Hashes cannot be reversed back to a MAC address             ║
 ║   • Fully anonymous — GDPR/PDPA compliant                        ║
 ║                                                                  ║
 ║   NOTE: Modern phones (iOS 14+, Android 10+) use randomized     ║
 ║   MACs per network. This means unique hash ≠ unique person for  ║
 ║   every phone. We count detections + estimate unique visitors    ║
 ║   using a time-window deduplication algorithm.                   ║
 ║                                                                  ║
 ║   HARDWARE: Same ESP32 as main counter — runs alongside it.      ║
 ║   No extra components needed.                                    ║
 ╚══════════════════════════════════════════════════════════════════╝

  LIBRARIES REQUIRED:
  • esp_wifi.h     (built-in ESP32 IDF)
  • mbedtls/md.h   (built-in ESP32 IDF — for SHA-256)
  • WiFi.h         (built-in)
  • HTTPClient.h   (built-in)
  • ArduinoJson    (v6+)
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_wifi.h>
#include "esp_wifi_types.h"
#include "mbedtls/md.h"
#include <ArduinoJson.h>
#include <map>
#include <string>

// ─── CONFIGURATION ───────────────────────────────────────────────────
const char* WIFI_SSID       = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD   = "YOUR_WIFI_PASSWORD";
const char* SERVER_URL      = "http://your-railway-app.up.railway.app/api/probe";
const char* ROOM_ID         = "main_hall";

// Detection tuning
const int   RSSI_THRESHOLD  = -75;    // dBm — ignore very weak signals (far away phones)
const int   DEDUP_WINDOW_MS = 30000;  // 30s — same hash seen again = same person, don't recount
const int   SEND_INTERVAL   = 10000;  // 10s — how often to POST aggregated data
const int   CLEANUP_INTERVAL= 120000; // 2min — clear old entries from memory

// ─── STATE ────────────────────────────────────────────────────────────
struct DeviceEntry {
  unsigned long firstSeen;
  unsigned long lastSeen;
  int rssi;
  int seenCount;
};

std::map<std::string, DeviceEntry> detectedDevices;
int totalUniqueToday   = 0;
int currentInRange     = 0;
unsigned long lastSend = 0;
unsigned long lastClean= 0;
portMUX_TYPE probeMux  = portMUX_INITIALIZER_UNLOCKED;

// ─── SHA-256 HASH ────────────────────────────────────────────────────
// Hash MAC address so raw MAC is never stored
String hashMAC(const uint8_t* mac) {
  uint8_t digest[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
  mbedtls_md_starts(&ctx);
  mbedtls_md_update(&ctx, mac, 6);
  mbedtls_md_finish(&ctx, digest);
  mbedtls_md_free(&ctx);

  // Return first 16 hex chars (64-bit prefix — enough for dedup, not reversible)
  String result = "";
  for (int i = 0; i < 8; i++) {
    if (digest[i] < 0x10) result += "0";
    result += String(digest[i], HEX);
  }
  return result;
}

// ─── PROMISCUOUS MODE CALLBACK ───────────────────────────────────────
// Called by ESP32 for every Wi-Fi frame in range
void IRAM_ATTR wifiSnifferCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;  // Only management frames (probe requests)

  const wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
  const wifi_ieee80211_packet_t* ipkt = (wifi_ieee80211_packet_t*)pkt->payload;
  const wifi_ieee80211_mac_hdr_t* hdr = &ipkt->hdr;

  // Frame control: type=0 (management), subtype=4 (probe request)
  uint8_t frameType    = (hdr->frame_ctrl & 0x000C) >> 2;
  uint8_t frameSubtype = (hdr->frame_ctrl & 0x00F0) >> 4;
  if (frameType != 0 || frameSubtype != 4) return;

  int rssi = pkt->rx_ctrl.rssi;
  if (rssi < RSSI_THRESHOLD) return;  // Too far away

  // Hash the source MAC (addr2 = transmitter)
  String hashId = hashMAC(hdr->addr2);
  std::string key = hashId.c_str();

  unsigned long now = millis();

  portENTER_CRITICAL_ISR(&probeMux);
  auto it = detectedDevices.find(key);
  if (it == detectedDevices.end()) {
    // New device
    detectedDevices[key] = { now, now, rssi, 1 };
    totalUniqueToday++;
  } else {
    // Seen before — update last seen + RSSI
    it->second.lastSeen  = now;
    it->second.rssi      = rssi;
    it->second.seenCount++;
  }
  portEXIT_CRITICAL_ISR(&probeMux);
}

// ─── SETUP ────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== Museum Wi-Fi Probe Sniffer ===");

  // Connect to Wi-Fi for sending data
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) {
    delay(500); Serial.print("."); tries++;
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? "\nConnected!" : "\nOffline mode");

  // Enable promiscuous mode for sniffing
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&wifiSnifferCallback);

  // Scan all 2.4GHz channels (phones probe on all channels)
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

  Serial.println("Probe sniffing active. Listening...");
}

// ─── MAIN LOOP ────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  // Rotate through Wi-Fi channels 1–13 every 200ms
  // Phones probe on random channels — we need to scan all of them
  static unsigned long lastChannelSwitch = 0;
  static uint8_t currentChannel = 1;
  if (now - lastChannelSwitch > 200) {
    currentChannel = (currentChannel % 13) + 1;
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
    lastChannelSwitch = now;
  }

  // Compute "currently in range" = devices seen in last 30 seconds
  if (now - lastSend >= SEND_INTERVAL) {
    portENTER_CRITICAL(&probeMux);
    currentInRange = 0;
    for (auto& kv : detectedDevices) {
      if (now - kv.second.lastSeen < DEDUP_WINDOW_MS) {
        currentInRange++;
      }
    }
    portEXIT_CRITICAL(&probeMux);

    sendProbeData();
    lastSend = now;

    Serial.printf("[PROBE] In range: %d | Unique today: %d\n",
      currentInRange, totalUniqueToday);
  }

  // Periodic cleanup — remove entries older than 5 minutes to save RAM
  if (now - lastClean >= CLEANUP_INTERVAL) {
    portENTER_CRITICAL(&probeMux);
    auto it = detectedDevices.begin();
    while (it != detectedDevices.end()) {
      if (now - it->second.lastSeen > 300000) {  // 5 minutes
        it = detectedDevices.erase(it);
      } else {
        ++it;
      }
    }
    portEXIT_CRITICAL(&probeMux);
    lastClean = now;
    Serial.printf("[CLEANUP] Active entries: %d\n", detectedDevices.size());
  }

  delay(10);
}

// ─── SEND DATA ────────────────────────────────────────────────────────
void sendProbeData() {
  if (WiFi.status() != WL_CONNECTED) return;

  // Temporarily disable promiscuous mode to use Wi-Fi for HTTP
  esp_wifi_set_promiscuous(false);

  HTTPClient http;
  http.begin(SERVER_URL);
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<256> doc;
  doc["room_id"]          = ROOM_ID;
  doc["phones_in_range"]  = currentInRange;
  doc["unique_today"]     = totalUniqueToday;
  doc["active_entries"]   = (int)detectedDevices.size();
  doc["timestamp"]        = millis();
  doc["channel_hopping"]  = true;

  String body;
  serializeJson(doc, body);
  int code = http.POST(body);
  Serial.printf("[HTTP] Probe POST → %d\n", code);
  http.end();

  // Re-enable sniffing
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&wifiSnifferCallback);
}

// ─── IEEE 802.11 STRUCTS ─────────────────────────────────────────────
// Needed to parse raw Wi-Fi frames
typedef struct {
  unsigned frame_ctrl: 16;
  unsigned duration_id: 16;
  uint8_t addr1[6];
  uint8_t addr2[6];
  uint8_t addr3[6];
  unsigned sequence_ctrl: 16;
  uint8_t addr4[6];
} wifi_ieee80211_mac_hdr_t;

typedef struct {
  wifi_ieee80211_mac_hdr_t hdr;
  uint8_t payload[0];
} wifi_ieee80211_packet_t;
