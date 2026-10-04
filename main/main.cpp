#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <algorithm>
#include <atomic>
#include <ctime>
#include <cstdlib>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"

#include "version.h"
#include "web_assets.h"

static const char *TAG = "POLAR";
static const char *NVS_NS = "polar";
static const int WIFI_CONNECTED_BIT = BIT0;
static EventGroupHandle_t s_wifi_events = nullptr;
static esp_netif_t *s_sta_netif = nullptr;
static esp_netif_t *s_ap_netif = nullptr;
static httpd_handle_t s_httpd = nullptr;

static constexpr size_t WIFI_PROFILE_MAX = 5;
struct WifiProfile {
    char ssid[33]{};
    char password[65]{};
};
static WifiProfile g_wifi_profiles[WIFI_PROFILE_MAX]{};
static size_t g_wifi_profile_count = 0;
static SemaphoreHandle_t g_wifi_mutex = nullptr;
static std::atomic<int> g_wifi_requested_index{-1};

static constexpr const char *HOSTS_URL = "http://www.pistar.uk/downloads/DStar_Hosts.json";
enum class HostKind : uint8_t { XLX = 0, REF = 1, XRF = 2, DCS = 3 };
struct HostEntry {
    char name[8]{};
    uint8_t ip[4]{};
    HostKind kind{HostKind::XLX};
};
// DStar_Hosts.json currently contains more than 4,000 usable DCS/XLX/REF/XRF
// entries. Keeping IPv4 in 4 bytes lets us hold the complete list in about the
// same RAM used by v0.1.6's truncated 2,200-entry table.
static constexpr size_t HOST_CAP = 4600;
static HostEntry g_hosts[HOST_CAP]{};
static size_t g_host_count = 0;
static SemaphoreHandle_t g_hosts_mutex = nullptr;
static std::atomic<bool> g_host_sync_requested{false};
static std::atomic<bool> g_host_syncing{false};
static int64_t g_host_last_sync = 0;


struct AppConfig {
    char callsign[16] = "PP5CI";
    char module[2] = "B";
    char location[40] = "Joinville - SC";
    int64_t rx_hz = 438800000;
    int64_t tx_hz = 438800000;
    int32_t rx_offset_hz = 0;
    int32_t tx_offset_hz = 0;
    int32_t tx_level = 50;
    int32_t rx_level = 50;
    char reflector[16] = "XLX300";
    char reflector_type[5] = "XLX";
    char reflector_module[2] = "D";
};

static AppConfig g_cfg;
static SemaphoreHandle_t g_cfg_mutex = nullptr;

struct LogLine {
    uint64_t ms;
    char level[12];
    char text[176];
};
static constexpr size_t LOG_CAP = 80;
static LogLine g_logs[LOG_CAP]{};
static size_t g_log_head = 0;
static size_t g_log_count = 0;
static SemaphoreHandle_t g_log_mutex = nullptr;

static uint64_t uptime_ms() {
    return static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);
}

