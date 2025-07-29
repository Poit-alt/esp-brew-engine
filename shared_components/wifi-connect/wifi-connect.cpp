/*
 * esp-brew-engine
 * Copyright (C) Dekien Jeroen 2024
 *
 */
#include "wifi-connect.h"

using namespace std;
using json = nlohmann::json;

static const char *TAG = "WiFiConnect";

WiFiConnect::WiFiConnect(SettingsManager *settingsManager)
{
    this->settingsManager = settingsManager;
}

void WiFiConnect::Connect()
{
    this->readSettings();

    if (this->enableAP)
    {
        ESP_LOGI(TAG, "Starting wifi Access Point");
        this->wifi_init_softap();
    }
    else
    {
        ESP_LOGI(TAG, "Starting wifi Station");
        this->wifi_init_sta();
    }
}

void WiFiConnect::readSettings()
{
    ESP_LOGI(TAG, "Reading Wifi Settings");

    // the logic here is that settings from nvs get preference, but if they don't exist settings from menuconfig are used and also saved to nvs
    this->ssid = this->settingsManager->Read("wifi_ssid", (string)CONFIG_WIFI_SSID);
    this->password = this->settingsManager->Read("wifi_password", (string)CONFIG_WIFI_PASS);
    this->Hostname = this->settingsManager->Read("Hostname", (string)CONFIG_HOSTNAME);
    this->maxWifiPower = this->settingsManager->Read("wifi_max_power", (int8_t)CONFIG_ESP_PHY_MAX_WIFI_TX_POWER);

    bool configUseWifiAP = false;
// is there a cleaner way to do this?, config to bool doesn't seem to work properly
#if defined(CONFIG_WIFI_AP)
    configUseWifiAP = true;
#endif

    this->enableAP = this->settingsManager->Read("wifi_ap", (bool)configUseWifiAP);

    ESP_LOGI(TAG, "Reading Wifi Settings Done");
}

void WiFiConnect::saveSettings()
{
    ESP_LOGI(TAG, "Saving Wifi Settings");

    this->settingsManager->Write("wifi_ssid", this->ssid);
    this->settingsManager->Write("wifi_password", this->password);
    this->settingsManager->Write("wifi_ap", this->enableAP);
    this->settingsManager->Write("wifi_max_power", this->maxWifiPower);
    this->settingsManager->Write("Hostname", this->Hostname);

    ESP_LOGI(TAG, "Saving Wifi Settings Done");
}

void WiFiConnect::gotIP(string Ip)
{
    ESP_LOGI(TAG, "Got IP:%s", Ip.c_str());
    if (this->GotIpCallback)
    {
        this->GotIpCallback(Ip);
    }
    this->Ip = Ip;

    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
}

void WiFiConnect::printTime()
{
    char strftime_buf[64];

    // // Set timezone to Eastern Standard Time and print local time
    // setenv("BR", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    // tzset();
    // localtime_r(&now, &timeinfo);

    time_t now = time(0);
    tm *localtm = localtime(&now);
    strftime(strftime_buf, sizeof(strftime_buf), "%c", localtm);
    ESP_LOGI(TAG, "The current date/time is: %s", strftime_buf);
}

void WiFiConnect::obtainTime()
{
    ESP_LOGI(TAG, "Initializing SNTP");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, this->ntpServer.c_str());
#ifdef CONFIG_SNTP_TIME_SYNC_METHOD_SMOOTH
    sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
#endif
    esp_sntp_init();

    // wait for time to be set
    time_t now = 0;
    struct tm timeinfo;
    int retry = 0;
    const int retry_count = 20;
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < retry_count)
    {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
    time(&now);
    localtime_r(&now, &timeinfo);
}

