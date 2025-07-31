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
    SENSOR_PT1000
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
    };

protected:
private:
};

#endif /* _TemperatureSensor_H_ */