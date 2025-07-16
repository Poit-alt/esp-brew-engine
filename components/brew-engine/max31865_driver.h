#ifndef MAX31865_DRIVER_H
#define MAX31865_DRIVER_H

#include <stdint.h>
#include "esp_err.h"
#include "hal/gpio_types.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

// MAX31865 Register addresses
#define MAX31865_REG_CONFIG         0x00
#define MAX31865_REG_RTD_MSB        0x01
#define MAX31865_REG_RTD_LSB        0x02
#define MAX31865_REG_HIGH_FAULT_MSB 0x03
#define MAX31865_REG_HIGH_FAULT_LSB 0x04
#define MAX31865_REG_LOW_FAULT_MSB  0x05
#define MAX31865_REG_LOW_FAULT_LSB  0x06
#define MAX31865_REG_FAULT_STATUS   0x07

// Configuration register bits
#define MAX31865_CONFIG_BIAS        0x80
#define MAX31865_CONFIG_MODEAUTO    0x40
#define MAX31865_CONFIG_MODEOFF     0x00
#define MAX31865_CONFIG_1SHOT       0x20
#define MAX31865_CONFIG_3WIRE       0x10
#define MAX31865_CONFIG_4WIRE       0x00
#define MAX31865_CONFIG_FAULTSTAT   0x02
#define MAX31865_CONFIG_FILT50HZ    0x01
#define MAX31865_CONFIG_FILT60HZ    0x00

// Fault status register bits
#define MAX31865_FAULT_HIGHTHRESH   0x80
#define MAX31865_FAULT_LOWTHRESH    0x40
#define MAX31865_FAULT_REFINLOW     0x20
#define MAX31865_FAULT_REFINHIGH    0x10
#define MAX31865_FAULT_RTDINLOW     0x08
#define MAX31865_FAULT_OVUV         0x04

// Error codes for RTD faults
#define MAX31865_ERROR_DISCONNECTED -1000
#define MAX31865_ERROR_SHORT_CIRCUIT -1001
#define MAX31865_ERROR_OVERVOLTAGE -1002

// RTD constants
#define MAX31865_RTD_A    3.9083e-3
#define MAX31865_RTD_B    -5.775e-7
#define MAX31865_RTD_C    -4.183e-12

typedef struct {
    spi_device_handle_t spi;
    uint8_t config;
    uint16_t rtd_nominal;     // 100 for PT100, 1000 for PT1000
    uint16_t ref_resistor;    // 430 for PT100, 4300 for PT1000
} max31865_t;

/**
 * @brief Initialize SPI bus for MAX31865 (call once)
 * 
 * @param host SPI host
 * @param mosi_pin MOSI pin
 * @param miso_pin MISO pin
 * @param sclk_pin SCLK pin
 * @return ESP_OK on success
 */
esp_err_t max31865_init_bus(spi_host_device_t host, int mosi_pin, int miso_pin, int sclk_pin);

/**
 * @brief Initialize MAX31865 device descriptor
 * 
 * @param dev MAX31865 device descriptor
 * @param host SPI host
 * @param cs_pin Chip select pin
 * @return ESP_OK on success
 */
esp_err_t max31865_init_desc(max31865_t *dev, spi_host_device_t host, int cs_pin);

/**
 * @brief Set MAX31865 configuration
 * 
 * @param dev MAX31865 device descriptor
 * @param vbias Enable bias voltage
 * @param conversion_mode Conversion mode (auto/manual)
 * @param one_shot One shot mode
 * @param three_wire Three wire mode
 * @param fault_cycle Fault cycle setting
 * @param fault_clear Clear fault
 * @param filter_50hz Enable 50Hz filter
 * @param low_threshold Low fault threshold
 * @param high_threshold High fault threshold
 * @return ESP_OK on success
 */
esp_err_t max31865_set_config(max31865_t *dev, bool vbias, uint8_t conversion_mode, 
                               bool one_shot, bool three_wire, uint8_t fault_cycle, 
                               bool fault_clear, bool filter_50hz, uint16_t low_threshold, 
                               uint16_t high_threshold);

/**
 * @brief Read temperature from MAX31865
 * 
 * @param dev MAX31865 device descriptor
 * @param resistance Pointer to store RTD resistance value
 * @param temperature Pointer to store temperature value
 * @return ESP_OK on success
 */
esp_err_t max31865_measure(max31865_t *dev, float *resistance, float *temperature);

/**
 * @brief Read raw RTD value from MAX31865
 * 
 * @param dev MAX31865 device descriptor
 * @param rtd_value Pointer to store raw RTD value
 * @return ESP_OK on success
 */
esp_err_t max31865_read_rtd(max31865_t *dev, uint16_t *rtd_value);

/**
 * @brief Calculate temperature from RTD resistance
 * 
 * @param resistance RTD resistance value
 * @param rtd_nominal Nominal RTD resistance (100 for PT100, 1000 for PT1000)
 * @return Temperature in Celsius
 */
float max31865_calculate_temperature(float resistance, uint16_t rtd_nominal);

#ifdef __cplusplus
}
#endif

#endif // MAX31865_DRIVER_H