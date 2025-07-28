/*
 * esp-brew-engine
 * Copyright (C) Dekien Jeroen 2024
 */
#include "settings-manager.h"

using namespace std;

static const char *TAG = "SettingsManager";

SettingsManager::SettingsManager()
{
    ESP_LOGI(TAG, "SettingsManager Construct");
}

void SettingsManager::Init()
{
    // Initialize NVS
    ESP_LOGI(TAG, "NVS partition Init: Start");
    esp_err_t partInit = nvs_flash_init();
    if (partInit == ESP_ERR_NVS_NO_FREE_PAGES || partInit == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGI(TAG, "NVS partition was truncated and needs to be erased");
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        partInit = nvs_flash_init();
        ESP_ERROR_CHECK(partInit);
    }
    else if (partInit != ESP_OK)
    {
        printf("Error (%s) NVS Flash init!\n", esp_err_to_name(partInit));
    }
    ESP_LOGI(TAG, "NVS partition Init: Done");

    ESP_ERROR_CHECK(nvs_open(this->Namespace.c_str(), NVS_READWRITE, this->nvsHandle));

    nvs_stats_t nvs_stats;
    nvs_get_stats("nvs", &nvs_stats);

    ESP_LOGI(TAG, "NVS Used:%d Free:%d Total:%d", nvs_stats.used_entries, nvs_stats.free_entries, nvs_stats.total_entries);

    size_t used;
    nvs_get_used_entry_count(*this->nvsHandle, &used);

    ESP_LOGI(TAG, "NVS Used:%d", used);
}

void SettingsManager::FactoryReset()
{
    // Reset NVS
    ESP_LOGI(TAG, "FactoryReset: Start");

    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());

    ESP_LOGI(TAG, "FactoryReset: Done");
}

// maby an option for the future, atm it just seems to make it more complex
// template <typename T>
// T *SettingsManager::Read(string name, T *defaultValue)
// {
//     if constexpr (std::is_integral_v<T>)
//     { // constexpr only necessary on first statement
//         ESP_LOGI(TAG, "Read Int");
//     }
//     else if (std::is_floating_point_v<T>)
//     { // automatically constexpr
//         ESP_LOGI(TAG, "Read float");
//     }
//     else if (is_string<T, std::string>)
//     { // automatically constexpr
//         ESP_LOGI(TAG, "Read string");
//     }
// }

string SettingsManager::Read(string name, string defaultValue)
{
    size_t size = 0;
    esp_err_t err = nvs_get_str(*this->nvsHandle, name.c_str(), NULL, &size);

    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGE(TAG, "Error reading string setting: %s - %s", name.c_str(), esp_err_to_name(err));
        
        // If string read failed, try reading as blob (for long Firebase tokens)
        size_t blob_size = 0;
        esp_err_t blob_err = nvs_get_blob(*this->nvsHandle, name.c_str(), NULL, &blob_size);
        
        if (blob_err == ESP_OK && blob_size > 0)
        {
            ESP_LOGI(TAG, "Found blob storage for setting: %s (size: %d)", name.c_str(), blob_size);
            
            char *blob_chars = (char *)malloc(blob_size);
            blob_err = nvs_get_blob(*this->nvsHandle, name.c_str(), blob_chars, &blob_size);
            
            if (blob_err == ESP_OK)
            {
                string result(blob_chars, blob_size - 1); // -1 to exclude null terminator
                
                // Debug logging for blob reads
                if (name == "fbUrl" && !result.empty()) {
                    ESP_LOGI(TAG, "Read fbUrl from blob: len=%d, first char code=%d, content: '%s'", 
                             result.length(), (int)result[0], result.c_str());
                }
                
                free(blob_chars);
                return result;
            }
            else
            {
                ESP_LOGE(TAG, "Error reading blob setting: %s - %s", name.c_str(), esp_err_to_name(blob_err));
                free(blob_chars);
            }
        }
    }

    if (size == 0)
    {
        // does not exist yet, we save the default
        this->Write(name, defaultValue);
        return defaultValue;
    }

    char *chars = (char *)malloc(size);

    err = nvs_get_str(*this->nvsHandle, name.c_str(), chars, &size);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Error reading Setting: %s", name.c_str());
        free(chars);
        return defaultValue;
    }

    string result(chars);
    free(chars);
    
    // Debug logging for specific Firebase URL reading
    if (name == "fbUrl" && !result.empty()) {
        ESP_LOGI(TAG, "Read fbUrl: len=%d, first char code=%d, content: '%s'", 
                 result.length(), (int)result[0], result.c_str());
    }
    
    return result;
}

