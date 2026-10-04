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
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
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

static constexpr const char *UPDATE_MANIFEST_URL =
    "https://github.com/cleziotc/esp32-d-star/releases/latest/download/latest.json";

struct OnlineUpdateState {
    bool checked{false};
    bool available{false};
    bool checking{false};
    bool installing{false};
    int progress{0};
    int64_t size{0};
    char version[16]{};
    char firmware_url[384]{};
    char sha256[65]{};
    char message[128]{"Ainda não verificado"};
};

struct OnlineUpdateManifest {
    char version[16]{};
    char firmware_url[384]{};
    char sha256[65]{};
    int64_t size{0};
};

static OnlineUpdateState g_update;
static SemaphoreHandle_t g_update_mutex = nullptr;
static std::atomic<bool> g_online_ota_active{false};

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
}

static const char *host_kind_name(HostKind kind) {
    switch (kind) {
        case HostKind::XLX: return "XLX";
        case HostKind::REF: return "REF";
        case HostKind::XRF: return "XRF";
        default: return "DCS";
    }
}

static bool accepted_host_name(const char *name) {
    return name && (strncmp(name, "XLX", 3) == 0 || strncmp(name, "REF", 3) == 0 ||
                    strncmp(name, "XRF", 3) == 0 || strncmp(name, "DCS", 3) == 0);
}

