#include "wifi_board.h"

#include "application.h"
#include "assets/lang_config.h"
#include "display.h"
#include "settings.h"
#include "system_info.h"

#include <esp_log.h>
#include <esp_mac.h>
#include <esp_network.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <utility>

#include <material_symbols.h>
#include <ssid_manager.h>
#include <wifi_manager.h>
#include <wifi_station.h>
#ifdef CONFIG_XIAOZHI_MICROPYTHON
#include "esp_wifi.h"
#include "xz_bridge.h"
#endif
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
#include "blufi.h"
#endif

static const char* TAG = "WifiBoard";

// Connection timeout in seconds
static constexpr int CONNECT_TIMEOUT_SEC = 60;

#ifdef CONFIG_XIAOZHI_MICROPYTHON
#include "freertos/semphr.h"

// --- MicroPython WiFi bridge -------------------------------------------
//
// Scan strategy: WifiStation's SCAN_DONE handler consumes scan results
// unconditionally, so a parallel get_ap_records crashes (use-after-free)
// and results would be stolen anyway. Instead the WifiBoard ctor registers
// our own SCAN_DONE handler BEFORE StartNetwork() starts the station, so it
// runs first in the event loop. It only intercepts records while a
// MicroPython scan is pending; every other scan cycle belongs to the
// assistant itself and is left untouched.
static SemaphoreHandle_t s_mp_scan_sem = nullptr;
static volatile bool s_mp_scan_pending = false;
static wifi_ap_record_t s_mp_scan_records[24];
static uint16_t s_mp_scan_count = 0;

static void XzWifiScanDoneHandler(void* arg, esp_event_base_t event_base, int32_t event_id,
                                  void* event_data) {
    (void)arg;
    (void)event_base;
    (void)event_id;
    (void)event_data;
    if (!s_mp_scan_pending) {
        return;  // the assistant's own scan: leave the records to WifiStation
    }
    s_mp_scan_pending = false;
    uint16_t n = 24;
    esp_wifi_scan_get_ap_records(&n, s_mp_scan_records);
    s_mp_scan_count = n;
    xSemaphoreGive(s_mp_scan_sem);
}

// Bridge callbacks for the MicroPython `xiaozhi.wifi()` object. connect()
// follows the same provisioning path as the cardputer keyboard UI / blufi:
// persist via SsidManager, (auto-)stop the config AP, let the station
// connect, then poll. On failure the temporary entry is rolled back so the
// assistant keeps using its previous network.
static int XzWifiConnectImpl(const char* ssid, const char* password, int timeout_ms) {
    auto& wifi_manager = WifiManager::GetInstance();
    auto& ssid_manager = SsidManager::GetInstance();
    if (!wifi_manager.IsInitialized()) {
        return -1;
    }

    // Remember a pre-existing entry for this SSID so a failed attempt can
    // restore it (AddSsid overwrites the password in place).
    std::string old_password;
    bool had_entry = false;
    for (const auto& item : ssid_manager.GetSsidList()) {
        if (item.ssid == ssid) {
            old_password = item.password;
            had_entry = true;
            break;
        }
    }

    ssid_manager.AddSsid(ssid, password);
    if (wifi_manager.IsConfigMode()) {
        wifi_manager.StopConfigAp();
    } else {
        // StartStation() is idempotent-guarded ("already active"), so force
        // a restart to make the station re-read the updated SSID list.
        wifi_manager.StopStation();
    }
    wifi_manager.StartStation();

    int waited_ms = 0;
    while (waited_ms < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(200));
        waited_ms += 200;
        if (wifi_manager.IsConnected() && wifi_manager.GetSsid() == ssid) {
            return 0;  // Credentials already persisted for the assistant.
        }
    }

    ESP_LOGW(TAG, "MicroPython WiFi connect to %s failed, rolling back", ssid);
    const auto& list = ssid_manager.GetSsidList();
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].ssid == ssid) {
            ssid_manager.RemoveSsid(static_cast<int>(i));
            break;
        }
    }
    if (had_entry) {
        ssid_manager.AddSsid(ssid, old_password);
    }
    wifi_manager.StopStation();
    wifi_manager.StartStation();  // Reconnect with the remaining list
    return -1;
}

