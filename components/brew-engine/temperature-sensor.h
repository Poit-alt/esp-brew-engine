#ifndef _TemperatureSensor_H_
#define _TemperatureSensor_H_

#include "nlohmann_json.hpp"
#include "ds18b20.h"
#include "max31865_driver.h"
#include <cstring>

using namespace std;
using json = nlohmann::json;

enum SensorType {
    SENSOR_DS18B20,
    SENSOR_PT100,
    SENSOR_PT1000,
    SENSOR_NTC
};

class TemperatureSensor
{
public:
    uint64_t id;
    string name;
    string color;
    bool show;
    bool useForControl;
    bool connected;
    float compensateAbsolute;
    float compensateRelative;
    float lastTemp;
    SensorType sensorType;
    
    // Sensor-specific handles
    ds18b20_device_handle_t ds18b20Handle;
    max31865_t max31865Handle;
    
    // NTC sensor configuration
    int analogPin;          // GPIO pin for analog reading
    float ntcResistance;    // NTC resistance at 25°C (in ohms)
    float dividerResistor;  // Voltage divider resistor value (in ohms)
    
    // RTD sensor recovery tracking
    int consecutiveFailures;

    json to_json()
    {
        json jSensor;
        jSensor["id"] = to_string(this->id); // js doesn't support uint64_t, so we convert to string
        jSensor["name"] = this->name;
        jSensor["color"] = this->color;
        jSensor["show"] = this->show;
        jSensor["useForControl"] = this->useForControl;
        jSensor["connected"] = this->connected;
        jSensor["compensateAbsolute"] = this->compensateAbsolute;
        jSensor["compensateRelative"] = this->compensateRelative;
        jSensor["lastTemp"] = (double)((int)(this->lastTemp * 10)) / 10; // round float to 0.1 for display
        jSensor["sensorType"] = this->sensorType;
        
        // Include CS pin for RTD sensors
        if (this->sensorType == SENSOR_PT100 || this->sensorType == SENSOR_PT1000) {
            int csPin = (int)(this->id - 0x31865000);
            if (csPin >= 0 && csPin < 256) { // Valid GPIO range
                jSensor["csPin"] = csPin;
            }
        }
        
        // Include NTC sensor configuration
        if (this->sensorType == SENSOR_NTC) {
            int analogPin = (int)(this->id - 0x4E544300); // "NTC" base ID + pin
            if (analogPin >= 0 && analogPin < 256) { // Valid GPIO range
                jSensor["analogPin"] = analogPin;
            }
            jSensor["ntcResistance"] = this->ntcResistance;
            jSensor["dividerResistor"] = this->dividerResistor;
        }

        return jSensor;
    };

    void from_json(const json &jsonData)
    {
        string stringId = jsonData["id"].get<string>(); // js doesn't support uint64_t, so we convert it from string
        uint64_t sensorId = std::stoull(stringId);

        this->id = sensorId;
        this->name = (string)jsonData["name"];
        this->color = (string)jsonData["color"];

        if (!jsonData["show"].is_null() && jsonData["show"].is_boolean())
        {
            this->show = jsonData["show"];
        }
        else
        {
            this->show = true;
        }

        if (!jsonData["useForControl"].is_null() && jsonData["useForControl"].is_boolean())
        {
            this->useForControl = jsonData["useForControl"];
        }
        else
        {
            this->useForControl = true;
        }

        if (!jsonData["compensateAbsolute"].is_null() && jsonData["compensateAbsolute"].is_number_float())
        {
            this->compensateAbsolute = (float)jsonData["compensateAbsolute"];
        }
        else
        {
            this->compensateAbsolute = 0;
        }

        if (!jsonData["compensateRelative"].is_null() && jsonData["compensateRelative"].is_number_float())
        {
            this->compensateRelative = (float)jsonData["compensateRelative"];
        }
        else
        {
            this->compensateRelative = 1;
        }

        if (jsonData.contains("sensorType") && !jsonData["sensorType"].is_null() && jsonData["sensorType"].is_number_integer())
        {
            this->sensorType = (SensorType)jsonData["sensorType"];
        }
        else
        {
            this->sensorType = SENSOR_DS18B20; // default to DS18B20 for backward compatibility
        }

        // will be set by detection
        this->connected = false;
        this->consecutiveFailures = 0;
        
        // Initialize hardware handles to safe defaults
        this->ds18b20Handle = nullptr;
        memset(&this->max31865Handle, 0, sizeof(this->max31865Handle));
        
        // Initialize NTC configuration from JSON or defaults
        if (this->sensorType == SENSOR_NTC)
        {
            // Extract analog pin from sensor ID
            this->analogPin = (int)(this->id - 0x4E544300);
            
            // Load NTC configuration from JSON
            if (jsonData.contains("ntcResistance") && !jsonData["ntcResistance"].is_null() && jsonData["ntcResistance"].is_number())
            {
                this->ntcResistance = (float)jsonData["ntcResistance"];
            }
            else
            {
                this->ntcResistance = 10000.0f; // 10k NTC default
            }
            
            if (jsonData.contains("dividerResistor") && !jsonData["dividerResistor"].is_null() && jsonData["dividerResistor"].is_number())
            {
                this->dividerResistor = (float)jsonData["dividerResistor"];
            }
            else
            {
                this->dividerResistor = 10000.0f; // 10k voltage divider default
            }
        }
        else
        {
            // Initialize NTC configuration to safe defaults for non-NTC sensors
            this->analogPin = 0;
            this->ntcResistance = 10000.0f;
            this->dividerResistor = 10000.0f;
        }
    };

protected:
private:
};

#endif /* _TemperatureSensor_H_ */