vector<uint8_t> SettingsManager::Read(string name, vector<uint8_t> defaultValue)
{

    size_t size = 0;
    esp_err_t err = nvs_get_blob(*this->nvsHandle, name.c_str(), NULL, &size);

    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGE(TAG, "Error reading Setting: %s", name.c_str());
    }

    if (size == 0)
    {
        // does not exist yet, we save the default
        this->Write(name, defaultValue);
        return defaultValue;
    }

    ESP_LOGD(TAG, "Size: %d", size);

    uint8_t blob[size];

    err = nvs_get_blob(*this->nvsHandle, name.c_str(), blob, &size);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Error reading Setting: %s", name.c_str());
    }

    vector<uint8_t> v_blob = vector(blob, *(&blob + 1));

    return v_blob;
}

bool SettingsManager::Read(string name, bool defaultValue)
{
    uint8_t value = 0;
    esp_err_t err = nvs_get_u8(*this->nvsHandle, name.c_str(), &value);

    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGE(TAG, "Error reading Setting: %s", name.c_str());
    }

    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        // does not exist yet, we save the default
        this->Write(name, defaultValue);
        return defaultValue;
    }

    return (bool)value;
}

uint8_t SettingsManager::Read(string name, uint8_t defaultValue)
{
    uint8_t value = 0;
    esp_err_t err = nvs_get_u8(*this->nvsHandle, name.c_str(), &value);

    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGE(TAG, "Error reading Setting: %s", name.c_str());
    }

    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        // does not exist yet, we save the default
        this->Write(name, defaultValue);
        return defaultValue;
    }

    return value;
}

int8_t SettingsManager::Read(string name, int8_t defaultValue)
{
    int8_t value = 0;
    esp_err_t err = nvs_get_i8(*this->nvsHandle, name.c_str(), &value);

    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGE(TAG, "Error reading Setting: %s", name.c_str());
    }

    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        // does not exist yet, we save the default
        this->Write(name, defaultValue);
        return defaultValue;
    }

    return value;
}

uint16_t SettingsManager::Read(string name, uint16_t defaultValue)
{
    uint16_t value = 0;
    esp_err_t err = nvs_get_u16(*this->nvsHandle, name.c_str(), &value);

    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGE(TAG, "Error reading Setting: %s", name.c_str());
    }

    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        // does not exist yet, we save the default
        this->Write(name, defaultValue);
        return defaultValue;
    }

    return value;
}

void SettingsManager::Write(string name, string value)
{
    esp_err_t err = nvs_set_str(*this->nvsHandle, name.c_str(), value.c_str());

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Error writing Setting: %s - %s (len: %d)", name.c_str(), esp_err_to_name(err), value.length());
        
        // If string is too large for nvs_set_str, try blob storage for very long values
        if (err == ESP_ERR_NVS_VALUE_TOO_LONG && value.length() > 4000)
        {
            ESP_LOGI(TAG, "String too long for NVS str, trying blob storage for: %s", name.c_str());
            
            // Store as blob for very long strings
            err = nvs_set_blob(*this->nvsHandle, name.c_str(), value.c_str(), value.length() + 1);
            
            if (err == ESP_OK)
            {
                ESP_LOGI(TAG, "Successfully stored long string as blob: %s", name.c_str());
            }
            else
            {
                ESP_LOGE(TAG, "Failed to store long string as blob: %s - %s", name.c_str(), esp_err_to_name(err));
            }
        }
    }
}

void SettingsManager::Write(string name, vector<uint8_t> value)
{
    int size = value.size();
    uint8_t arr[size];
    copy(value.begin(), value.end(), arr);

    esp_err_t err = nvs_set_blob(*this->nvsHandle, name.c_str(), arr, size);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Error writing Setting: %s", name.c_str());
    }
}

void SettingsManager::Write(string name, bool value)
{
    esp_err_t err = nvs_set_u8(*this->nvsHandle, name.c_str(), (uint8_t)value);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Error writing Setting: %s", name.c_str());
    }
}

void SettingsManager::Write(string name, uint8_t value)
{
    esp_err_t err = nvs_set_u8(*this->nvsHandle, name.c_str(), value);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Error writing Setting: %s", name.c_str());
    }
}

void SettingsManager::Write(string name, int8_t value)
{
    esp_err_t err = nvs_set_i8(*this->nvsHandle, name.c_str(), value);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Error writing Setting: %s", name.c_str());
    }
}

void SettingsManager::Write(string name, uint16_t value)
{
    esp_err_t err = nvs_set_u16(*this->nvsHandle, name.c_str(), value);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Error writing Setting: %s", name.c_str());
    }
}