static bool parse_ipv4(const char *text, uint8_t out[4]) {
    if (!text) return false;
    unsigned a = 0, b = 0, c = 0, d = 0;
    char tail = '\0';
    if (sscanf(text, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    out[0] = static_cast<uint8_t>(a);
    out[1] = static_cast<uint8_t>(b);
    out[2] = static_cast<uint8_t>(c);
    out[3] = static_cast<uint8_t>(d);
    return true;
}

static void format_ipv4(const uint8_t ip[4], char *out, size_t out_len) {
    snprintf(out, out_len, "%u.%u.%u.%u",
             static_cast<unsigned>(ip[0]), static_cast<unsigned>(ip[1]),
             static_cast<unsigned>(ip[2]), static_cast<unsigned>(ip[3]));
}

static bool parse_host_object(const char *obj, HostEntry &out) {
    cJSON *json = cJSON_Parse(obj);
    if (!json) return false;
    cJSON *name = cJSON_GetObjectItemCaseSensitive(json, "name");
    cJSON *ip = cJSON_GetObjectItemCaseSensitive(json, "ipv4");
    bool ok = cJSON_IsString(name) && cJSON_IsString(ip) && name->valuestring && ip->valuestring &&
              accepted_host_name(name->valuestring) && parse_ipv4(ip->valuestring, out.ip);
    if (ok) {
        copy_cstr(out.name, name->valuestring);
        out.kind = host_kind_from_name(out.name);
    }
    cJSON_Delete(json);
    return ok;
}

static void save_host_sync_time(int64_t epoch) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i64(h, "host_sync", epoch);
    nvs_commit(h);
    nvs_close(h);
}

static bool download_host_list() {
    if (!wifi_station_connected()) return false;
    g_host_syncing = true;
    add_log("HOSTS", "Sincronizando DStar_Hosts.json do Pi-Star");

    esp_http_client_config_t cfg{};
    cfg.url = HOSTS_URL;
    cfg.timeout_ms = 30000;
    cfg.keep_alive_enable = false;
    cfg.buffer_size = 2048;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { g_host_syncing = false; return false; }
    esp_http_client_set_header(client, "User-Agent", "Polar-DStar-ESP32/0.1.7");
    esp_http_client_set_header(client, "Connection", "close");
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        add_log("HOSTS", "Falha HTTP: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        g_host_syncing = false;
        return false;
    }
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        add_log("HOSTS", "Servidor respondeu HTTP %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        g_host_syncing = false;
        return false;
    }

    HostEntry *temp = static_cast<HostEntry *>(calloc(HOST_CAP, sizeof(HostEntry)));
    if (!temp) {
        add_log("HOSTS", "Sem memória para lista temporária");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        g_host_syncing = false;
        return false;
    }

    size_t count = 0;
    int depth = 0;
    bool in_string = false, escape = false, capturing = false, object_overflow = false, list_truncated = false;
    char object[384]{};
    size_t object_len = 0;
    char buf[2048];
    int r = 0;
    int transient_reads = 0;
    for (;;) {
        r = esp_http_client_read(client, buf, sizeof(buf));
        if (r == -ESP_ERR_HTTP_EAGAIN && transient_reads < 20) {
            ++transient_reads;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (r <= 0) break;
        transient_reads = 0;
        for (int i = 0; i < r; ++i) {
            const char c = buf[i];
            if (in_string) {
                if (capturing) {
                    if (object_len + 1 < sizeof(object)) object[object_len++] = c;
                    else object_overflow = true;
                }
                if (escape) escape = false;
                else if (c == '\\') escape = true;
                else if (c == '"') in_string = false;
                continue;
            }
            if (c == '"') {
                if (capturing) {
                    if (object_len + 1 < sizeof(object)) object[object_len++] = c;
                    else object_overflow = true;
                }
                in_string = true;
                continue;
            }
            if (c == '{') {
                if (depth == 1) { capturing = true; object_len = 0; object_overflow = false; }
                ++depth;
                if (capturing) {
                    if (object_len + 1 < sizeof(object)) object[object_len++] = c;
                    else object_overflow = true;
                }
                continue;
            }
            if (c == '}') {
                if (capturing) {
                    if (object_len + 1 < sizeof(object)) object[object_len++] = c;
                    else object_overflow = true;
                }
                if (depth == 2 && capturing) {
                    object[std::min(object_len, sizeof(object) - 1)] = '\0';
                    HostEntry entry{};
                    if (!object_overflow && parse_host_object(object, entry)) {
                        if (count < HOST_CAP) temp[count++] = entry;
                        else list_truncated = true;
                    }
                    capturing = false;
                    object_len = 0;
                    object_overflow = false;
                }
                if (depth > 0) --depth;
                continue;
            }
            if (capturing) {
                if (object_len + 1 < sizeof(object)) object[object_len++] = c;
                else object_overflow = true;
            }
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    size_t parsed_counts[4]{};
    for (size_t i = 0; i < count; ++i) ++parsed_counts[static_cast<int>(temp[i].kind)];
    const bool complete_enough = count >= 2000 && parsed_counts[0] > 0 && parsed_counts[1] > 0 && parsed_counts[2] > 0 && parsed_counts[3] > 0;
    if (r < 0 || !complete_enough) {
        add_log("HOSTS", "Sincronização inválida: %u entradas (XLX %u REF %u XRF %u DCS %u)",
                static_cast<unsigned>(count),
                static_cast<unsigned>(parsed_counts[0]), static_cast<unsigned>(parsed_counts[1]),
                static_cast<unsigned>(parsed_counts[2]), static_cast<unsigned>(parsed_counts[3]));
        free(temp);
        g_host_syncing = false;
        return false;
    }

    std::sort(temp, temp + count, [](const HostEntry &a, const HostEntry &b) {
        if (a.kind != b.kind) return static_cast<int>(a.kind) < static_cast<int>(b.kind);
        return strcmp(a.name, b.name) < 0;
    });
    size_t compact = 0;
    for (size_t i = 0; i < count; ++i) {
        if (compact && temp[i].kind == temp[compact - 1].kind && strcmp(temp[i].name, temp[compact - 1].name) == 0) continue;
        temp[compact++] = temp[i];
    }

    xSemaphoreTake(g_hosts_mutex, portMAX_DELAY);
    memcpy(g_hosts, temp, compact * sizeof(HostEntry));
    g_host_count = compact;
    xSemaphoreGive(g_hosts_mutex);
    free(temp);

    time_t now = time(nullptr);
    if (now > 1700000000) {
        g_host_last_sync = static_cast<int64_t>(now);
        save_host_sync_time(g_host_last_sync);
    }
    if (list_truncated) add_log("HOSTS", "Aviso: lista atingiu o limite interno de %u entradas", static_cast<unsigned>(HOST_CAP));
    add_log("HOSTS", "Lista sincronizada: %u reflectores (XLX %u REF %u XRF %u DCS %u)",
            static_cast<unsigned>(compact),
            static_cast<unsigned>(parsed_counts[0]), static_cast<unsigned>(parsed_counts[1]),
            static_cast<unsigned>(parsed_counts[2]), static_cast<unsigned>(parsed_counts[3]));
    g_host_syncing = false;
    return true;
}

static bool daily_host_sync_due() {
    time_t now = time(nullptr);
    if (now <= 1700000000) return false;
    struct tm today{}, last{};
    localtime_r(&now, &today);
    if (g_host_last_sync <= 1700000000) return today.tm_hour >= 3;
    time_t last_t = static_cast<time_t>(g_host_last_sync);
    localtime_r(&last_t, &last);
    const bool same_day = today.tm_year == last.tm_year && today.tm_yday == last.tm_yday;
    if (!same_day) return today.tm_hour >= 3;
    return today.tm_hour >= 3 && last.tm_hour < 3;
}

static void host_sync_task(void *) {
    bool first_online_sync = true;
    uint64_t last_attempt_ms = 0;
    for (;;) {
        if (!wifi_station_connected() || g_online_ota_active.load()) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        if (first_online_sync) {
            for (int i = 0; i < 20 && time(nullptr) <= 1700000000; ++i) vTaskDelay(pdMS_TO_TICKS(500));
        }
        bool requested = g_host_sync_requested.exchange(false);
        bool empty = false;
        xSemaphoreTake(g_hosts_mutex, portMAX_DELAY);
        empty = g_host_count == 0;
        xSemaphoreGive(g_hosts_mutex);
        const bool throttle_ok = last_attempt_ms == 0 || uptime_ms() - last_attempt_ms >= 300000ULL;
        const bool retry_due = empty && throttle_ok;
        const bool daily_due = daily_host_sync_due() && throttle_ok;
        if (!g_host_syncing && (requested || retry_due || daily_due)) {
            last_attempt_ms = uptime_ms();
            download_host_list();
            first_online_sync = false;
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}


static esp_err_t send_json(httpd_req_t *req, cJSON *root, int status = 200);
static esp_err_t send_error(httpd_req_t *req, int status, const char *message);

struct ManifestHttpBuffer {
    std::string body;
    bool overflow{false};
};

static esp_err_t manifest_http_event(esp_http_client_event_t *evt) {
    auto *buffer = static_cast<ManifestHttpBuffer *>(evt->user_data);
    if (!buffer) return ESP_OK;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0) {
        if (buffer->body.size() + static_cast<size_t>(evt->data_len) > 4096) {
            buffer->overflow = true;
            return ESP_OK;
        }
        buffer->body.append(static_cast<const char *>(evt->data), static_cast<size_t>(evt->data_len));
    }
    return ESP_OK;
}

static int compare_versions(const char *a, const char *b) {
    int av[3]{}, bv[3]{};
    if (sscanf(a ? a : "", "%d.%d.%d", &av[0], &av[1], &av[2]) != 3 ||
        sscanf(b ? b : "", "%d.%d.%d", &bv[0], &bv[1], &bv[2]) != 3) {
        return strcmp(a ? a : "", b ? b : "");
    }
    for (int i = 0; i < 3; ++i) {
        if (av[i] < bv[i]) return -1;
        if (av[i] > bv[i]) return 1;
    }
    return 0;
}

static bool fetch_online_manifest(OnlineUpdateManifest &out, char *error, size_t error_len) {
    ManifestHttpBuffer buffer;
    buffer.body.reserve(1024);

    esp_http_client_config_t cfg{};
    cfg.url = UPDATE_MANIFEST_URL;
    cfg.event_handler = manifest_http_event;
    cfg.user_data = &buffer;
    cfg.timeout_ms = 25000;
    cfg.max_redirection_count = 8;
    cfg.keep_alive_enable = false;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.user_agent = "Polar-DStar-ESP32/online-update";

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        snprintf(error, error_len, "Falha ao criar cliente HTTPS");
        return false;
    }
    esp_http_client_set_header(client, "Connection", "close");
    esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        snprintf(error, error_len, "Falha HTTPS: %s", esp_err_to_name(err));
        return false;
    }
    if (status != 200) {
        snprintf(error, error_len, "GitHub respondeu HTTP %d", status);
        return false;
    }
    if (buffer.overflow || buffer.body.empty()) {
        snprintf(error, error_len, "Manifesto de atualização inválido");
        return false;
    }

    cJSON *json = cJSON_Parse(buffer.body.c_str());
    if (!json) {
        snprintf(error, error_len, "latest.json inválido");
        return false;
    }
    cJSON *version = cJSON_GetObjectItemCaseSensitive(json, "version");
    cJSON *firmware = cJSON_GetObjectItemCaseSensitive(json, "firmware");
    cJSON *sha256 = cJSON_GetObjectItemCaseSensitive(json, "sha256");
    cJSON *size = cJSON_GetObjectItemCaseSensitive(json, "size");

    const bool valid = cJSON_IsString(version) && version->valuestring &&
                       cJSON_IsString(firmware) && firmware->valuestring &&
                       cJSON_IsString(sha256) && sha256->valuestring &&
                       strlen(sha256->valuestring) == 64 &&
                       cJSON_IsNumber(size) && size->valuedouble > 0;
    if (!valid) {
        cJSON_Delete(json);
        snprintf(error, error_len, "Campos ausentes em latest.json");
        return false;
    }

    copy_cstr(out.version, version->valuestring);
    copy_cstr(out.firmware_url, firmware->valuestring);
    copy_cstr(out.sha256, sha256->valuestring);
    out.size = static_cast<int64_t>(size->valuedouble);
    cJSON_Delete(json);

    const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
    if (!target || out.size > static_cast<int64_t>(target->size)) {
        snprintf(error, error_len, "Firmware online não cabe na partição OTA");
        return false;
    }
    return true;
}

static void update_set_failure(const char *message) {
    xSemaphoreTake(g_update_mutex, portMAX_DELAY);
    g_update.checking = false;
    g_update.installing = false;
    g_update.progress = 0;
    snprintf(g_update.message, sizeof(g_update.message), "%s", message ? message : "Falha na atualização");
    xSemaphoreGive(g_update_mutex);
    g_online_ota_active = false;
}

static void online_update_check_task(void *) {
    OnlineUpdateManifest manifest{};
    char error[128]{};
    const bool ok = fetch_online_manifest(manifest, error, sizeof(error));

    xSemaphoreTake(g_update_mutex, portMAX_DELAY);
    g_update.checking = false;
    if (!ok) {
        g_update.checked = false;
        g_update.available = false;
        g_update.progress = 0;
        snprintf(g_update.message, sizeof(g_update.message), "%s", error);
    } else {
        g_update.checked = true;
        g_update.progress = 0;
        g_update.size = manifest.size;
        copy_cstr(g_update.version, manifest.version);
        copy_cstr(g_update.firmware_url, manifest.firmware_url);
        copy_cstr(g_update.sha256, manifest.sha256);
        const int cmp = compare_versions(POLAR_DSTAR_VERSION, manifest.version);
        g_update.available = cmp < 0;
        if (cmp < 0) {
            snprintf(g_update.message, sizeof(g_update.message), "Nova versão v%s disponível", manifest.version);
        } else if (cmp == 0) {
            snprintf(g_update.message, sizeof(g_update.message), "Firmware já está atualizado");
        } else {
            snprintf(g_update.message, sizeof(g_update.message), "Versão instalada é mais nova que a publicada");
        }
    }
    xSemaphoreGive(g_update_mutex);
    if (ok) add_log("UPDATE", "GitHub: v%s (%s)", manifest.version,
                    compare_versions(POLAR_DSTAR_VERSION, manifest.version) < 0 ? "nova versão" : "atual");
    else add_log("UPDATE", "%s", error);
    vTaskDelete(nullptr);
}

static void online_update_install_task(void *) {
    OnlineUpdateManifest manifest{};
    xSemaphoreTake(g_update_mutex, portMAX_DELAY);
    copy_cstr(manifest.version, g_update.version);
    copy_cstr(manifest.firmware_url, g_update.firmware_url);
    copy_cstr(manifest.sha256, g_update.sha256);
    manifest.size = g_update.size;
    xSemaphoreGive(g_update_mutex);

    const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
    if (!target) {
        update_set_failure("Nenhuma partição OTA disponível");
        add_log("UPDATE", "Nenhuma partição OTA disponível");
        vTaskDelete(nullptr);
        return;
    }

    add_log("UPDATE", "Baixando v%s diretamente do GitHub", manifest.version);
    esp_http_client_config_t http_cfg{};
    http_cfg.url = manifest.firmware_url;
    http_cfg.timeout_ms = 30000;
    http_cfg.max_redirection_count = 8;
    http_cfg.keep_alive_enable = false;
    http_cfg.buffer_size = 4096;
    http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    http_cfg.user_agent = "Polar-DStar-ESP32/online-ota";

    esp_https_ota_config_t ota_cfg{};
    ota_cfg.http_config = &http_cfg;
    ota_cfg.partition.staging = target;

    esp_https_ota_handle_t handle = nullptr;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Falha ao iniciar OTA: %s", esp_err_to_name(err));
        update_set_failure(msg);
        add_log("UPDATE", "%s", msg);
        vTaskDelete(nullptr);
        return;
    }

    int read_bytes = 0;
    for (;;) {
        err = esp_https_ota_perform(handle);
        read_bytes = esp_https_ota_get_image_len_read(handle);
        const int image_size = esp_https_ota_get_image_size(handle);
        int64_t total = manifest.size > 0 ? manifest.size : image_size;
        int progress = 1;
        if (read_bytes > 0 && total > 0) {
            progress = std::clamp<int>(static_cast<int>((static_cast<int64_t>(read_bytes) * 100) / total), 1, 99);
        }
        xSemaphoreTake(g_update_mutex, portMAX_DELAY);
        g_update.progress = progress;
        snprintf(g_update.message, sizeof(g_update.message), "Baixando v%s: %d%%", manifest.version, progress);
        xSemaphoreGive(g_update_mutex);

        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
        taskYIELD();
    }

    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
        esp_https_ota_abort(handle);
        char msg[128];
        snprintf(msg, sizeof(msg), "Download OTA falhou: %s", esp_err_to_name(err));
        update_set_failure(msg);
        add_log("UPDATE", "%s", msg);
        vTaskDelete(nullptr);
        return;
    }

    if (manifest.size > 0 && read_bytes != manifest.size) {
        esp_https_ota_abort(handle);
        char msg[128];
        snprintf(msg, sizeof(msg), "Tamanho recebido inválido: %d de %lld bytes",
                 read_bytes, static_cast<long long>(manifest.size));
        update_set_failure(msg);
        add_log("UPDATE", "%s", msg);
        vTaskDelete(nullptr);
        return;
    }

    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Validação do firmware falhou: %s", esp_err_to_name(err));
        update_set_failure(msg);
        add_log("UPDATE", "%s", msg);
        vTaskDelete(nullptr);
        return;
    }

    xSemaphoreTake(g_update_mutex, portMAX_DELAY);
    g_update.installing = false;
    g_update.available = false;
    g_update.progress = 100;
    snprintf(g_update.message, sizeof(g_update.message), "v%s instalada. Reiniciando...", manifest.version);
    xSemaphoreGive(g_update_mutex);
    add_log("UPDATE", "v%s instalada com sucesso; reiniciando", manifest.version);
    vTaskDelay(pdMS_TO_TICKS(1800));
    esp_restart();
}

