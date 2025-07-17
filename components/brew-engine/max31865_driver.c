#include "max31865_driver.h"
#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "MAX31865";

// Write to MAX31865 register
static esp_err_t max31865_write_reg(max31865_t *dev, uint8_t reg, uint8_t value)
{
    esp_err_t ret;
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    
    uint8_t tx_data[2] = {0x80 | reg, value}; // Set write bit
    
    t.length = 16;
    t.tx_buffer = tx_data;
    
    ret = spi_device_transmit(dev->spi, &t);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write register 0x%02x: %s", reg, esp_err_to_name(ret));
    }
    
    return ret;
}

// Read from MAX31865 register
static esp_err_t max31865_read_reg(max31865_t *dev, uint8_t reg, uint8_t *value)
{
    esp_err_t ret;
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    
    uint8_t tx_data[2] = {reg, 0x00}; // Clear write bit for read
    uint8_t rx_data[2];
    
    t.length = 16;
    t.tx_buffer = tx_data;
    t.rx_buffer = rx_data;
    
    ret = spi_device_transmit(dev->spi, &t);
    if (ret == ESP_OK) {
        *value = rx_data[1];
    } else {
        ESP_LOGE(TAG, "Failed to read register 0x%02x: %s", reg, esp_err_to_name(ret));
    }
    
    return ret;
}

// Read 16-bit value from MAX31865
static esp_err_t max31865_read_reg16(max31865_t *dev, uint8_t reg, uint16_t *value)
{
    esp_err_t ret;
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    
    uint8_t tx_data[3] = {reg, 0x00, 0x00}; // Clear write bit for read
    uint8_t rx_data[3];
    
    t.length = 24;
    t.tx_buffer = tx_data;
    t.rx_buffer = rx_data;
    
    ret = spi_device_transmit(dev->spi, &t);
    if (ret == ESP_OK) {
        *value = (rx_data[1] << 8) | rx_data[2];
    } else {
        ESP_LOGE(TAG, "Failed to read 16-bit register 0x%02x: %s", reg, esp_err_to_name(ret));
    }
    
    return ret;
}

esp_err_t max31865_init_bus(spi_host_device_t host, int mosi_pin, int miso_pin, int sclk_pin)
{
    esp_err_t ret;
    
    // Initialize SPI bus config
    spi_bus_config_t buscfg = {
        .mosi_io_num = mosi_pin,
        .miso_io_num = miso_pin,
        .sclk_io_num = sclk_pin,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096
    };
    
    // Initialize SPI bus
    ret = spi_bus_initialize(host, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "MAX31865 SPI bus initialized successfully");
    return ESP_OK;
}

esp_err_t max31865_init_desc(max31865_t *dev, spi_host_device_t host, int cs_pin)
{
    esp_err_t ret;
    
    // Validate CS pin
    if (cs_pin < 0 || cs_pin >= GPIO_NUM_MAX) {
        ESP_LOGE(TAG, "Invalid CS pin: %d", cs_pin);
        return ESP_ERR_INVALID_ARG;
    }
    
    // SPI device configuration
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 1000000,  // 1 MHz
        .mode = 1,                  // SPI mode 1
        .spics_io_num = cs_pin,
        .queue_size = 1,
        .command_bits = 0,
        .address_bits = 0,
        .dummy_bits = 0
    };
    
    // Add device to SPI bus
    ret = spi_bus_add_device(host, &devcfg, &dev->spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SPI device (CS pin %d): %s", cs_pin, esp_err_to_name(ret));
        return ret;
    }
    
    // Set default values
    dev->config = MAX31865_CONFIG_BIAS | MAX31865_CONFIG_MODEAUTO | MAX31865_CONFIG_FILT50HZ;
    dev->rtd_nominal = 100;    // Default to PT100
    dev->ref_resistor = 430;   // Default reference resistor for PT100
    
    ESP_LOGI(TAG, "MAX31865 device initialized successfully on CS pin %d", cs_pin);
    return ESP_OK;
}

esp_err_t max31865_set_config(max31865_t *dev, bool vbias, uint8_t conversion_mode, 
                               bool one_shot, bool three_wire, uint8_t fault_cycle, 
                               bool fault_clear, bool filter_50hz, uint16_t low_threshold, 
                               uint16_t high_threshold)
{
    esp_err_t ret;
    
    // Build configuration register
    uint8_t config = 0;
    if (vbias) config |= MAX31865_CONFIG_BIAS;
    if (conversion_mode) config |= MAX31865_CONFIG_MODEAUTO;
    if (one_shot) config |= MAX31865_CONFIG_1SHOT;
    if (three_wire) config |= MAX31865_CONFIG_3WIRE;
    if (fault_clear) config |= MAX31865_CONFIG_FAULTSTAT;
    if (filter_50hz) config |= MAX31865_CONFIG_FILT50HZ;
    
    dev->config = config;
    
    // Write configuration
    ret = max31865_write_reg(dev, MAX31865_REG_CONFIG, config);
    if (ret != ESP_OK) {
        return ret;
    }
    
    // Set thresholds
    ret = max31865_write_reg(dev, MAX31865_REG_HIGH_FAULT_MSB, (high_threshold >> 8) & 0xFF);
    if (ret != ESP_OK) return ret;
    
    ret = max31865_write_reg(dev, MAX31865_REG_HIGH_FAULT_LSB, high_threshold & 0xFF);
    if (ret != ESP_OK) return ret;
    
    ret = max31865_write_reg(dev, MAX31865_REG_LOW_FAULT_MSB, (low_threshold >> 8) & 0xFF);
    if (ret != ESP_OK) return ret;
    
    ret = max31865_write_reg(dev, MAX31865_REG_LOW_FAULT_LSB, low_threshold & 0xFF);
    if (ret != ESP_OK) return ret;
    
    ESP_LOGI(TAG, "MAX31865 configuration set successfully");
    return ESP_OK;
}