static int XzWifiSaveImpl(const char* ssid, const char* password) {
    SsidManager::GetInstance().AddSsid(ssid, password);
    return 0;
}

// Scan nearby APs and fill a deduplicated list in RSSI order (hidden APs
// skipped). Returns the network count.
static int XzWifiScanImpl(xz_mpy_wifi_ap_t* out_aps, int max_aps) {
    auto& wifi_manager = WifiManager::GetInstance();
    if (!wifi_manager.IsInitialized()) {
        return -1;
    }
    // Works in config-AP mode too: our handler is registered before the
    // config AP's own SCAN_DONE handler, so a pending scan still wins.

    // Self-initiated scans intermittently return 0 records while the station
    // is unassociated (modem power save shortens the dwell), so disable PS
    // for the duration of the scan and restore it afterwards.
    wifi_ps_type_t old_ps = WIFI_PS_MIN_MODEM;
    esp_wifi_get_ps(&old_ps);
    esp_wifi_set_ps(WIFI_PS_NONE);

    // Round 1: start our own scan (retry while the driver is busy with the
    // assistant's own cycle; if such a cycle completes while we are pending
    // our handler takes its records instead, which serves just as well).
    s_mp_scan_pending = true;
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 45; i++) {  // ~9s of retries
        err = esp_wifi_scan_start(nullptr, false);
        if (err == ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (err == ESP_OK) {
        xSemaphoreTake(s_mp_scan_sem, pdMS_TO_TICKS(8000));
    } else {
        ESP_LOGW(TAG, "MicroPython WiFi scan failed: %s", esp_err_to_name(err));
    }
    // Round 2: empty result -> purely wait for the assistant's next scan
    // cycle (station backoff 10-80s, config AP every ~10s) and take its
    // records; no new scan of our own.
    if (s_mp_scan_count == 0) {
        xSemaphoreTake(s_mp_scan_sem, pdMS_TO_TICKS(14000));
    }
    s_mp_scan_pending = false;

    esp_wifi_set_ps(old_ps);
    if (err != ESP_OK && s_mp_scan_count == 0) {
        return -1;
    }

    const wifi_ap_record_t* records = s_mp_scan_records;
    uint16_t count = s_mp_scan_count;

    int written = 0;
    for (uint16_t i = 0; i < count && written < max_aps; i++) {
        const char* ssid = reinterpret_cast<const char*>(records[i].ssid);
        if (ssid[0] == '\0') {
            continue;  // hidden AP
        }
        bool dup = false;
        for (uint16_t j = 0; j < i; j++) {
            if (records[j].ssid[0] != '\0' &&
                strcmp(reinterpret_cast<const char*>(records[j].ssid), ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        strlcpy(out_aps[written].ssid, ssid, sizeof(out_aps[written].ssid));
        out_aps[written].secure = records[i].authmode != WIFI_AUTH_OPEN;
        out_aps[written].channel = records[i].primary;
        out_aps[written].rssi = records[i].rssi;
        written++;
    }
    return written;
}

// --- Worker task ---------------------------------------------------------
//
// WifiManager / esp_netif / lwip call chains (config-AP teardown, netif
// destroy, pthread TLS cleanup) must never run on the MicroPython task:
// they interleave with MicroPython's own pthread/TLS usage (GC lock) and
// crash the runtime. All blocking wifi operations execute here instead.

struct XzWifiJob {
    int kind;  // 0 = connect, 1 = save, 2 = scan
    char ssid[33];
    char password[65];
    int timeout_ms;
    xz_mpy_wifi_ap_t* aps;
    int aps_max;
    int result;
};
static XzWifiJob s_wifi_job;
static SemaphoreHandle_t s_wifi_job_req = nullptr;
static SemaphoreHandle_t s_wifi_job_done = nullptr;
static SemaphoreHandle_t s_wifi_job_lock = nullptr;

static void XzWifiWorker(void* arg) {
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_wifi_job_req, portMAX_DELAY);
        switch (s_wifi_job.kind) {
            case 0:
                s_wifi_job.result =
                    XzWifiConnectImpl(s_wifi_job.ssid, s_wifi_job.password, s_wifi_job.timeout_ms);
                break;
            case 1:
                s_wifi_job.result = XzWifiSaveImpl(s_wifi_job.ssid, s_wifi_job.password);
                break;
            default:
                s_wifi_job.result = XzWifiScanImpl(s_wifi_job.aps, s_wifi_job.aps_max);
                break;
        }
        xSemaphoreGive(s_wifi_job_done);
    }
}

// Submit one job to the worker and wait for the result (caller blocks, the
// MicroPython runtime stays untouched). Serialised by s_wifi_job_lock.
static int XzWifiSubmit(int kind, const char* ssid, const char* password, int timeout_ms,
                        xz_mpy_wifi_ap_t* aps, int aps_max) {
    if (s_wifi_job_lock == nullptr) {
        return -1;
    }
    if (xSemaphoreTake(s_wifi_job_lock, pdMS_TO_TICKS(70000)) != pdTRUE) {
        return -1;  // a previous wifi operation is still stuck
    }
    s_wifi_job.kind = kind;
    s_wifi_job.ssid[0] = '\0';
    if (ssid != nullptr) {
        strlcpy(s_wifi_job.ssid, ssid, sizeof(s_wifi_job.ssid));
    }
    s_wifi_job.password[0] = '\0';
    if (password != nullptr) {
        strlcpy(s_wifi_job.password, password, sizeof(s_wifi_job.password));
    }
    s_wifi_job.timeout_ms = timeout_ms;
    s_wifi_job.aps = aps;
    s_wifi_job.aps_max = aps_max;
    s_wifi_job.result = -1;
    xSemaphoreGive(s_wifi_job_req);
    xSemaphoreTake(s_wifi_job_done, portMAX_DELAY);
    int result = s_wifi_job.result;
    xSemaphoreGive(s_wifi_job_lock);
    return result;
}

// Thin bridge callbacks: forward to the worker task.
static int XzWifiConnect(const char* ssid, const char* password, int timeout_ms) {
    return XzWifiSubmit(0, ssid, password, timeout_ms, nullptr, 0);
}

static int XzWifiSave(const char* ssid, const char* password) {
    return XzWifiSubmit(1, ssid, password, 0, nullptr, 0);
}

static int XzWifiScan(xz_mpy_wifi_ap_t* out_aps, int max_aps) {
    return XzWifiSubmit(2, nullptr, nullptr, 0, out_aps, max_aps);
}

static bool XzWifiIsConnected() { return WifiManager::GetInstance().IsConnected(); }

static void XzWifiGetSsid(char* buf, size_t buf_size) {
    snprintf(buf, buf_size, "%s", WifiManager::GetInstance().GetSsid().c_str());
}

static void XzWifiGetIp(char* buf, size_t buf_size) {
    snprintf(buf, buf_size, "%s", WifiManager::GetInstance().GetIpAddress().c_str());
}

static int XzWifiGetRssi() { return WifiManager::GetInstance().GetRssi(); }
#endif  // CONFIG_XIAOZHI_MICROPYTHON

WifiBoard::WifiBoard() {
#ifdef CONFIG_XIAOZHI_MICROPYTHON
    s_mp_scan_sem = xSemaphoreCreateBinary();
    // Dedicated worker for the blocking MicroPython wifi operations.
    s_wifi_job_req = xSemaphoreCreateBinary();
    s_wifi_job_done = xSemaphoreCreateBinary();
    s_wifi_job_lock = xSemaphoreCreateMutex();
    xTaskCreate(XzWifiWorker, "xz_wifi", 12 * 1024 / sizeof(StackType_t), nullptr, 4, nullptr);
#endif
    // Create connection timeout timer
    esp_timer_create_args_t timer_args = {.callback = OnWifiConnectTimeout,
                                          .arg = this,
                                          .dispatch_method = ESP_TIMER_TASK,
                                          .name = "wifi_connect_timer",
                                          .skip_unhandled_events = true};
    esp_timer_create(&timer_args, &connect_timer_);

#ifdef CONFIG_XIAOZHI_MICROPYTHON
    // Expose the WiFi station to `xiaozhi.wifi()` (all WifiBoard boards).
    static const xz_mpy_wifi_api_t xz_wifi_api = {
        .connect = XzWifiConnect,
        .save = XzWifiSave,
        .scan = XzWifiScan,
        .is_connected = XzWifiIsConnected,
        .get_ssid = XzWifiGetSsid,
        .get_ip = XzWifiGetIp,
        .get_rssi = XzWifiGetRssi,
    };
    xz_mpy_wifi_register(&xz_wifi_api);
#endif
}

WifiBoard::~WifiBoard() {
    if (connect_timer_) {
        esp_timer_stop(connect_timer_);
        esp_timer_delete(connect_timer_);
    }
}

std::string WifiBoard::GetBoardType() { return "wifi"; }

void WifiBoard::StartNetwork() {
    auto& wifi_manager = WifiManager::GetInstance();

    // Initialize WiFi manager
    WifiManagerConfig config;
    config.ssid_prefix = "Xiaozhi";
    config.language = Lang::CODE;
    config.show_ota_config = true;
    config.show_sleep_config = true;

    // Set a DHCP hostname so the router shows a friendly name instead of "espressif".
    // Uses the same "<prefix>-<last 2 MAC bytes>" scheme as the config AP SSID.
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        char hostname[32];
        snprintf(hostname, sizeof(hostname), "%s-%02X%02X", config.ssid_prefix.c_str(), mac[4],
                 mac[5]);
        config.station_hostname = hostname;
    }
    wifi_manager.Initialize(config);

    // Set unified event callback - forward to NetworkEvent with SSID data
    wifi_manager.SetEventCallback([this](WifiEvent event, const std::string& data) {
        switch (event) {
            case WifiEvent::Scanning:
                OnNetworkEvent(NetworkEvent::Scanning);
                break;
            case WifiEvent::Connecting:
                OnNetworkEvent(NetworkEvent::Connecting, data);
                break;
            case WifiEvent::Connected:
                OnNetworkEvent(NetworkEvent::Connected, data);
                break;
            case WifiEvent::Disconnected:
                OnNetworkEvent(NetworkEvent::Disconnected);
                break;
            case WifiEvent::ConfigModeEnter:
                OnNetworkEvent(NetworkEvent::WifiConfigModeEnter);
                break;
            case WifiEvent::ConfigModeExit:
                OnNetworkEvent(NetworkEvent::WifiConfigModeExit);
                break;
        }
    });

#ifdef CONFIG_XIAOZHI_MICROPYTHON
    // The default event loop only exists after Initialize(); registering
    // here keeps us BEFORE the station's own SCAN_DONE handler (installed
    // by TryWifiConnect() below), so a pending MicroPython scan wins the
    // records and WifiStation just sees an empty result.
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, &XzWifiScanDoneHandler, nullptr);
#endif
    // Try to connect or enter config mode
    TryWifiConnect();
}