static esp_err_t api_update_get(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "installed", POLAR_DSTAR_VERSION);
    cJSON_AddStringToObject(root, "manifest_url", UPDATE_MANIFEST_URL);

    xSemaphoreTake(g_update_mutex, portMAX_DELAY);
    cJSON_AddBoolToObject(root, "checked", g_update.checked);
    cJSON_AddBoolToObject(root, "available", g_update.available);
    cJSON_AddBoolToObject(root, "checking", g_update.checking);
    cJSON_AddBoolToObject(root, "installing", g_update.installing);
    cJSON_AddNumberToObject(root, "progress", g_update.progress);
    cJSON_AddNumberToObject(root, "size", static_cast<double>(g_update.size));
    cJSON_AddStringToObject(root, "latest", g_update.version);
    cJSON_AddStringToObject(root, "sha256", g_update.sha256);
    cJSON_AddStringToObject(root, "message", g_update.message);
    xSemaphoreGive(g_update_mutex);
    return send_json(req, root);
}

static esp_err_t api_update_check(httpd_req_t *req) {
    if (!wifi_station_connected()) return send_error(req, 400, "Conecte o hotspot à Internet antes de verificar");
    if (g_host_syncing.load()) return send_error(req, 400, "Aguarde a sincronização dos hosts terminar");

    xSemaphoreTake(g_update_mutex, portMAX_DELAY);
    if (g_update.checking || g_update.installing) {
        xSemaphoreGive(g_update_mutex);
        return send_error(req, 400, "Uma operação de atualização já está em andamento");
    }
    g_update.checking = true;
    g_update.progress = 0;
    snprintf(g_update.message, sizeof(g_update.message), "Consultando GitHub...");
    xSemaphoreGive(g_update_mutex);

    if (xTaskCreate(&online_update_check_task, "update_check", 8192, nullptr, 3, nullptr) != pdPASS) {
        update_set_failure("Não foi possível iniciar a verificação");
        return send_error(req, 500, "Falha ao criar tarefa de atualização");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "queued", true);
    return send_json(req, root, 202);
}