template <size_t N>
static void copy_cstr(char (&dst)[N], const char *src) {
    if (!src) { dst[0] = '\0'; return; }
    const size_t len = std::min(strlen(src), N - 1);
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void copy_wifi_field(uint8_t *dst, size_t capacity, const char *src) {
    memset(dst, 0, capacity);
    if (!src || capacity == 0) return;
    const size_t len = std::min(strlen(src), capacity);
    memcpy(dst, src, len);
}

static void add_log(const char *level, const char *fmt, ...) {
    char buf[176];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (g_log_mutex) xSemaphoreTake(g_log_mutex, portMAX_DELAY);
    LogLine &line = g_logs[g_log_head];
    line.ms = uptime_ms();
    snprintf(line.level, sizeof(line.level), "%s", level);
    snprintf(line.text, sizeof(line.text), "%s", buf);
    g_log_head = (g_log_head + 1) % LOG_CAP;
    g_log_count = std::min(g_log_count + 1, LOG_CAP);
    if (g_log_mutex) xSemaphoreGive(g_log_mutex);
    ESP_LOGI(TAG, "[%s] %s", level, buf);
}

static std::string nvs_get_string(nvs_handle_t h, const char *key, const char *def) {
    size_t len = 0;
    if (nvs_get_str(h, key, nullptr, &len) != ESP_OK || len == 0) return def;
    std::string value(len, '\0');
    if (nvs_get_str(h, key, value.data(), &len) != ESP_OK) return def;
    if (!value.empty() && value.back() == '\0') value.pop_back();
    return value;
}

static void load_config() {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        add_log("CFG", "NVS sem configuração; usando padrões");
        return;
    }
    auto cs = nvs_get_string(h, "callsign", g_cfg.callsign);
    auto mod = nvs_get_string(h, "module", g_cfg.module);
    auto loc = nvs_get_string(h, "location", g_cfg.location);
    auto refl = nvs_get_string(h, "reflector", g_cfg.reflector);
    auto reflt = nvs_get_string(h, "refl_type", g_cfg.reflector_type);
    auto reflm = nvs_get_string(h, "refl_mod", g_cfg.reflector_module);
    copy_cstr(g_cfg.callsign, cs.c_str());
    copy_cstr(g_cfg.module, mod.c_str());
    copy_cstr(g_cfg.location, loc.c_str());
    copy_cstr(g_cfg.reflector, refl.c_str());
    copy_cstr(g_cfg.reflector_type, reflt.c_str());
    copy_cstr(g_cfg.reflector_module, reflm.c_str());
    nvs_get_i64(h, "rx_hz", &g_cfg.rx_hz);
    nvs_get_i64(h, "tx_hz", &g_cfg.tx_hz);
    nvs_get_i32(h, "rx_off", &g_cfg.rx_offset_hz);
    nvs_get_i32(h, "tx_off", &g_cfg.tx_offset_hz);
    nvs_get_i32(h, "tx_level", &g_cfg.tx_level);
    nvs_get_i32(h, "rx_level", &g_cfg.rx_level);
    nvs_close(h);
    add_log("CFG", "Configuração carregada para %s-%s", g_cfg.callsign, g_cfg.module);
}

static esp_err_t save_config_locked() {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_str(h, "callsign", g_cfg.callsign);
    nvs_set_str(h, "module", g_cfg.module);
    nvs_set_str(h, "location", g_cfg.location);
    nvs_set_i64(h, "rx_hz", g_cfg.rx_hz);
    nvs_set_i64(h, "tx_hz", g_cfg.tx_hz);
    nvs_set_i32(h, "rx_off", g_cfg.rx_offset_hz);
    nvs_set_i32(h, "tx_off", g_cfg.tx_offset_hz);
    nvs_set_i32(h, "tx_level", g_cfg.tx_level);
    nvs_set_i32(h, "rx_level", g_cfg.rx_level);
    nvs_set_str(h, "reflector", g_cfg.reflector);
    nvs_set_str(h, "refl_type", g_cfg.reflector_type);
    nvs_set_str(h, "refl_mod", g_cfg.reflector_module);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void save_wifi_profiles_locked() {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "wifi_count", static_cast<int32_t>(g_wifi_profile_count));
    for (size_t i = 0; i < WIFI_PROFILE_MAX; ++i) {
        char sk[4] = {'w', 's', static_cast<char>('0' + i), '\0'};
        char pk[4] = {'w', 'p', static_cast<char>('0' + i), '\0'};
        if (i < g_wifi_profile_count && g_wifi_profiles[i].ssid[0]) {
            nvs_set_str(h, sk, g_wifi_profiles[i].ssid);
            nvs_set_str(h, pk, g_wifi_profiles[i].password);
        } else {
            nvs_erase_key(h, sk);
            nvs_erase_key(h, pk);
        }
    }
    nvs_commit(h);
    nvs_close(h);
}