void WifiBoard::TryWifiConnect() {
    auto& ssid_manager = SsidManager::GetInstance();
    bool have_ssid = !ssid_manager.GetSsidList().empty();

    if (have_ssid) {
        // Start connection attempt with timeout
        ESP_LOGI(TAG, "Starting WiFi connection attempt");
        esp_timer_start_once(connect_timer_, CONNECT_TIMEOUT_SEC * 1000000ULL);
        WifiManager::GetInstance().StartStation();
    } else {
        // No SSID configured, enter config mode
        // Wait for the board version to be shown
        vTaskDelay(pdMS_TO_TICKS(1500));
        StartWifiConfigMode();
    }
}

void WifiBoard::OnNetworkEvent(NetworkEvent event, const std::string& data) {
    switch (event) {
        case NetworkEvent::Connected:
            // Stop timeout timer
            esp_timer_stop(connect_timer_);
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
            // make sure blufi resources has been released
            Blufi::GetInstance().deinit();
#endif
            in_config_mode_ = false;
            ESP_LOGI(TAG, "Connected to WiFi: %s", data.c_str());
            break;
        case NetworkEvent::Scanning:
            ESP_LOGI(TAG, "WiFi scanning");
            break;
        case NetworkEvent::Connecting:
            ESP_LOGI(TAG, "WiFi connecting to %s", data.c_str());
            break;
        case NetworkEvent::Disconnected:
            ESP_LOGW(TAG, "WiFi disconnected");
            break;
        case NetworkEvent::WifiConfigModeEnter:
            ESP_LOGI(TAG, "WiFi config mode entered");
            in_config_mode_ = true;
            break;
        case NetworkEvent::WifiConfigModeExit:
            ESP_LOGI(TAG, "WiFi config mode exited");
            in_config_mode_ = false;
            // Try to connect with the new credentials
            TryWifiConnect();
            break;
        default:
            break;
    }

    // Notify external callback if set
    if (network_event_callback_) {
        network_event_callback_(event, data);
    }
}