static esp_err_t api_update_install(httpd_req_t *req) {
    if (!wifi_station_connected()) return send_error(req, 400, "Conecte o hotspot à Internet antes de atualizar");
    if (g_host_syncing.load()) return send_error(req, 400, "Aguarde a sincronização dos hosts terminar");

    xSemaphoreTake(g_update_mutex, portMAX_DELAY);
    if (g_update.checking || g_update.installing) {
        xSemaphoreGive(g_update_mutex);
        return send_error(req, 400, "Uma operação de atualização já está em andamento");
    }
    if (!g_update.checked || !g_update.available || !g_update.firmware_url[0]) {
        xSemaphoreGive(g_update_mutex);
        return send_error(req, 400, "Nenhuma nova versão verificada");
    }
    g_update.installing = true;
    g_update.progress = 1;
    snprintf(g_update.message, sizeof(g_update.message), "Preparando atualização para v%s...", g_update.version);
    xSemaphoreGive(g_update_mutex);
    g_online_ota_active = true;

    if (xTaskCreate(&online_update_install_task, "online_ota", 12288, nullptr, 4, nullptr) != pdPASS) {
        update_set_failure("Não foi possível iniciar a OTA online");
        return send_error(req, 500, "Falha ao criar tarefa OTA");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "started", true);
    return send_json(req, root, 202);
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root, int status) {
    char status_line[32];
    snprintf(status_line, sizeof(status_line), "%d %s", status,
             status == 200 ? "OK" : status == 202 ? "Accepted" : status == 400 ? "Bad Request" : "Error");
    httpd_resp_set_status(req, status_line);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char *text = cJSON_PrintUnformatted(root);
    esp_err_t err = httpd_resp_sendstr(req, text ? text : "{}");
    if (text) free(text);
    cJSON_Delete(root);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, int status, const char *message) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", message);
    return send_json(req, root, status);
}

static std::string recv_body(httpd_req_t *req, size_t max_len = 4096) {
    if (req->content_len <= 0 || static_cast<size_t>(req->content_len) > max_len) return "";
    std::string body(req->content_len, '\0');
    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, body.data() + received, req->content_len - received);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) return "";
        received += r;
    }
    return body;
}

static esp_err_t static_file(httpd_req_t *req, const uint8_t *data, size_t len, const char *type) {
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, reinterpret_cast<const char *>(data), static_cast<ssize_t>(len));
}