static void load_wifi_profiles() {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    int32_t count = 0;
    if (nvs_get_i32(h, "wifi_count", &count) == ESP_OK) {
        count = std::clamp<int32_t>(count, 0, WIFI_PROFILE_MAX);
        for (int32_t i = 0; i < count; ++i) {
            char sk[4] = {'w', 's', static_cast<char>('0' + i), '\0'};
            char pk[4] = {'w', 'p', static_cast<char>('0' + i), '\0'};
            auto ssid = nvs_get_string(h, sk, "");
            auto pass = nvs_get_string(h, pk, "");
            if (!ssid.empty()) {
                copy_cstr(g_wifi_profiles[g_wifi_profile_count].ssid, ssid.c_str());
                copy_cstr(g_wifi_profiles[g_wifi_profile_count].password, pass.c_str());
                ++g_wifi_profile_count;
            }
        }
    } else {
        // Migração transparente da v0.1.4 e anteriores.
        auto ssid = nvs_get_string(h, "wifi_ssid", "");
        auto pass = nvs_get_string(h, "wifi_pass", "");
        if (!ssid.empty()) {
            copy_cstr(g_wifi_profiles[0].ssid, ssid.c_str());
            copy_cstr(g_wifi_profiles[0].password, pass.c_str());
            g_wifi_profile_count = 1;
        }
    }
    int64_t sync = 0;
    if (nvs_get_i64(h, "host_sync", &sync) == ESP_OK) g_host_last_sync = sync;
    nvs_close(h);
    if (g_wifi_profile_count) {
        xSemaphoreTake(g_wifi_mutex, portMAX_DELAY);
        save_wifi_profiles_locked();
        xSemaphoreGive(g_wifi_mutex);
    }
}

static int upsert_wifi_profile(const char *ssid, const char *pass) {
    if (!ssid || !ssid[0]) return -1;
    xSemaphoreTake(g_wifi_mutex, portMAX_DELAY);
    int found = -1;
    for (size_t i = 0; i < g_wifi_profile_count; ++i) {
        if (strcmp(g_wifi_profiles[i].ssid, ssid) == 0) { found = static_cast<int>(i); break; }
    }
    if (found < 0) {
        if (g_wifi_profile_count >= WIFI_PROFILE_MAX) {
            xSemaphoreGive(g_wifi_mutex);
            return -2;
        }
        found = static_cast<int>(g_wifi_profile_count++);
    }
    copy_cstr(g_wifi_profiles[found].ssid, ssid);
    copy_cstr(g_wifi_profiles[found].password, pass ? pass : "");
    save_wifi_profiles_locked();
    xSemaphoreGive(g_wifi_mutex);
    return found;
}

static bool delete_wifi_profile(const char *ssid) {
    if (!ssid || !ssid[0]) return false;
    xSemaphoreTake(g_wifi_mutex, portMAX_DELAY);
    size_t idx = WIFI_PROFILE_MAX;
    for (size_t i = 0; i < g_wifi_profile_count; ++i) {
        if (strcmp(g_wifi_profiles[i].ssid, ssid) == 0) { idx = i; break; }
    }
    if (idx == WIFI_PROFILE_MAX) { xSemaphoreGive(g_wifi_mutex); return false; }
    for (size_t i = idx; i + 1 < g_wifi_profile_count; ++i) g_wifi_profiles[i] = g_wifi_profiles[i + 1];
    if (g_wifi_profile_count) --g_wifi_profile_count;
    g_wifi_profiles[g_wifi_profile_count] = WifiProfile{};
    save_wifi_profiles_locked();
    xSemaphoreGive(g_wifi_mutex);
    return true;
}

static std::string current_wifi_ssid() {
    wifi_ap_record_t ap{};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return "";
    return reinterpret_cast<const char *>(ap.ssid);
}