void WifiBoard::SetNetworkEventCallback(NetworkEventCallback callback) {
    network_event_callback_ = std::move(callback);
}

void WifiBoard::OnWifiConnectTimeout(void* arg) {
    auto* board = static_cast<WifiBoard*>(arg);
    ESP_LOGW(TAG, "WiFi connection timeout, entering config mode");

    WifiManager::GetInstance().StopStation();
    board->StartWifiConfigMode();
}

void WifiBoard::StartWifiConfigMode() {
    in_config_mode_ = true;
    // Transition to wifi configuring state
    Application::GetInstance().SetDeviceState(kDeviceStateWifiConfiguring);
#ifdef CONFIG_USE_HOTSPOT_WIFI_PROVISIONING
    auto& wifi_manager = WifiManager::GetInstance();

    wifi_manager.StartConfigAp();

    // Show config prompt after a short delay
    Application::GetInstance().Schedule([&wifi_manager]() {
        std::string hint = Lang::Strings::CONNECT_TO_HOTSPOT;
        hint += wifi_manager.GetApSsid();
        hint += Lang::Strings::ACCESS_VIA_BROWSER;
        hint += wifi_manager.GetApWebUrl();

        Application::GetInstance().Alert(Lang::Strings::WIFI_CONFIG_MODE, hint.c_str(), "gear",
                                         Lang::Sounds::OGG_WIFICONFIG);
    });
#elif CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    auto& blufi = Blufi::GetInstance();
    // initialize esp-blufi protocol
    blufi.init();
#endif
}