static esp_err_t root_handler(httpd_req_t *req) { return static_file(req, kIndexHtml, kIndexHtmlLen, "text/html; charset=utf-8"); }
static esp_err_t css_handler(httpd_req_t *req) { return static_file(req, kStyleCss, kStyleCssLen, "text/css; charset=utf-8"); }
static esp_err_t js_handler(httpd_req_t *req) { return static_file(req, kAppJs, kAppJsLen, "application/javascript; charset=utf-8"); }

static esp_err_t api_status(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    xSemaphoreTake(g_cfg_mutex, portMAX_DELAY);
    cJSON_AddStringToObject(root, "callsign", g_cfg.callsign);
    cJSON_AddStringToObject(root, "module", g_cfg.module);
    cJSON_AddNumberToObject(root, "rx_hz", static_cast<double>(g_cfg.rx_hz));
    cJSON_AddNumberToObject(root, "tx_hz", static_cast<double>(g_cfg.tx_hz));
    cJSON_AddStringToObject(root, "reflector", g_cfg.reflector);
    cJSON_AddStringToObject(root, "reflector_type", g_cfg.reflector_type);
    cJSON_AddStringToObject(root, "reflector_module", g_cfg.reflector_module);
    xSemaphoreGive(g_cfg_mutex);
    cJSON_AddBoolToObject(root, "mmdvm_online", false);
    cJSON_AddStringToObject(root, "mmdvm_state", "Aguardando v0.2");
    cJSON_AddBoolToObject(root, "dstar_active", false);
    cJSON_AddBoolToObject(root, "tx_active", false);
    cJSON_AddBoolToObject(root, "rx_active", false);
    cJSON_AddBoolToObject(root, "reflector_connected", false);
    cJSON_AddBoolToObject(root, "wifi_connected", wifi_station_connected());
    cJSON_AddStringToObject(root, "ip", preferred_access_ip().c_str());
    cJSON_AddStringToObject(root, "sta_ip", station_ip().c_str());
    cJSON_AddStringToObject(root, "ap_ip", access_point_ip().c_str());
    cJSON_AddNumberToObject(root, "rssi", station_rssi());
    cJSON_AddNumberToObject(root, "uptime_s", uptime_ms() / 1000ULL);
    cJSON_AddStringToObject(root, "version", POLAR_DSTAR_VERSION);
    return send_json(req, root);
}

static esp_err_t api_config_get(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    xSemaphoreTake(g_cfg_mutex, portMAX_DELAY);
    cJSON_AddStringToObject(root, "callsign", g_cfg.callsign);
    cJSON_AddStringToObject(root, "module", g_cfg.module);
    cJSON_AddStringToObject(root, "location", g_cfg.location);
    cJSON_AddNumberToObject(root, "rx_hz", static_cast<double>(g_cfg.rx_hz));
    cJSON_AddNumberToObject(root, "tx_hz", static_cast<double>(g_cfg.tx_hz));
    cJSON_AddNumberToObject(root, "rx_offset_hz", g_cfg.rx_offset_hz);
    cJSON_AddNumberToObject(root, "tx_offset_hz", g_cfg.tx_offset_hz);
    cJSON_AddNumberToObject(root, "tx_level", g_cfg.tx_level);
    cJSON_AddNumberToObject(root, "rx_level", g_cfg.rx_level);
    cJSON_AddStringToObject(root, "reflector", g_cfg.reflector);
    cJSON_AddStringToObject(root, "reflector_type", g_cfg.reflector_type);
    cJSON_AddStringToObject(root, "reflector_module", g_cfg.reflector_module);
    xSemaphoreGive(g_cfg_mutex);
    return send_json(req, root);
}

static void json_copy_string(cJSON *root, const char *key, char *dst, size_t n) {
    cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(it) && it->valuestring) snprintf(dst, n, "%s", it->valuestring);
}
static void json_copy_i32(cJSON *root, const char *key, int32_t &dst) {
    cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(it)) dst = static_cast<int32_t>(it->valuedouble);
}
static void json_copy_i64(cJSON *root, const char *key, int64_t &dst) {
    cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(it)) dst = static_cast<int64_t>(it->valuedouble);
}

static esp_err_t api_config_post(httpd_req_t *req) {
    std::string body = recv_body(req);
    if (body.empty()) return send_error(req, 400, "JSON ausente ou grande demais");
    cJSON *json = cJSON_Parse(body.c_str());
    if (!json) return send_error(req, 400, "JSON inválido");

    xSemaphoreTake(g_cfg_mutex, portMAX_DELAY);
    json_copy_string(json, "callsign", g_cfg.callsign, sizeof(g_cfg.callsign));
    json_copy_string(json, "module", g_cfg.module, sizeof(g_cfg.module));
    json_copy_string(json, "location", g_cfg.location, sizeof(g_cfg.location));
    json_copy_i64(json, "rx_hz", g_cfg.rx_hz);
    json_copy_i64(json, "tx_hz", g_cfg.tx_hz);
    json_copy_i32(json, "rx_offset_hz", g_cfg.rx_offset_hz);
    json_copy_i32(json, "tx_offset_hz", g_cfg.tx_offset_hz);
    json_copy_i32(json, "tx_level", g_cfg.tx_level);
    json_copy_i32(json, "rx_level", g_cfg.rx_level);
    json_copy_string(json, "reflector", g_cfg.reflector, sizeof(g_cfg.reflector));
    json_copy_string(json, "reflector_type", g_cfg.reflector_type, sizeof(g_cfg.reflector_type));
    json_copy_string(json, "reflector_module", g_cfg.reflector_module, sizeof(g_cfg.reflector_module));
    g_cfg.tx_level = std::clamp<int32_t>(g_cfg.tx_level, 0, 100);
    g_cfg.rx_level = std::clamp<int32_t>(g_cfg.rx_level, 0, 100);
    esp_err_t err = save_config_locked();
    xSemaphoreGive(g_cfg_mutex);
    cJSON_Delete(json);
    if (err != ESP_OK) return send_error(req, 500, esp_err_to_name(err));
    add_log("CFG", "Configuração D-Star salva");
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", true);
    return send_json(req, out);
}