// must be static limitation of esp-idf or freertos
void WiFiConnect::wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{

    // class instanse is paased as arg, due to static limitation
    WiFiConnect *instance = (WiFiConnect *)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        ESP_LOGI(TAG, "Start Connect - ssid:%s password:%s ", instance->ssid.c_str(), instance->password.c_str());

        esp_wifi_connect();
    }
    // doesn't seem to work yet atm, ipv6 seems imature in esp-idf
    // else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED)
    // {
    //     ESP_LOGI(TAG, "Create Ip6 Linklocal");
    //     // esp_netif_create_ip6_linklocal(instance->sta_netif);

    //     // esp_ip6_addr_t addr;
    // }
    //   else if (event_base == IP_EVENT && event_id == IP_EVENT_GOT_IP6)
    // {
    //     ESP_LOGI(TAG, "Got Ip6");
    // }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        instance->s_retry_num++;
        ESP_LOGI(TAG, "Disconnected (attempt %d/10)", instance->s_retry_num);
        
        if (instance->s_retry_num < 10) {
            // Continue retrying - don't use vTaskDelay in event handler
            esp_wifi_connect();
        } else {
            // After 10 failed attempts, set simple flag (interrupt-safe)
            ESP_LOGW(TAG, "WiFi connection failed after 10 attempts, setting failure flag");
            instance->connection_failed = true;
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

        // Reset retry counter on successful connection
        instance->s_retry_num = 0;
        ESP_LOGI(TAG, "WiFi connected successfully, retry counter reset");

        string ipAddr;
        ipAddr = inet_ntoa(event->ip_info.ip);
        instance->gotIP(ipAddr);

        // Signal successful connection
        xEventGroupSetBits(instance->s_wifi_event_group, WIFI_CONNECTED_BIT);

        if (instance->setTime)
        {
            instance->obtainTime();
            instance->printTime();
        }
    }
}

void WiFiConnect::wifi_init_sta(void)
{
    // Reset retry counter and failure flag when starting station mode
    this->s_retry_num = 0;
    this->connection_failed = false;
    ESP_LOGI(TAG, "Starting WiFi station mode, retry counter and flags reset");

    this->s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    this->sta_netif = esp_netif_create_default_wifi_sta();

    // set the Hostname
    ESP_ERROR_CHECK(esp_netif_set_hostname(this->sta_netif, this->Hostname.c_str()));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_wifi_set_max_tx_power(this->maxWifiPower);

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &this->wifi_event_handler, this, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &this->wifi_event_handler, this, &instance_got_ip));

    wifi_config_t wifi_config{};
    strcpy((char *)wifi_config.sta.ssid, this->ssid.c_str());
    strcpy((char *)wifi_config.sta.password, this->password.c_str());

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Wait for connection or failure using a polling approach to avoid event group issues */
    ESP_LOGI(TAG, "Waiting for WiFi connection or failure...");
    
    // Poll for connection status with timeout
    int wait_time = 0;
    const int max_wait_time = 60000; // 60 seconds max wait
    const int poll_interval = 100;   // 100ms poll interval
    
    while (wait_time < max_wait_time) {
        // Check if connection succeeded
        EventBits_t bits = xEventGroupGetBits(this->s_wifi_event_group);
        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "Connected to ap ssid:%s password:%s", this->ssid.c_str(), this->password.c_str());
            break;
        }
        
        // Check if connection failed using our interrupt-safe flag
        if (this->connection_failed) {
            ESP_LOGI(TAG, "Failed to connect to ssid:%s, password:%s", this->ssid.c_str(), this->password.c_str());
            
            ESP_LOGW(TAG, "Switching to AP mode after %d failed attempts", this->s_retry_num);
            
            // Stop current WiFi
            esp_wifi_stop();
            
            // Enable AP mode and save settings
            this->enableAP = true;
            this->saveSettings();
            
            // Start AP mode
            this->wifi_init_softap();
            
            ESP_LOGI(TAG, "Switched to AP mode. Connect to SSID: %s", this->Hostname.c_str());
            break;
        }
        
        // Wait a bit before checking again
        vTaskDelay(pdMS_TO_TICKS(poll_interval));
        wait_time += poll_interval;
    }
    
    if (wait_time >= max_wait_time) {
        ESP_LOGW(TAG, "WiFi connection timeout after %d ms", max_wait_time);
    }

    /* The event will not be processed after unregister */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(this->s_wifi_event_group);
}