static void format_mac(char *out, size_t n, const uint8_t mac[6]) {
    snprintf(out, n, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static std::string netif_ip(esp_netif_t *netif) {
    if (!netif) return "0.0.0.0";
    esp_netif_ip_info_t info{};
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK) return "0.0.0.0";
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
    return ip;
}

static std::string station_ip() { return netif_ip(s_sta_netif); }
static std::string access_point_ip() { return netif_ip(s_ap_netif); }

static bool wifi_station_connected() {
    return s_wifi_events && (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) != 0;
}

static std::string preferred_access_ip() {
    const std::string sta = station_ip();
    if (wifi_station_connected() && sta != "0.0.0.0") return sta;
    const std::string ap = access_point_ip();
    return ap == "0.0.0.0" ? "192.168.4.1" : ap;
}

static int station_rssi() {
    wifi_ap_record_t ap{};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
    return -127;
}

static void wifi_event_handler(void *, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *event = static_cast<ip_event_got_ip_t *>(data);
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        esp_netif_sntp_start();
        add_log("WIFI", "STA conectado a %s: " IPSTR, current_wifi_ssid().c_str(), IP2STR(&event->ip_info.ip));
    }
}

static bool copy_wifi_profile(size_t index, WifiProfile &out) {
    bool ok = false;
    xSemaphoreTake(g_wifi_mutex, portMAX_DELAY);
    if (index < g_wifi_profile_count) { out = g_wifi_profiles[index]; ok = true; }
    xSemaphoreGive(g_wifi_mutex);
    return ok;
}

static size_t wifi_profile_count() {
    xSemaphoreTake(g_wifi_mutex, portMAX_DELAY);
    const size_t n = g_wifi_profile_count;
    xSemaphoreGive(g_wifi_mutex);
    return n;
}

static void apply_wifi_profile(size_t index) {
    WifiProfile p{};
    if (!copy_wifi_profile(index, p)) return;
    wifi_config_t sta_cfg{};
    copy_wifi_field(sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid), p.ssid);
    copy_wifi_field(sta_cfg.sta.password, sizeof(sta_cfg.sta.password), p.password);
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    esp_wifi_disconnect();
    if (esp_wifi_set_config(WIFI_IF_STA, &sta_cfg) == ESP_OK) {
        add_log("WIFI", "Tentando conectar a %s", p.ssid);
        esp_wifi_connect();
    }
}

static void wifi_manager_task(void *) {
    size_t next = 0;
    for (;;) {
        int requested = g_wifi_requested_index.exchange(-1);
        const size_t count = wifi_profile_count();
        if (requested >= 0 && static_cast<size_t>(requested) < count) {
            apply_wifi_profile(static_cast<size_t>(requested));
            next = (static_cast<size_t>(requested) + 1) % std::max<size_t>(count, 1);
            for (int i = 0; i < 20 && !wifi_station_connected(); ++i) vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (wifi_station_connected() || count == 0) {
            vTaskDelay(pdMS_TO_TICKS(1500));
            continue;
        }
        if (next >= count) next = 0;
        apply_wifi_profile(next);
        next = (next + 1) % count;
        for (int i = 0; i < 20 && !wifi_station_connected(); ++i) vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static void wifi_init() {
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta_netif, "polar-dstar");

    setenv("TZ", "BRT3", 1);
    tzset();
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sntp_cfg.start = false;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp_cfg));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    uint8_t mac[6]{};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "Polar-DSTAR-%02X%02X", mac[4], mac[5]);

    wifi_config_t ap_cfg{};
    copy_wifi_field(ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), ap_ssid);
    copy_wifi_field(ap_cfg.ap.password, sizeof(ap_cfg.ap.password), "polar-dstar");
    ap_cfg.ap.ssid_len = strlen(ap_ssid);
    ap_cfg.ap.channel = 6;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    xTaskCreate(&wifi_manager_task, "wifi_mgr", 4096, nullptr, 4, nullptr);

    add_log("WIFI", "AP de manutenção: %s / senha polar-dstar / 192.168.4.1", ap_ssid);
    if (!wifi_profile_count()) add_log("WIFI", "Nenhuma rede STA salva; use Redes > Pesquisar redes");
}

static HostKind host_kind_from_name(const char *name) {
    if (strncmp(name, "XLX", 3) == 0) return HostKind::XLX;
    if (strncmp(name, "REF", 3) == 0) return HostKind::REF;
    if (strncmp(name, "XRF", 3) == 0) return HostKind::XRF;
    return HostKind::DCS;