static esp_err_t api_wifi_get(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    const std::string current = current_wifi_ssid();
    cJSON_AddBoolToObject(root, "configured", wifi_profile_count() > 0);
    cJSON_AddBoolToObject(root, "connected", wifi_station_connected());
    cJSON_AddStringToObject(root, "ssid", current.c_str());
    cJSON_AddStringToObject(root, "ip", station_ip().c_str());
    cJSON_AddNumberToObject(root, "rssi", station_rssi());
    cJSON_AddStringToObject(root, "ap_ip", access_point_ip().c_str());
    cJSON *profiles = cJSON_AddArrayToObject(root, "profiles");
    xSemaphoreTake(g_wifi_mutex, portMAX_DELAY);
    for (size_t i = 0; i < g_wifi_profile_count; ++i) {
        cJSON *it = cJSON_CreateObject();
        cJSON_AddNumberToObject(it, "index", static_cast<double>(i));
        cJSON_AddStringToObject(it, "ssid", g_wifi_profiles[i].ssid);
        cJSON_AddBoolToObject(it, "connected", wifi_station_connected() && current == g_wifi_profiles[i].ssid);
        cJSON_AddItemToArray(profiles, it);
    }
    xSemaphoreGive(g_wifi_mutex);
    cJSON_AddNumberToObject(root, "max_profiles", WIFI_PROFILE_MAX);
    return send_json(req, root);
}

static esp_err_t api_wifi_scan(httpd_req_t *req) {
    wifi_scan_config_t scan{};
    esp_err_t err = esp_wifi_scan_start(&scan, true);
    if (err != ESP_OK) return send_error(req, 500, esp_err_to_name(err));
    uint16_t total = 0;
    esp_wifi_scan_get_ap_num(&total);
    uint16_t n = std::min<uint16_t>(total, 30);
    wifi_ap_record_t *records = n ? static_cast<wifi_ap_record_t *>(calloc(n, sizeof(wifi_ap_record_t))) : nullptr;
    if (n && !records) return send_error(req, 500, "Sem memória para scan Wi-Fi");
    if (n) esp_wifi_scan_get_ap_records(&n, records);
    cJSON *root = cJSON_CreateObject();
    cJSON *items = cJSON_AddArrayToObject(root, "items");
    for (uint16_t i = 0; i < n; ++i) {
        const char *ssid = reinterpret_cast<const char *>(records[i].ssid);
        if (!ssid || !ssid[0]) continue;
        bool duplicate = false;
        for (uint16_t j = 0; j < i; ++j) {
            if (strcmp(reinterpret_cast<const char *>(records[j].ssid), ssid) == 0) { duplicate = true; break; }
        }
        if (duplicate) continue;
        cJSON *it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "ssid", ssid);
        cJSON_AddNumberToObject(it, "rssi", records[i].rssi);
        cJSON_AddNumberToObject(it, "channel", records[i].primary);
        cJSON_AddBoolToObject(it, "open", records[i].authmode == WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(items, it);
    }
    free(records);
    return send_json(req, root);
}

static void restart_task(void *arg) {
    int delay_ms = reinterpret_cast<intptr_t>(arg);
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    esp_restart();
}

static esp_err_t api_wifi_post(httpd_req_t *req) {
    std::string body = recv_body(req);
    if (body.empty()) return send_error(req, 400, "JSON ausente");
    cJSON *json = cJSON_Parse(body.c_str());
    if (!json) return send_error(req, 400, "JSON inválido");
    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(json, "ssid");
    cJSON *pass = cJSON_GetObjectItemCaseSensitive(json, "password");
    if (!cJSON_IsString(ssid) || !ssid->valuestring || !ssid->valuestring[0] || strlen(ssid->valuestring) > 32) {
        cJSON_Delete(json);
        return send_error(req, 400, "SSID inválido");
    }
    const char *p = cJSON_IsString(pass) && pass->valuestring ? pass->valuestring : "";
    if (strlen(p) > 64) {
        cJSON_Delete(json);
        return send_error(req, 400, "Senha inválida");
    }
    std::string saved = ssid->valuestring;
    int index = upsert_wifi_profile(ssid->valuestring, p);
    cJSON_Delete(json);
    if (index == -2) return send_error(req, 400, "Limite de 5 redes Wi-Fi atingido");
    if (index < 0) return send_error(req, 500, "Falha ao salvar rede Wi-Fi");
    g_wifi_requested_index = index;
    add_log("WIFI", "Rede %s salva no perfil %d", saved.c_str(), index + 1);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddNumberToObject(out, "profile", index);
    cJSON_AddBoolToObject(out, "connecting", true);
    return send_json(req, out, 202);
}

static esp_err_t api_wifi_delete(httpd_req_t *req) {
    std::string body = recv_body(req);
    cJSON *json = body.empty() ? nullptr : cJSON_Parse(body.c_str());
    cJSON *ssid = json ? cJSON_GetObjectItemCaseSensitive(json, "ssid") : nullptr;
    if (!cJSON_IsString(ssid) || !ssid->valuestring) {
        if (json) cJSON_Delete(json);
        return send_error(req, 400, "SSID inválido");
    }
    std::string victim = ssid->valuestring;
    bool was_current = current_wifi_ssid() == victim;
    bool ok = delete_wifi_profile(victim.c_str());
    cJSON_Delete(json);
    if (!ok) return send_error(req, 400, "Rede não encontrada");
    add_log("WIFI", "Rede salva removida: %s", victim.c_str());
    if (was_current) esp_wifi_disconnect();
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", true);
    return send_json(req, out);
}

static void host_counts(size_t counts[4]) {
    memset(counts, 0, 4 * sizeof(size_t));
    xSemaphoreTake(g_hosts_mutex, portMAX_DELAY);
    for (size_t i = 0; i < g_host_count; ++i) ++counts[static_cast<int>(g_hosts[i].kind)];
    xSemaphoreGive(g_hosts_mutex);
}