void WiFiConnect::wifi_init_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    this->sta_netif = esp_netif_create_default_wifi_ap();

    // set the Hostname
    ESP_ERROR_CHECK(esp_netif_set_hostname(this->sta_netif, this->Hostname.c_str()));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_wifi_set_max_tx_power(this->maxWifiPower);

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &this->wifi_event_handler, this, NULL));

    wifi_config_t wifi_config = {};

    strcpy((char *)wifi_config.ap.ssid, this->Hostname.c_str());
    strcpy((char *)wifi_config.ap.password, this->password.c_str());
    wifi_config.ap.channel = this->apChannel;
    wifi_config.ap.max_connection = 10;
    // wifi_config.ap.pmf_cfg.required = true;
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    if (strlen(this->password.c_str()) == 0)
    {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    // future dns option inspired by?
    // https://github.com/craftmetrics/esp32-dns-server/blob/master/dns_server.c

    // other option is captive portal url via dhcp option
    // or wait unitl esp-idf implements proper dns

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA)); // ap/station mode, so we can scan for networks
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wifi Access Point finished. ssid:%s password:%s channel:%d", this->Hostname.c_str(), this->password.c_str(), this->apChannel);
}

json WiFiConnect::Scan()
{
    uint16_t number = CONFIG_WIFI_PROV_SCAN_MAX_ENTRIES;
    wifi_ap_record_t ap_info[CONFIG_WIFI_PROV_SCAN_MAX_ENTRIES];
    memset(ap_info, 0, sizeof(ap_info));
    uint16_t ap_count = 0;

    wifi_scan_config_t scanConfig = {};
    // run an active scan, a passive scan doesn't find all networks
    scanConfig.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scanConfig.show_hidden = true;

    esp_wifi_scan_start(&scanConfig, true);

    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&number, ap_info));

    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));
    ESP_LOGI(TAG, "Total APs scanned = %u", ap_count);

    json jNetworks = json::array({});

    for (int idx = 0; (idx < number) && (idx < ap_count); ++idx)
    {
        string ssid = (char *)ap_info[idx].ssid;

        string authMode;

        switch (ap_info[idx].authmode)
        {
        case WIFI_AUTH_OPEN:
            authMode = "Open";
            break;
        case WIFI_AUTH_WPA_PSK:
            authMode = "WPA";
            break;
        case WIFI_AUTH_WPA2_PSK:
            authMode = "WPA2";
            break;
        case WIFI_AUTH_WPA_WPA2_PSK:
            authMode = "WPA/WPA2";
            break;
        case WIFI_AUTH_WPA3_PSK:
            authMode = "WPA3";
            break;
        case WIFI_AUTH_WPA2_WPA3_PSK:
            authMode = "WPA2/WPA3";
            break;
        default:
            authMode = "Unsupported";
            break;
        }

        ESP_LOGI(TAG, "SSID: %s, RSSI: %d, Channel:%d, AuthMode:%s", ssid.c_str(), ap_info[idx].rssi, ap_info[idx].primary, authMode.c_str());

        json jNetwork;
        jNetwork["ssid"] = ssid;
        jNetwork["rssi"] = ap_info[idx].rssi;
        jNetwork["channel"] = ap_info[idx].primary;
        jNetwork["authMode"] = authMode;
        jNetworks.push_back(jNetwork);
    }

    return jNetworks;
}

json WiFiConnect::GetSettingsJson()
{
    json jWifiSettings;
    jWifiSettings["ssid"] = this->ssid;
    jWifiSettings["password"] = this->password;
    jWifiSettings["enableAP"] = this->enableAP;
    jWifiSettings["maxPower"] = this->maxWifiPower;

    return jWifiSettings;
}

void WiFiConnect::SaveSettingsJson(json config)
{
    if (!config["ssid"].is_null() && config["ssid"].is_string())
    {
        this->ssid = config["ssid"];
    }

    if (!config["password"].is_null() && config["password"].is_string())
    {
        this->password = config["password"];
    }

    if (!config["enableAP"].is_null() && config["enableAP"].is_boolean())
    {
        this->enableAP = config["enableAP"];
    }

    if (!config["maxPower"].is_null() && config["maxPower"].is_number())
    {
        this->maxWifiPower = config["maxPower"];
    }

    this->saveSettings();
}