esp_err_t max31865_read_rtd(max31865_t *dev, uint16_t *rtd_value)
{
    esp_err_t ret;
    
    // Read RTD value
    ret = max31865_read_reg16(dev, MAX31865_REG_RTD_MSB, rtd_value);
    if (ret != ESP_OK) {
        return ret;
    }
    
    // Check fault bit (bit 0)
    if (*rtd_value & 0x01) {
        uint8_t fault_status;
        ret = max31865_read_reg(dev, MAX31865_REG_FAULT_STATUS, &fault_status);
        if (ret != ESP_OK) {
            return ret;
        }
        
        ESP_LOGW(TAG, "RTD fault detected - Status: 0x%02x", fault_status);
        
        // Check for specific fault conditions
        if (fault_status & MAX31865_FAULT_RTDINLOW) {
            ESP_LOGW(TAG, "RTD probe disconnected or open circuit");
            return ESP_ERR_NOT_FOUND; // Indicates disconnected probe
        }
        
        if (fault_status & MAX31865_FAULT_REFINLOW) {
            ESP_LOGW(TAG, "Reference resistor too low");
            return ESP_ERR_INVALID_RESPONSE;
        }
        
        if (fault_status & MAX31865_FAULT_REFINHIGH) {
            ESP_LOGW(TAG, "Reference resistor too high");
            return ESP_ERR_INVALID_RESPONSE;
        }
        
        if (fault_status & MAX31865_FAULT_OVUV) {
            ESP_LOGW(TAG, "Overvoltage/Undervoltage fault");
            return ESP_ERR_INVALID_RESPONSE;
        }
        
        // Clear fault status by writing to config register
        max31865_write_reg(dev, MAX31865_REG_CONFIG, dev->config | MAX31865_CONFIG_FAULTSTAT);
        vTaskDelay(pdMS_TO_TICKS(10)); // Small delay for fault clearing
        max31865_write_reg(dev, MAX31865_REG_CONFIG, dev->config);
        
        return ESP_ERR_INVALID_RESPONSE;
    }
    
    // Remove fault bit
    *rtd_value >>= 1;
    
    return ESP_OK;
}

float max31865_calculate_temperature(float resistance, uint16_t rtd_nominal)
{
    // Use simplified RTD calculation for PT100/PT1000
    // T = (R - R0) / (R0 * α) where α ≈ 0.00385
    float alpha = 0.00385;
    float temperature = (resistance - rtd_nominal) / (rtd_nominal * alpha);
    
    return temperature;
}

esp_err_t max31865_measure(max31865_t *dev, float *resistance, float *temperature)
{
    esp_err_t ret;
    uint16_t rtd_value;
    
    // Read RTD value
    ret = max31865_read_rtd(dev, &rtd_value);
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_NOT_FOUND) {
            // Probe disconnected - return error values
            *resistance = 0.0;
            *temperature = -999.0; // Clearly invalid temperature
            ESP_LOGW(TAG, "RTD probe disconnected");
        }
        return ret;
    }
    
    // Convert to resistance
    *resistance = (float)rtd_value * dev->ref_resistor / 32768.0;
    
    // Check for unreasonable resistance values that indicate hardware issues
    float expected_min = dev->rtd_nominal * 0.6;  // 60Ω for PT100, 600Ω for PT1000 (around -100°C)
    float expected_max = dev->rtd_nominal * 2.0;   // 200Ω for PT100, 2000Ω for PT1000 (around +260°C)
    
    if (*resistance < expected_min || *resistance > expected_max) {
        ESP_LOGW(TAG, "RTD resistance out of range: %.2f Ω (expected %.2f - %.2f Ω)", 
                 *resistance, expected_min, expected_max);
        *temperature = -999.0; // Invalid temperature
        return ESP_ERR_INVALID_RESPONSE;
    }
    
    // Calculate temperature
    *temperature = max31865_calculate_temperature(*resistance, dev->rtd_nominal);
    
    // Sanity check temperature range for brewing applications
    if (*temperature < -40.0 || *temperature > 200.0) {
        ESP_LOGW(TAG, "RTD temperature out of brewing range: %.2f °C", *temperature);
        *temperature = -999.0; // Invalid temperature
        return ESP_ERR_INVALID_RESPONSE;
    }
    
    ESP_LOGD(TAG, "RTD value: %d, Resistance: %.2f Ω, Temperature: %.2f °C", 
             rtd_value, *resistance, *temperature);
    
    return ESP_OK;
}