static esp_err_t api_hosts_status(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    size_t counts[4]{};
    host_counts(counts);
    cJSON_AddStringToObject(root, "source", "Pi-Star DStar_Hosts.json");
    cJSON_AddStringToObject(root, "source_url", HOSTS_URL);
    cJSON_AddStringToObject(root, "schedule", "Diariamente às 03:00 (UTC-3)");
    cJSON_AddBoolToObject(root, "syncing", g_host_syncing.load());
    cJSON_AddNumberToObject(root, "last_sync_epoch", static_cast<double>(g_host_last_sync));
    cJSON_AddBoolToObject(root, "time_synced", time(nullptr) > 1700000000);
    char when[40] = "Nunca";
    if (g_host_last_sync > 1700000000) {
        time_t t = static_cast<time_t>(g_host_last_sync);
        struct tm local{};
        localtime_r(&t, &local);
        strftime(when, sizeof(when), "%d/%m/%Y %H:%M:%S", &local);
    }
    cJSON_AddStringToObject(root, "last_sync", when);
    cJSON *obj = cJSON_AddObjectToObject(root, "counts");
    cJSON_AddNumberToObject(obj, "XLX", counts[static_cast<int>(HostKind::XLX)]);
    cJSON_AddNumberToObject(obj, "REF", counts[static_cast<int>(HostKind::REF)]);
    cJSON_AddNumberToObject(obj, "XRF", counts[static_cast<int>(HostKind::XRF)]);
    cJSON_AddNumberToObject(obj, "DCS", counts[static_cast<int>(HostKind::DCS)]);
    return send_json(req, root);
}

static esp_err_t api_hosts_sync(httpd_req_t *req) {
    if (!wifi_station_connected()) return send_error(req, 400, "Conecte o hotspot à Internet antes de sincronizar");
    if (g_host_syncing.load()) return send_error(req, 400, "Sincronização já está em andamento");
    g_host_sync_requested = true;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "queued", true);
    return send_json(req, root, 202);
}

static HostKind query_host_kind(httpd_req_t *req) {
    char query[48]{};
    char type[8] = "XLX";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "type", type, sizeof(type));
    }
    if (strcmp(type, "REF") == 0) return HostKind::REF;
    if (strcmp(type, "XRF") == 0) return HostKind::XRF;
    if (strcmp(type, "DCS") == 0) return HostKind::DCS;
    return HostKind::XLX;
}

static esp_err_t api_reflectors(httpd_req_t *req) {
    const HostKind wanted = query_host_kind(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char head[48];
    snprintf(head, sizeof(head), "{\"type\":\"%s\",\"items\":[", host_kind_name(wanted));
    esp_err_t err = httpd_resp_send_chunk(req, head, HTTPD_RESP_USE_STRLEN);
    if (err != ESP_OK) return err;

    bool first = true;
    char chunk[96];
    xSemaphoreTake(g_hosts_mutex, portMAX_DELAY);
    for (size_t i = 0; i < g_host_count; ++i) {
        if (g_hosts[i].kind != wanted) continue;
        char ip[16];
        format_ipv4(g_hosts[i].ip, ip, sizeof(ip));
        int n = snprintf(chunk, sizeof(chunk), "%s{\"name\":\"%s\",\"ip\":\"%s\"}",
                         first ? "" : ",", g_hosts[i].name, ip);
        if (n <= 0 || static_cast<size_t>(n) >= sizeof(chunk)) continue;
        err = httpd_resp_send_chunk(req, chunk, static_cast<ssize_t>(n));
        if (err != ESP_OK) break;
        first = false;
    }
    xSemaphoreGive(g_hosts_mutex);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, "]}", 2);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, nullptr, 0);
    return err;
}

static esp_err_t api_system(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    esp_chip_info_t chip{};
    esp_chip_info(&chip);
    uint32_t flash = 0;
    esp_flash_get_size(nullptr, &flash);
    uint8_t mac[6]{};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char macs[20];
    format_mac(macs, sizeof(macs), mac);
    const esp_app_desc_t *desc = esp_app_get_description();

    cJSON_AddStringToObject(root, "model", "ESP32-S3 WROOM");
    cJSON_AddStringToObject(root, "firmware", POLAR_DSTAR_NAME);
    cJSON_AddStringToObject(root, "version", POLAR_DSTAR_VERSION);
    cJSON_AddStringToObject(root, "idf", esp_get_idf_version());
    cJSON_AddStringToObject(root, "build_date", desc->date);
    cJSON_AddStringToObject(root, "build_time", desc->time);
    cJSON_AddNumberToObject(root, "uptime_s", uptime_ms() / 1000ULL);
    cJSON_AddNumberToObject(root, "cores", chip.cores);
    cJSON_AddNumberToObject(root, "flash_bytes", flash);
    cJSON_AddNumberToObject(root, "heap_free", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "heap_min", esp_get_minimum_free_heap_size());
    const size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    cJSON_AddNumberToObject(root, "psram_total", psram_total);
    cJSON_AddNumberToObject(root, "psram_free", psram_free);
#ifdef CONFIG_SPIRAM
    cJSON_AddStringToObject(root, "psram_state", psram_total ? "ativa" : "não detectada");
#else
    cJSON_AddStringToObject(root, "psram_state", "não habilitada");
#endif
    cJSON_AddStringToObject(root, "ip", preferred_access_ip().c_str());
    cJSON_AddStringToObject(root, "sta_ip", station_ip().c_str());
    cJSON_AddStringToObject(root, "ap_ip", access_point_ip().c_str());
    cJSON_AddNumberToObject(root, "rssi", station_rssi());
    cJSON_AddStringToObject(root, "mac", macs);
    return send_json(req, root);
}

static esp_err_t api_logs(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "items");
    xSemaphoreTake(g_log_mutex, portMAX_DELAY);
    size_t start = (g_log_head + LOG_CAP - g_log_count) % LOG_CAP;
    for (size_t i = 0; i < g_log_count; ++i) {
        const LogLine &line = g_logs[(start + i) % LOG_CAP];
        cJSON *it = cJSON_CreateObject();
        cJSON_AddNumberToObject(it, "ms", static_cast<double>(line.ms));
        cJSON_AddStringToObject(it, "level", line.level);
        cJSON_AddStringToObject(it, "text", line.text);
        cJSON_AddItemToArray(arr, it);
    }
    xSemaphoreGive(g_log_mutex);
    return send_json(req, root);
}