void WifiBoard::EnterWifiConfigMode() {
    ESP_LOGI(TAG, "EnterWifiConfigMode called");
    GetDisplay()->ShowNotification(Lang::Strings::ENTERING_WIFI_CONFIG_MODE);

    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();

    if (state == kDeviceStateSpeaking || state == kDeviceStateNotifying ||
        state == kDeviceStateListening || state == kDeviceStateIdle) {
        // Reset protocol (close audio channel, reset protocol)
        Application::GetInstance().ResetProtocol();

        xTaskCreate(
            [](void* arg) {
                auto* board = static_cast<WifiBoard*>(arg);

                // Wait for 1 second to allow speaking to finish gracefully
                vTaskDelay(pdMS_TO_TICKS(1000));

                // Stop any ongoing connection attempt
                esp_timer_stop(board->connect_timer_);
                WifiManager::GetInstance().StopStation();

                // Enter config mode
                board->StartWifiConfigMode();

                vTaskDelete(NULL);
            },
            "wifi_cfg_delay", 4096, this, 2, NULL);
        return;
    }

    if (state != kDeviceStateStarting) {
        ESP_LOGE(TAG,
                 "EnterWifiConfigMode called but device state is not starting or speaking, device "
                 "state: %d",
                 state);
        return;
    }

    // Stop any ongoing connection attempt
    esp_timer_stop(connect_timer_);
    WifiManager::GetInstance().StopStation();

    StartWifiConfigMode();
}

bool WifiBoard::IsInWifiConfigMode() const { return WifiManager::GetInstance().IsConfigMode(); }

NetworkInterface* WifiBoard::GetNetwork() {
    static EspNetwork network;
    return &network;
}

const char* WifiBoard::GetNetworkStateIcon() {
    auto& wifi = WifiManager::GetInstance();

    if (wifi.IsConfigMode()) {
        return MATERIAL_SYMBOLS_WIFI;
    }
    if (!wifi.IsConnected()) {
        return MATERIAL_SYMBOLS_WIFI_OFF;
    }

    int rssi = wifi.GetRssi();
    if (rssi >= -65) {
        return MATERIAL_SYMBOLS_WIFI;
    } else if (rssi >= -75) {
        return MATERIAL_SYMBOLS_WIFI_2_BAR;
    }
    return MATERIAL_SYMBOLS_WIFI_1_BAR;
}

std::string WifiBoard::GetBoardJson() {
    auto& wifi = WifiManager::GetInstance();
    std::string json = R"({"type":")" + std::string(BOARD_TYPE) + R"(",)";
    json += R"("name":")" + std::string(BOARD_NAME) + R"(",)";
    json += R"("manufacturer":")" + std::string(BOARD_MANUFACTURER) + R"(",)";

    if (!wifi.IsConfigMode()) {
        json += R"("ssid":")" + wifi.GetSsid() + R"(",)";
        json += R"("rssi":)" + std::to_string(wifi.GetRssi()) + R"(,)";
        json += R"("channel":)" + std::to_string(wifi.GetChannel()) + R"(,)";
        json += R"("ip":")" + wifi.GetIpAddress() + R"(",)";
    }

    json += R"("mac":")" + SystemInfo::GetMacAddress() + R"("})";
    return json;
}

void WifiBoard::SetPowerSaveLevel(PowerSaveLevel level) {
    WifiPowerSaveLevel wifi_level;
    switch (level) {
        case PowerSaveLevel::LOW_POWER:
            wifi_level = WifiPowerSaveLevel::LOW_POWER;
            break;
        case PowerSaveLevel::BALANCED:
            wifi_level = WifiPowerSaveLevel::BALANCED;
            break;
        case PowerSaveLevel::PERFORMANCE:
        default:
            wifi_level = WifiPowerSaveLevel::PERFORMANCE;
            break;
    }
    WifiManager::GetInstance().SetPowerSaveLevel(wifi_level);
}

std::string WifiBoard::GetDeviceStatusJson() {
    auto& board = Board::GetInstance();
    auto root = cJSON_CreateObject();

    // Audio speaker
    auto audio_speaker = cJSON_CreateObject();
    if (auto codec = board.GetAudioCodec()) {
        cJSON_AddNumberToObject(audio_speaker, "volume", codec->output_volume());
    }
    cJSON_AddItemToObject(root, "audio_speaker", audio_speaker);

    // Screen
    auto screen = cJSON_CreateObject();
    if (auto backlight = board.GetBacklight()) {
        cJSON_AddNumberToObject(screen, "brightness", backlight->brightness());
    }
    if (auto display = board.GetDisplay(); display && display->height() > 64) {
        if (auto theme = display->GetTheme()) {
            cJSON_AddStringToObject(screen, "theme", theme->name().c_str());
        }
    }
    cJSON_AddItemToObject(root, "screen", screen);

    // Battery
    int level = 0;
    bool charging = false, discharging = false;
    if (board.GetBatteryLevel(level, charging, discharging)) {
        auto battery = cJSON_CreateObject();
        cJSON_AddNumberToObject(battery, "level", level);
        cJSON_AddBoolToObject(battery, "charging", charging);
        cJSON_AddItemToObject(root, "battery", battery);
    }

    // Network
    auto& wifi = WifiManager::GetInstance();
    auto network = cJSON_CreateObject();
    cJSON_AddStringToObject(network, "type", "wifi");
    cJSON_AddStringToObject(network, "ssid", wifi.GetSsid().c_str());
    int rssi = wifi.GetRssi();
    const char* signal = rssi >= -60 ? "strong" : (rssi >= -70 ? "medium" : "weak");
    cJSON_AddStringToObject(network, "signal", signal);
    cJSON_AddItemToObject(root, "network", network);

    // Chip temperature
    float temp = 0.0f;
    if (board.GetTemperature(temp)) {
        auto chip = cJSON_CreateObject();
        cJSON_AddNumberToObject(chip, "temperature", temp);
        cJSON_AddItemToObject(root, "chip", chip);
    }

    auto str = cJSON_PrintUnformatted(root);
    std::string result(str);
    cJSON_free(str);
    cJSON_Delete(root);
    return result;
}