static esp_err_t api_calls(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddArrayToObject(root, "items");
    cJSON_AddStringToObject(root, "note", "Histórico de chamadas será ativado com a MMDVM na v0.2");
    return send_json(req, root);
}

static esp_err_t api_reboot(httpd_req_t *req) {
    add_log("SYS", "Reinicialização solicitada pela interface");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "restarting", true);
    esp_err_t resp = send_json(req, root, 202);
    xTaskCreate(&restart_task, "restart", 2048, reinterpret_cast<void *>(1200), 5, nullptr);
    return resp;
}

static esp_err_t api_factory_reset(httpd_req_t *req) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    add_log("SYS", "Configurações apagadas; reiniciando");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "restarting", true);
    esp_err_t resp = send_json(req, root, 202);
    xTaskCreate(&restart_task, "restart", 2048, reinterpret_cast<void *>(1200), 5, nullptr);
    return resp;
}

static esp_err_t api_ota(httpd_req_t *req) {
    if (req->content_len <= 0) return send_error(req, 400, "Firmware vazio");
    const esp_partition_t *update = esp_ota_get_next_update_partition(nullptr);
    if (!update) return send_error(req, 500, "Nenhuma partição OTA disponível");
    if (req->content_len > static_cast<int>(update->size)) return send_error(req, 400, "Firmware maior que a partição OTA");

    add_log("OTA", "Recebendo firmware: %d bytes", req->content_len);
    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(update, OTA_SIZE_UNKNOWN, &handle);
    if (err != ESP_OK) return send_error(req, 500, esp_err_to_name(err));

    char *buf = static_cast<char *>(malloc(4096));
    if (!buf) {
        esp_ota_abort(handle);
        return send_error(req, 500, "Sem memória para OTA");
    }

    int remaining = req->content_len;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, std::min(remaining, 4096));
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            free(buf);
            esp_ota_abort(handle);
            add_log("OTA", "Upload interrompido");
            return send_error(req, 500, "Upload interrompido");
        }
        err = esp_ota_write(handle, buf, r);
        if (err != ESP_OK) {
            free(buf);
            esp_ota_abort(handle);
            add_log("OTA", "Falha ao gravar: %s", esp_err_to_name(err));
            return send_error(req, 500, esp_err_to_name(err));
        }
        remaining -= r;
    }
    free(buf);

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        add_log("OTA", "Imagem inválida: %s", esp_err_to_name(err));
        return send_error(req, 400, "Imagem de firmware inválida");
    }
    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) return send_error(req, 500, esp_err_to_name(err));

    add_log("OTA", "Firmware aceito; reiniciando na nova partição");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "restarting", true);
    esp_err_t resp = send_json(req, root, 202);
    xTaskCreate(&restart_task, "restart", 2048, reinterpret_cast<void *>(1500), 5, nullptr);
    return resp;
}

static void register_uri(httpd_handle_t server, const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *)) {
    httpd_uri_t u{};
    u.uri = uri;
    u.method = method;
    u.handler = handler;
    u.user_ctx = nullptr;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &u));
}

static void start_http_server() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 32;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    ESP_ERROR_CHECK(httpd_start(&s_httpd, &config));

    register_uri(s_httpd, "/", HTTP_GET, root_handler);
    register_uri(s_httpd, "/style.css", HTTP_GET, css_handler);
    register_uri(s_httpd, "/app.js", HTTP_GET, js_handler);
    register_uri(s_httpd, "/api/status", HTTP_GET, api_status);
    register_uri(s_httpd, "/api/config", HTTP_GET, api_config_get);
    register_uri(s_httpd, "/api/config", HTTP_POST, api_config_post);
    register_uri(s_httpd, "/api/wifi", HTTP_GET, api_wifi_get);
    register_uri(s_httpd, "/api/wifi", HTTP_POST, api_wifi_post);
    register_uri(s_httpd, "/api/wifi/scan", HTTP_GET, api_wifi_scan);
    register_uri(s_httpd, "/api/wifi/delete", HTTP_POST, api_wifi_delete);
    register_uri(s_httpd, "/api/hosts", HTTP_GET, api_hosts_status);
    register_uri(s_httpd, "/api/hosts/sync", HTTP_POST, api_hosts_sync);
    register_uri(s_httpd, "/api/reflectors", HTTP_GET, api_reflectors);
    register_uri(s_httpd, "/api/system", HTTP_GET, api_system);
    register_uri(s_httpd, "/api/logs", HTTP_GET, api_logs);
    register_uri(s_httpd, "/api/calls", HTTP_GET, api_calls);
    register_uri(s_httpd, "/api/reboot", HTTP_POST, api_reboot);
    register_uri(s_httpd, "/api/factory-reset", HTTP_POST, api_factory_reset);
    register_uri(s_httpd, "/api/ota", HTTP_POST, api_ota);
    register_uri(s_httpd, "/api/update", HTTP_GET, api_update_get);
    register_uri(s_httpd, "/api/update/check", HTTP_POST, api_update_check);
    register_uri(s_httpd, "/api/update/install", HTTP_POST, api_update_install);
    add_log("WEB", "Dashboard iniciado em HTTP porta 80");
}

extern "C" void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(ret);
    }

    g_log_mutex = xSemaphoreCreateMutex();
    g_cfg_mutex = xSemaphoreCreateMutex();
    g_wifi_mutex = xSemaphoreCreateMutex();
    g_hosts_mutex = xSemaphoreCreateMutex();
    g_update_mutex = xSemaphoreCreateMutex();
    add_log("BOOT", "%s v%s", POLAR_DSTAR_NAME, POLAR_DSTAR_VERSION);
    add_log("BOOT", "v0.1.8: atualização online pelo GitHub");
    add_log("BOOT", "Reset reason: %d", static_cast<int>(esp_reset_reason()));
    add_log("MMDVM", "Driver serial reservado para v0.2");
    load_config();
    load_wifi_profiles();
    wifi_init();
    start_http_server();
    xTaskCreate(&host_sync_task, "host_sync", 6144, nullptr, 3, nullptr);
    add_log("SYS", "Inicialização concluída");
}
