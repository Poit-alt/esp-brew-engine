/*
 * esp-brew-engine
 * Copyright (C) Dekien Jeroen 2024
 */
#include "brew-engine.h"
#include "driver/spi_master.h"

using namespace std;
using json = nlohmann::json;

static const char *TAG = "BrewEngine";

// esp http server only works with static handlers, no other option atm then to save a pointer.
BrewEngine *mainInstance;

BrewEngine::BrewEngine(SettingsManager *settingsManager)
{
	ESP_LOGI(TAG, "BrewEngine Construct");
	this->settingsManager = settingsManager;
	this->statisticsManager = new StatisticsManager(settingsManager);
	mainInstance = this;
}

void BrewEngine::Init()
{
	// read the post important settings first so when van set outputs asap.
	this->readSystemSettings();

	this->readHeaterSettings();

	// init gpio as soon as possible
	if (this->invertOutputs)
	{
		this->gpioHigh = 0;
		this->gpioLow = 1;
	}

	this->initHeaters();

	if (this->stir_PIN == GPIO_NUM_NC || this->stir_PIN >= GPIO_NUM_MAX)
	{
		ESP_LOGW(TAG, "StirPin is not configured or invalid (pin: %d)!", this->stir_PIN);

		this->stirStatusText = "Disabled";
	}
	else
	{
		gpio_reset_pin(this->stir_PIN);
		gpio_set_direction(this->stir_PIN, GPIO_MODE_OUTPUT);
		gpio_set_level(this->stir_PIN, this->gpioLow);
	}

	if (this->buzzer_PIN == GPIO_NUM_NC || this->buzzer_PIN >= GPIO_NUM_MAX)
	{
		ESP_LOGW(TAG, "Buzzer is not configured or invalid (pin: %d)!", this->buzzer_PIN);
	}
	else
	{
		gpio_reset_pin(this->buzzer_PIN);
		gpio_set_direction(this->buzzer_PIN, GPIO_MODE_OUTPUT);
		gpio_set_level(this->buzzer_PIN, this->gpioLow);
	}

	// read other settings like maishschedules and pid
	this->readSettings();

	this->readTempSensorSettings();

	this->initOneWire();

	this->detectOnewireTemperatureSensors();

	this->initRtdSensors();

	this->detectRtdTemperatureSensors();

	this->initMqtt();

	this->initInfluxDB();

	this->statisticsManager->Init();

	this->run = true;

	xTaskCreate(&this->readLoop, "readloop_task", 8192, this, 5, NULL);

	this->server = this->startWebserver();
}

void BrewEngine::initHeaters()
{
	for (auto const &heater : this->heaters)
	{
		ESP_LOGI(TAG, "Heater %s Configured", heater->name.c_str());

		gpio_reset_pin(heater->pinNr);
		gpio_set_direction(heater->pinNr, GPIO_MODE_OUTPUT);
		gpio_set_level(heater->pinNr, this->gpioLow);
	}
}

void BrewEngine::readSystemSettings()
{
	ESP_LOGI(TAG, "Reading System Settings");

	// io settings
	this->oneWire_PIN = (gpio_num_t)this->settingsManager->Read("onewirePin", (uint16_t)CONFIG_ONEWIRE);
	this->stir_PIN = (gpio_num_t)this->settingsManager->Read("stirPin", (uint16_t)CONFIG_STIR);
	this->buzzer_PIN = (gpio_num_t)this->settingsManager->Read("buzzerPin", (uint16_t)CONFIG_BUZZER);
	this->buzzerTime = this->settingsManager->Read("buzzerTime", (uint8_t)2);

	// RTD sensor settings - use shorter keys to avoid NVS limits
	this->rtdSensorsEnabled = this->settingsManager->Read("rtdEnabled", false);
	this->spi_mosi_pin = (gpio_num_t)this->settingsManager->Read("spiMosi", (uint16_t)23);
	this->spi_miso_pin = (gpio_num_t)this->settingsManager->Read("spiMiso", (uint16_t)19);
	this->spi_clk_pin = (gpio_num_t)this->settingsManager->Read("spiClk", (uint16_t)18);
	this->spi_cs_pin = (gpio_num_t)this->settingsManager->Read("spiCs", (uint16_t)5);
	this->rtdSensorCount = 0;

	bool configInvertOutputs = false;
// is there a cleaner way to do this?, config to bool doesn't seem to work properly
#if defined(CONFIG_InvertOutputs)
	configInvertOutputs = true;
#endif
	this->invertOutputs = this->settingsManager->Read("invertOutputs", configInvertOutputs);

	// mqtt
	this->mqttUri = this->settingsManager->Read("mqttUri", (string)CONFIG_MQTT_URI);

	// InfluxDB
	this->influxdbUrl = this->settingsManager->Read("influxdbUrl", (string)"");
	this->influxdbToken = this->settingsManager->Read("influxdbToken", (string)"");
	this->influxdbOrg = this->settingsManager->Read("influxdbOrg", (string)"");
	this->influxdbBucket = this->settingsManager->Read("influxdbBucket", (string)"");
	this->influxdbSendInterval = this->settingsManager->Read("influxdbInt", (uint16_t)10);

	// temperature scale
	uint8_t defaultConfigScale = 0; // default to Celsius
#if defined(CONFIG_SCALE_FAHRENHEIT)
	defaultConfigScale = Fahrenheit;
#endif

	this->temperatureScale = (TemperatureScale)this->settingsManager->Read("tempScale", defaultConfigScale);

	ESP_LOGI(TAG, "Reading System Settings Done");
}

void BrewEngine::saveSystemSettingsJson(const json &config)
{
	ESP_LOGI(TAG, "Saving System Settings");

	if (!config["onewirePin"].is_null() && config["onewirePin"].is_number())
	{
		this->settingsManager->Write("onewirePin", (uint16_t)config["onewirePin"]);
		this->oneWire_PIN = (gpio_num_t)config["onewirePin"];
	}
	if (!config["stirPin"].is_null() && config["stirPin"].is_number())
	{
		this->settingsManager->Write("stirPin", (uint16_t)config["stirPin"]);
		this->stir_PIN = (gpio_num_t)config["stirPin"];
	}
	if (!config["buzzerPin"].is_null() && config["buzzerPin"].is_number())
	{
		this->settingsManager->Write("buzzerPin", (uint16_t)config["buzzerPin"]);
		this->buzzer_PIN = (gpio_num_t)config["buzzerPin"];
	}
	if (!config["buzzerTime"].is_null() && config["buzzerTime"].is_number())
	{
		this->settingsManager->Write("buzzerTime", (uint8_t)config["buzzerTime"]);
		this->buzzerTime = (uint8_t)config["buzzerTime"];
	}
	if (!config["invertOutputs"].is_null() && config["invertOutputs"].is_boolean())
	{
		this->settingsManager->Write("invertOutputs", (bool)config["invertOutputs"]);
		this->invertOutputs = (bool)config["invertOutputs"];
	}
	if (!config["mqttUri"].is_null() && config["mqttUri"].is_string())
	{
		this->settingsManager->Write("mqttUri", (string)config["mqttUri"]);
		this->mqttUri = config["mqttUri"];
	}
	
	// InfluxDB configuration
	if (!config["influxdbUrl"].is_null() && config["influxdbUrl"].is_string())
	{
		this->settingsManager->Write("influxdbUrl", (string)config["influxdbUrl"]);
		this->influxdbUrl = config["influxdbUrl"];
	}
	if (!config["influxdbToken"].is_null() && config["influxdbToken"].is_string())
	{
		this->settingsManager->Write("influxdbToken", (string)config["influxdbToken"]);
		this->influxdbToken = config["influxdbToken"];
	}
	if (!config["influxdbOrg"].is_null() && config["influxdbOrg"].is_string())
	{
		this->settingsManager->Write("influxdbOrg", (string)config["influxdbOrg"]);
		this->influxdbOrg = config["influxdbOrg"];
	}
	if (!config["influxdbBucket"].is_null() && config["influxdbBucket"].is_string())
	{
		this->settingsManager->Write("influxdbBucket", (string)config["influxdbBucket"]);
		this->influxdbBucket = config["influxdbBucket"];
	}
	if (!config["influxdbSendInterval"].is_null() && config["influxdbSendInterval"].is_number())
	{
		uint16_t interval = config["influxdbSendInterval"];
		// Validate interval is between 1 and 300 seconds
		if (interval >= 1 && interval <= 300)
		{
			this->settingsManager->Write("influxdbInt", interval);
			this->influxdbSendInterval = interval;
		}
	}
	
	if (!config["temperatureScale"].is_null() && config["temperatureScale"].is_number())
	{
		uint8_t scale = (uint8_t)config["temperatureScale"];
		this->settingsManager->Write("tempScale", scale); // key is limited to x chars so we shorten it
		this->temperatureScale = (TemperatureScale)config["temperatureScale"];
	}

	// RTD sensor configuration - use shorter keys to avoid NVS limits
	if (!config["rtdSensorsEnabled"].is_null() && config["rtdSensorsEnabled"].is_boolean())
	{
		this->settingsManager->Write("rtdEnabled", (bool)config["rtdSensorsEnabled"]);
		this->rtdSensorsEnabled = (bool)config["rtdSensorsEnabled"];
	}
	if (!config["spiMosiPin"].is_null() && config["spiMosiPin"].is_number())
	{
		this->settingsManager->Write("spiMosi", (uint16_t)config["spiMosiPin"]);
		this->spi_mosi_pin = (gpio_num_t)config["spiMosiPin"];
	}
	if (!config["spiMisoPin"].is_null() && config["spiMisoPin"].is_number())
	{
		this->settingsManager->Write("spiMiso", (uint16_t)config["spiMisoPin"]);
		this->spi_miso_pin = (gpio_num_t)config["spiMisoPin"];
	}
	if (!config["spiClkPin"].is_null() && config["spiClkPin"].is_number())
	{
		this->settingsManager->Write("spiClk", (uint16_t)config["spiClkPin"]);
		this->spi_clk_pin = (gpio_num_t)config["spiClkPin"];
	}
	if (!config["spiCsPin"].is_null() && config["spiCsPin"].is_number())
	{
		this->settingsManager->Write("spiCs", (uint16_t)config["spiCsPin"]);
		this->spi_cs_pin = (gpio_num_t)config["spiCsPin"];
	}

	ESP_LOGI(TAG, "Saving System Settings Done");
}

void BrewEngine::readSettings()
{
	ESP_LOGI(TAG, "Reading Settings");

	vector<uint8_t> empty = json::to_msgpack(json::array({}));
	vector<uint8_t> serialized = this->settingsManager->Read("mashschedules", empty);

	json jSchedules = json::from_msgpack(serialized);

	if (jSchedules.empty())
	{
		ESP_LOGI(TAG, "Adding Default Mash Schedules");
		this->addDefaultMash();
		this->saveMashSchedules();
	}
	else
	{
		for (const auto &el : jSchedules.items())
		{
			json jSchedule = el.value();

			auto schedule = new MashSchedule();
			schedule->from_json(jSchedule);

			this->mashSchedules.insert_or_assign(schedule->name, schedule);
		}
	}

	// we save and load pid doubles as unit16 becease nvs doesnt' have double support, and we are happy with only 1 decimal
	uint16_t pint = this->settingsManager->Read("kP", (uint16_t)(this->mashkP * 10));
	uint16_t iint = this->settingsManager->Read("kI", (uint16_t)(this->mashkI * 10));
	uint16_t dint = this->settingsManager->Read("kD", (uint16_t)(this->mashkD * 10));

	this->mashkP = (double)pint / 10;
	this->mashkI = (double)iint / 10;
	this->mashkD = (double)dint / 10;

	uint16_t bpint = this->settingsManager->Read("boilkP", (uint16_t)(this->boilkP * 10));
	uint16_t biint = this->settingsManager->Read("boilkI", (uint16_t)(this->boilkI * 10));
	uint16_t bdint = this->settingsManager->Read("boilkD", (uint16_t)(this->boilkD * 10));

	this->boilkP = (double)bpint / 10;
	this->boilkI = (double)biint / 10;
	this->boilkD = (double)bdint / 10;

	this->pidLoopTime = this->settingsManager->Read("pidLoopTime", (uint16_t)CONFIG_PID_LOOPTIME);
	this->stepInterval = this->settingsManager->Read("stepInterval", (uint16_t)CONFIG_PID_LOOPTIME); // we use same as pidloop time

	this->boostModeUntil = this->settingsManager->Read("boostModeUntil", (uint8_t)this->boostModeUntil);
}

void BrewEngine::setMashSchedule(const json &jSchedule)
{
	json newSteps = jSchedule["steps"];

	auto newMash = new MashSchedule();
	newMash->name = jSchedule["name"].get<string>();
	newMash->boil = jSchedule["boil"].get<bool>();

	for (auto const &step : newMash->steps)
	{
		delete step;
	}
	newMash->steps.clear();

	for (auto &el : newSteps.items())
	{
		auto jStep = el.value();

		auto newStep = new MashStep();
		newStep->from_json(jStep);
		newMash->steps.push_back(newStep);
	}

	newMash->sort_steps();

	json newNotifications = jSchedule["notifications"];
	for (auto const &notification : newMash->notifications)
	{
		delete notification;
	}
	newMash->notifications.clear();

	for (auto &el : newNotifications.items())
	{
		auto jNotification = el.value();

		auto newNotification = new Notification();
		newNotification->from_json(jNotification);
		newMash->notifications.push_back(newNotification);
	}

	newMash->sort_notifications();

	this->mashSchedules.insert_or_assign(newMash->name, newMash);
}

void BrewEngine::saveMashSchedules()
{
	ESP_LOGI(TAG, "Saving Mash Schedules");

	json jSchedules = json::array({});
	for (auto const &[key, mashSchedule] : this->mashSchedules)
	{

		if (!mashSchedule->temporary)
		{
			json jSchedule = mashSchedule->to_json();
			jSchedules.push_back(jSchedule);
		}
	}

	// serialize to MessagePack for size
	vector<uint8_t> serialized = json::to_msgpack(jSchedules);

	this->settingsManager->Write("mashschedules", serialized);

	ESP_LOGI(TAG, "Saving Mash Schedules Done, %d bytes", serialized.size());
}

void BrewEngine::savePIDSettings()
{
	ESP_LOGI(TAG, "Saving PID Settings");

	uint16_t pint = static_cast<uint16_t>(this->mashkP * 10);
	uint16_t iint = static_cast<uint16_t>(this->mashkI * 10);
	uint16_t dint = static_cast<uint16_t>(this->mashkD * 10);

	this->settingsManager->Write("kP", pint);
	this->settingsManager->Write("kI", iint);
	this->settingsManager->Write("kD", dint);

	uint16_t bpint = static_cast<uint16_t>(this->boilkP * 10);
	uint16_t biint = static_cast<uint16_t>(this->boilkI * 10);
	uint16_t bdint = static_cast<uint16_t>(this->boilkD * 10);

	this->settingsManager->Write("boilkP", bpint);
	this->settingsManager->Write("boilkI", biint);
	this->settingsManager->Write("boilkD", bdint);

	this->settingsManager->Write("pidLoopTime", this->pidLoopTime);
	this->settingsManager->Write("stepInterval", this->stepInterval);

	this->settingsManager->Write("boostModeUntil", this->boostModeUntil);

	ESP_LOGI(TAG, "Saving PID Settings Done");
}

void BrewEngine::addDefaultMash()
{
	auto defaultMash = new MashSchedule();
	defaultMash->name = "Default";
	defaultMash->boil = false;

	auto defaultMash_s1 = new MashStep();
	defaultMash_s1->index = 0;
	defaultMash_s1->name = "Beta Amylase";
	defaultMash_s1->temperature = (this->temperatureScale == Celsius) ? 64 : 150;
	defaultMash_s1->stepTime = 5;
	defaultMash_s1->extendStepTimeIfNeeded = true;
	defaultMash_s1->allowBoost = true;
	defaultMash_s1->time = 45;
	defaultMash->steps.push_back(defaultMash_s1);

	auto defaultMash_s2 = new MashStep();
	defaultMash_s2->index = 1;
	defaultMash_s2->name = "Alpha Amylase";
	defaultMash_s2->temperature = (this->temperatureScale == Celsius) ? 72 : 160;
	defaultMash_s2->stepTime = 5;
	defaultMash_s2->extendStepTimeIfNeeded = true;
	defaultMash_s2->allowBoost = false;
	defaultMash_s2->time = 20;
	defaultMash->steps.push_back(defaultMash_s2);

	auto defaultMash_s3 = new MashStep();
	defaultMash_s3->index = 2;
	defaultMash_s3->name = "Mash Out";
	defaultMash_s3->temperature = (this->temperatureScale == Celsius) ? 78 : 170;
	defaultMash_s3->stepTime = 5;
	defaultMash_s3->extendStepTimeIfNeeded = true;
	defaultMash_s3->allowBoost = false;
	defaultMash_s3->time = 5;
	defaultMash->steps.push_back(defaultMash_s3);

	auto defaultMash_n1 = new Notification();
	defaultMash_n1->name = "Add Grains";
	defaultMash_n1->message = "Please add Grains";
	defaultMash_n1->timeFromStart = 5;
	defaultMash_n1->buzzer = true;
	defaultMash->notifications.push_back(defaultMash_n1);

	auto defaultMash_n2 = new Notification();
	defaultMash_n2->name = "Start Lautering";
	defaultMash_n2->message = "Please Start Lautering/Sparging";
	defaultMash_n2->timeFromStart = 85;
	defaultMash_n2->buzzer = true;
	defaultMash->notifications.push_back(defaultMash_n2);

	this->mashSchedules.insert_or_assign(defaultMash->name, defaultMash);

	auto ryeMash = new MashSchedule();
	ryeMash->name = "Rye Mash";
	ryeMash->boil = false;

	auto ryeMash_s1 = new MashStep();
	ryeMash_s1->index = 0;
	ryeMash_s1->name = "Beta Glucanase";
	ryeMash_s1->temperature = (this->temperatureScale == Celsius) ? 43 : 110;
	ryeMash_s1->stepTime = 5;
	ryeMash_s1->extendStepTimeIfNeeded = true;
	ryeMash_s1->allowBoost = true;
	ryeMash_s1->time = 20;
	ryeMash->steps.push_back(ryeMash_s1);

	auto ryeMash_s2 = new MashStep();
	ryeMash_s2->index = 1;
	ryeMash_s2->name = "Beta Amylase";
	ryeMash_s2->temperature = (this->temperatureScale == Celsius) ? 64 : 150;
	ryeMash_s2->stepTime = 5;
	ryeMash_s2->extendStepTimeIfNeeded = true;
	ryeMash_s2->allowBoost = false;
	ryeMash_s2->time = 45;
	ryeMash->steps.push_back(ryeMash_s2);

	auto ryeMash_s3 = new MashStep();
	ryeMash_s3->index = 2;
	ryeMash_s3->name = "Alpha Amylase";
	ryeMash_s3->temperature = (this->temperatureScale == Celsius) ? 72 : 160;
	ryeMash_s3->stepTime = 5;
	ryeMash_s3->extendStepTimeIfNeeded = true;
	ryeMash_s3->allowBoost = false;
	ryeMash_s3->time = 20;
	ryeMash->steps.push_back(ryeMash_s3);

	auto ryeMash_s4 = new MashStep();
	ryeMash_s4->index = 3;
	ryeMash_s4->name = "Mash Out";
	ryeMash_s4->temperature = (this->temperatureScale == Celsius) ? 78 : 170;
	ryeMash_s4->stepTime = 5;
	ryeMash_s4->extendStepTimeIfNeeded = true;
	ryeMash_s4->allowBoost = false;
	ryeMash_s4->time = 5;
	ryeMash->steps.push_back(ryeMash_s4);

	auto ryeMash_n1 = new Notification();
	ryeMash_n1->name = "Add Grains";
	ryeMash_n1->message = "Please add Grains";
	ryeMash_n1->timeFromStart = 5;
	ryeMash_n1->buzzer = true;
	ryeMash->notifications.push_back(ryeMash_n1);

	auto ryeMash_n2 = new Notification();
	ryeMash_n2->name = "Start Lautering";
	ryeMash_n2->message = "Please Start Lautering/Sparging";
	ryeMash_n2->timeFromStart = 110;
	ryeMash_n2->buzzer = true;
	ryeMash->notifications.push_back(ryeMash_n2);

	this->mashSchedules.insert_or_assign(ryeMash->name, ryeMash);

	auto boil = new MashSchedule();
	boil->name = "Boil 70 Min";
	boil->boil = true;

	auto boil_s1 = new MashStep();
	boil_s1->index = 0;
	boil_s1->name = "Boil";
	boil_s1->temperature = (this->temperatureScale == Celsius) ? 101 : 214;
	boil_s1->stepTime = 0;
	boil_s1->extendStepTimeIfNeeded = true;
	boil_s1->time = 70;
	boil->steps.push_back(boil_s1);

	auto boil_n1 = new Notification();
	boil_n1->name = "Bittering Hops";
	boil_n1->message = "Please add Bittering Hops";
	boil_n1->timeFromStart = 0;
	boil_n1->buzzer = true;
	boil->notifications.push_back(boil_n1);

	auto boil_n2 = new Notification();
	boil_n2->name = "Aroma Hops";
	boil_n2->message = "Please add Aroma Hops";
	boil_n2->timeFromStart = 55;
	boil_n2->buzzer = true;
	boil->notifications.push_back(boil_n2);

	this->mashSchedules.insert_or_assign(boil->name, boil);
}

void BrewEngine::addDefaultHeaters()
{
	auto defaultHeater1 = new Heater();
	defaultHeater1->id = 1;
	defaultHeater1->name = "Heater 1";
	defaultHeater1->pinNr = (gpio_num_t)CONFIG_HEAT1;
	defaultHeater1->preference = 1;
	defaultHeater1->watt = 1500;
	defaultHeater1->useForMash = true;
	defaultHeater1->useForBoil = true;

	this->heaters.push_back(defaultHeater1);

	auto defaultHeater2 = new Heater();
	defaultHeater2->id = 2;
	defaultHeater2->name = "Heater 2";
	defaultHeater2->pinNr = (gpio_num_t)CONFIG_HEAT2;
	defaultHeater2->preference = 2;
	defaultHeater2->watt = 1500;
	defaultHeater2->useForMash = true;
	defaultHeater2->useForBoil = true;
	this->heaters.push_back(defaultHeater2);
}

void BrewEngine::readHeaterSettings()
{
	vector<uint8_t> empty = json::to_msgpack(json::array({}));
	vector<uint8_t> serialized = this->settingsManager->Read("heaters", empty);

	json jHeaters = json::from_msgpack(serialized);

	if (jHeaters.empty())
	{
		ESP_LOGI(TAG, "Adding Default Heaters");
		this->addDefaultHeaters();
	}
	else
	{

		for (auto &el : jHeaters.items())
		{
			auto jHeater = el.value();

			auto heater = new Heater();
			heater->from_json(jHeater);

			ESP_LOGI(TAG, "Heater From Settings ID:%d", heater->id);

			this->heaters.push_back(heater);
		}
	}

	// Sort on preference
	sort(this->heaters.begin(), this->heaters.end(), [](Heater *h1, Heater *h2)
		 { return (h1->preference < h2->preference); });
}

void BrewEngine::saveHeaterSettings(const json &jHeaters)
{
	ESP_LOGI(TAG, "Saving Heater Settings");

	if (!jHeaters.is_array())
	{
		ESP_LOGW(TAG, "Heater settings must be an array!");
		return;
	}

	// wait for stop
	vTaskDelay(pdMS_TO_TICKS(1000));

	// clear
	for (auto const &heater : this->heaters)
	{
		delete heater;
	}
	this->heaters.clear();

	uint8_t newId = 0;

	// update running data
	for (auto &el : jHeaters.items())
	{
		newId++;

		if (newId > 10)
		{
			ESP_LOGE(TAG, "Only 10 heaters supported!");
			continue;
		}

		auto jHeater = el.value();
		jHeater["id"] = newId;

		auto heater = new Heater();
		heater->from_json(jHeater);
		heater->id = newId;

		this->heaters.push_back(heater);
	}

	// Sort on preference
	sort(this->heaters.begin(), this->heaters.end(), [](Heater *h1, Heater *h2)
		 { return (h1->preference < h2->preference); });

	// Serialize to MessagePack for size
	vector<uint8_t> serialized = json::to_msgpack(jHeaters);

	this->settingsManager->Write("heaters", serialized);

	// re-init so they can be used
	this->initHeaters();

	ESP_LOGI(TAG, "Saving Heater Settings Done");
}

void BrewEngine::readTempSensorSettings()
{
	vector<uint8_t> empty = json::to_msgpack(json::array({}));
	vector<uint8_t> serialized = this->settingsManager->Read("tempsensors", empty);

	json jTempSensors = json::from_msgpack(serialized);

	for (auto &el : jTempSensors.items())
	{
		auto jSensor = el.value();

		auto sensor = new TemperatureSensor();
		sensor->from_json(jSensor);

		uint64_t sensorId = sensor->id;

		ESP_LOGI(TAG, "Sensor From Settings address: %016llX, ID:%llu", sensorId, sensorId);

		this->sensors.insert_or_assign(sensorId, sensor);
	}
}

void BrewEngine::saveTempSensorSettings(const json &jTempSensors)
{
	ESP_LOGI(TAG, "Saving Temp Sensor Settings");

	if (!jTempSensors.is_array())
	{
		ESP_LOGW(TAG, "Temp settings must be an array!");
		return;
	}

	// we need to temp stop our temp read loop while we change the sensor data
	this->skipTempLoop = true;
	vTaskDelay(pdMS_TO_TICKS(2000));

	// update running data
	for (auto &el : jTempSensors.items())
	{
		auto jSensor = el.value();
		string stringId = jSensor["id"].get<string>();
		uint64_t sensorId = std::stoull(stringId);

		std::map<uint64_t, TemperatureSensor *>::iterator it;
		it = this->sensors.find(sensorId);

		if (it == this->sensors.end())
		{
			// doesn't exist anymore, just ignore
			ESP_LOGI(TAG, "doesn't exist anymore, just ignore %llu", sensorId);
			continue;
		}
		else
		{
			ESP_LOGI(TAG, "Updating Sensor %llu", sensorId);
			// update it
			TemperatureSensor *sensor = it->second;
			sensor->name = jSensor["name"];
			sensor->color = jSensor["color"];

			if (!jSensor["useForControl"].is_null() && jSensor["useForControl"].is_boolean())
			{
				sensor->useForControl = jSensor["useForControl"];
			}

			if (!jSensor["show"].is_null() && jSensor["show"].is_boolean())
			{
				sensor->show = jSensor["show"];

				if (!sensor->show)
				{
					// when show is disabled we also remove it from current, so it doesn't showup anymore
					this->currentTemperatures.erase(sensorId);
				}
			}

			if (!jSensor["compensateAbsolute"].is_null() && jSensor["compensateAbsolute"].is_number())
			{
				sensor->compensateAbsolute = (float)jSensor["compensateAbsolute"];
			}

			if (!jSensor["compensateRelative"].is_null() && jSensor["compensateRelative"].is_number())
			{
				sensor->compensateRelative = (float)jSensor["compensateRelative"];
			}
		}
	}

	// We also need to delete sensors that are no longer in the list
	vector<uint64_t> sensorsToDelete;

	for (auto const &[key, sensor] : this->sensors)
	{
		uint64_t sensorId = sensor->id;
		string stringId = to_string(sensorId); // json doesn't support unit64 so in out json id is string
		auto foundSensor = std::find_if(jTempSensors.begin(), jTempSensors.end(), [&stringId](const json &x)
										{
											auto it = x.find("id");
											return it != x.end() and it.value() == stringId; });

		// remove it
		if (foundSensor == jTempSensors.end())
		{
			ESP_LOGI(TAG, "Erasing Sensor %llu", sensorId);
			sensorsToDelete.push_back(sensorId);
			this->currentTemperatures.erase(sensorId);
		}
	}

	// erase in second loop, we can't mutate wile in auto loop (c++ limitation atm)
	for (auto &sensorId : sensorsToDelete)
	{
		// Clean up RTD hardware handles if this is an RTD sensor
		auto sensorIt = this->sensors.find(sensorId);
		if (sensorIt != this->sensors.end())
		{
			TemperatureSensor *sensor = sensorIt->second;
			if (sensor->sensorType == SENSOR_PT100 || sensor->sensorType == SENSOR_PT1000)
			{
				// Find and remove the corresponding RTD hardware handle
				for (auto rtdIt = this->rtdSensors.begin(); rtdIt != this->rtdSensors.end(); ++rtdIt)
				{
					if (*rtdIt != nullptr && memcmp(&(sensor->max31865Handle), *rtdIt, sizeof(max31865_t)) == 0)
					{
						// Remove device from SPI bus
						if ((*rtdIt)->spi != nullptr)
						{
							spi_bus_remove_device((*rtdIt)->spi);
						}
						delete *rtdIt;
						this->rtdSensors.erase(rtdIt);
						this->rtdSensorCount--;
						break;
					}
				}
			}
		}
		this->sensors.erase(sensorId);
	}

	// // Convert sensors to json and save to nvram
	json jSensors = json::array({});

	for (auto const &[key, val] : this->sensors)
	{
		json jSensor = val->to_json();
		jSensors.push_back(jSensor);
	}

	// Serialize to MessagePack for size
	vector<uint8_t> serialized = json::to_msgpack(jSensors);

	this->settingsManager->Write("tempsensors", serialized);

	// continue our temp loop
	this->skipTempLoop = false;

	ESP_LOGI(TAG, "Saving Temp Sensor Settings Done");
}

void BrewEngine::initMqtt()
{
	// return if no broker is configured
	if (this->mqttUri.find("mqtt://") == std::string::npos)
	{
		return;
	}

	ESP_LOGI(TAG, "initMqtt: Start");

	esp_mqtt_client_config_t mqtt5_cfg = {};
	mqtt5_cfg.broker.address.uri = this->mqttUri.c_str();
	mqtt5_cfg.session.protocol_ver = MQTT_PROTOCOL_V_5;
	mqtt5_cfg.network.disable_auto_reconnect = false;

	this->mqttClient = esp_mqtt_client_init(&mqtt5_cfg);
	// atm we don't need an event
	// esp_mqtt_client_register_event(this->mqttClient, MQTT_EVENT_ANY, this->mqtt_event_handler, this);
	esp_err_t err = esp_mqtt_client_start(this->mqttClient);

	if (err != ESP_OK)
	{
		ESP_LOGW(TAG, "Error Creating MQTT Client");
		return;
	}

	// string iso_datetime = this->to_iso_8601(std::chrono::system_clock::now());
	// string iso_date = iso_datetime.substr(0, 10);

	// we create a topic and just post all out data to runningLog, more complex configuration can follow in the future
	this->mqttTopic = "esp-brew-engine/" + this->Hostname + "/history";
	this->mqttTopicLog = "esp-brew-engine/" + this->Hostname + "/log";
	this->mqttEnabled = true;

	ESP_LOGI(TAG, "initMqtt: Done");
}

void BrewEngine::initInfluxDB()
{
	// return if no URL is configured
	if (this->influxdbUrl.empty() || this->influxdbToken.empty() || this->influxdbOrg.empty() || this->influxdbBucket.empty())
	{
		ESP_LOGI(TAG, "InfluxDB not configured, skipping initialization");
		return;
	}

	ESP_LOGI(TAG, "initInfluxDB: Start");
	
	// Generate a unique session ID for this boot
	this->currentSessionId = esp_random();
	
	this->influxdbEnabled = true;
	
	// Initialize last send time to allow immediate first send
	this->lastInfluxDBSend = system_clock::now() - seconds(this->influxdbSendInterval);
	
	ESP_LOGI(TAG, "initInfluxDB: Done - Session ID: %lu", this->currentSessionId);
}

string BrewEngine::escapeInfluxDBTagValue(const string &value)
{
	// Pre-allocate with extra space for escaping
	string escaped;
	escaped.reserve(value.length() + 20);
	
	for (char c : value) {
		if (c == ' ' || c == ',' || c == '=') {
			escaped += '\\';
		}
		escaped += c;
	}
	return escaped;
}

void BrewEngine::sendToInfluxDB(const string &measurement, const string &fields, const string &tags)
{
	if (!this->influxdbEnabled)
	{
		return;
	}

	// Construct InfluxDB line protocol
	string lineProtocol = measurement;
	
	// Add tags if provided
	if (!tags.empty())
	{
		lineProtocol += "," + tags;
	}
	
	// Add default tags
	lineProtocol += ",host=" + this->Hostname;
	lineProtocol += ",session_id=" + to_string(this->currentSessionId);
	
	// Add fields and timestamp
	lineProtocol += " " + fields;
	lineProtocol += " " + to_string(duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count());
	
	ESP_LOGD(TAG, "InfluxDB line: %s", lineProtocol.c_str());
	
	// Configure HTTP client with static buffers
	static char urlBuffer[512];
	static char authBuffer[256];
	
	snprintf(urlBuffer, sizeof(urlBuffer), "%s/api/v2/write?org=%s&bucket=%s", 
		this->influxdbUrl.c_str(), this->influxdbOrg.c_str(), this->influxdbBucket.c_str());
	snprintf(authBuffer, sizeof(authBuffer), "Bearer %s", this->influxdbToken.c_str());
	
	ESP_LOGI(TAG, "InfluxDB URL: %s", urlBuffer);
	ESP_LOGI(TAG, "InfluxDB Token length: %d", this->influxdbToken.length());
	
	esp_http_client_config_t config = {};
	config.url = urlBuffer;
	config.method = HTTP_METHOD_POST;
	config.timeout_ms = 5000;
	config.disable_auto_redirect = true;
	
	esp_http_client_handle_t client = esp_http_client_init(&config);
	if (client == NULL) {
		ESP_LOGE(TAG, "Failed to initialize HTTP client for InfluxDB");
		return;
	}
	
	// Set headers - try setting both ways
	esp_http_client_set_header(client, "Content-Type", "text/plain");
	esp_http_client_set_header(client, "Accept", "*/*");
	
	// Set authorization header after client init
	esp_err_t header_err = esp_http_client_set_header(client, "authorization", authBuffer);
	ESP_LOGI(TAG, "Set authorization header (lowercase) result: %s", esp_err_to_name(header_err));
	
	// Also try uppercase
	header_err = esp_http_client_set_header(client, "Authorization", authBuffer);
	ESP_LOGI(TAG, "Set Authorization header (uppercase) result: %s", esp_err_to_name(header_err));
	
	// Set POST data
	ESP_LOGI(TAG, "InfluxDB data: %s", lineProtocol.c_str());
	esp_http_client_set_post_field(client, lineProtocol.c_str(), lineProtocol.length());
	
	// Perform request
	esp_err_t err = esp_http_client_perform(client);
	
	if (err == ESP_OK)
	{
		int status_code = esp_http_client_get_status_code(client);
		if (status_code == 204)
		{
			ESP_LOGI(TAG, "InfluxDB write successful");
		}
		else
		{
			// Get response body for error details
			int content_length = esp_http_client_get_content_length(client);
			if (content_length > 0 && content_length < 1024) {
				char response_buffer[1024];
				int data_read = esp_http_client_read_response(client, response_buffer, sizeof(response_buffer) - 1);
				if (data_read > 0) {
					response_buffer[data_read] = '\0';
					ESP_LOGW(TAG, "InfluxDB error response: %s", response_buffer);
				}
			}
			ESP_LOGW(TAG, "InfluxDB write failed with status: %d", status_code);
		}
	}
	else
	{
		ESP_LOGE(TAG, "InfluxDB HTTP request failed: %s", esp_err_to_name(err));
	}
	
	esp_http_client_cleanup(client);
}

string BrewEngine::queryInfluxDB(const string &query)
{
	if (!this->influxdbEnabled)
	{
		return "";
	}

	ESP_LOGD(TAG, "InfluxDB query: %s", query.c_str());
	
	// URL encode the query
	string encodedQuery = query;
	// TODO: Implement proper URL encoding if needed
	
	// Configure HTTP client for query
	string queryUrl = this->influxdbUrl + "/api/v2/query?org=" + this->influxdbOrg;
	esp_http_client_config_t config = {};
	config.url = queryUrl.c_str();
	config.method = HTTP_METHOD_POST;
	config.timeout_ms = 10000;
	config.disable_auto_redirect = true;
	
	esp_http_client_handle_t client = esp_http_client_init(&config);
	
	// Set headers
	esp_http_client_set_header(client, "Authorization", ("Bearer " + this->influxdbToken).c_str());
	esp_http_client_set_header(client, "Content-Type", "application/vnd.flux");
	esp_http_client_set_header(client, "Accept", "application/json");
	
	// Set POST data (Flux query)
	string fluxQuery = "from(bucket:\"" + this->influxdbBucket + "\") " + query;
	esp_http_client_set_post_field(client, fluxQuery.c_str(), fluxQuery.length());
	
	// Perform request
	esp_err_t err = esp_http_client_perform(client);
	
	string response = "";
	if (err == ESP_OK)
	{
		int status_code = esp_http_client_get_status_code(client);
		int content_length = esp_http_client_get_content_length(client);
		
		if (status_code == 200 && content_length > 0)
		{
			char *buffer = (char *)malloc(content_length + 1);
			if (buffer)
			{
				int read_length = esp_http_client_read(client, buffer, content_length);
				if (read_length > 0)
				{
					buffer[read_length] = '\0';
					response = string(buffer);
				}
				free(buffer);
			}
		}
		else
		{
			ESP_LOGW(TAG, "InfluxDB query failed with status: %d", status_code);
		}
	}
	else
	{
		ESP_LOGE(TAG, "InfluxDB query HTTP request failed: %s", esp_err_to_name(err));
	}
	
	esp_http_client_cleanup(client);
	return response;
}

json BrewEngine::getInfluxDBStatistics(const json &requestData)
{
	if (!this->influxdbEnabled)
	{
		// Fallback to local statistics if InfluxDB is not enabled
		json result;
		
		// Get sessions
		vector<BrewSession> sessions = this->statisticsManager->GetSessionList();
		json jSessions = json::array();
		for (const auto& session : sessions) {
			json jSession;
			jSession["sessionId"] = session.sessionId;
			jSession["scheduleName"] = session.scheduleName;
			jSession["startTime"] = session.startTime;
			jSession["endTime"] = session.endTime;
			jSession["duration"] = session.totalDuration;
			jSession["dataPoints"] = session.dataPoints;
			jSession["avgTemperature"] = session.avgTemperature;
			jSession["minTemperature"] = session.minTemperature;
			jSession["maxTemperature"] = session.maxTemperature;
			jSession["completed"] = session.completed;
			jSessions.push_back(jSession);
		}
		
		// Get stats
		map<string, uint32_t> stats = this->statisticsManager->GetSessionStats();
		
		// Get config
		json config = {
			{"maxSessions", this->statisticsManager->GetMaxSessions()},
			{"currentSessionActive", this->statisticsManager->IsSessionActive()},
			{"currentSessionId", this->statisticsManager->GetCurrentSessionId()},
			{"currentDataPoints", this->statisticsManager->GetCurrentSessionDataPoints()}
		};
		
		result["sessions"] = jSessions;
		result["stats"] = stats;
		result["config"] = config;
		
		return result;
	}

	json result;
	
	// Get session list with basic statistics
	string sessionQuery = R"(
		|> range(start: -30d)
		|> filter(fn: (r) => r["_measurement"] == "session_metadata")
		|> filter(fn: (r) => r["host"] == ")" + this->Hostname + R"(")
		|> pivot(rowKey:["_time"], columnKey: ["_field"], valueColumn: "_value")
		|> sort(columns: ["_time"], desc: true)
		|> limit(n: 50)
	)";
	
	string sessionResponse = this->queryInfluxDB(sessionQuery);
	
	// Get temperature statistics for sessions
	string tempQuery = R"(
		|> range(start: -30d)
		|> filter(fn: (r) => r["_measurement"] == "temperature")
		|> filter(fn: (r) => r["host"] == ")" + this->Hostname + R"(")
		|> group(columns: ["session_id"])
		|> aggregateWindow(every: 1h, fn: mean, createEmpty: false)
		|> yield(name: "session_temps")
	)";
	
	string tempResponse = this->queryInfluxDB(tempQuery);
	
	// Process the responses and build the result
	json sessions = json::array();
	json stats = {
		{"totalSessions", 0},
		{"totalBrewTime", 0},
		{"avgSessionDuration", 0}
	};
	
	json config = {
		{"maxSessions", 100},
		{"currentSessionActive", this->controlRun},
		{"currentSessionId", to_string(this->currentSessionId)},
		{"currentDataPoints", 0},
		{"retentionPolicy", "30d"}
	};
	
	// TODO: Parse InfluxDB response and populate sessions array
	// For now, return basic structure
	
	result["sessions"] = sessions;
	result["stats"] = stats;
	result["config"] = config;
	
	return result;
}

json BrewEngine::getInfluxDBSessionData(const json &requestData)
{
	if (!this->influxdbEnabled)
	{
		// Fallback to local statistics if InfluxDB is not enabled
		uint32_t sessionId = requestData["sessionId"];
		BrewSession session = this->statisticsManager->GetSessionById(sessionId);
		
		if (session.sessionId == 0) {
			// Session not found
			json result;
			result["error"] = "Session not found";
			return result;
		}
		
		vector<TempDataPoint> sessionData = this->statisticsManager->GetSessionData(sessionId);
		
		json result;
		result["sessionId"] = session.sessionId;
		result["scheduleName"] = session.scheduleName;
		result["startTime"] = session.startTime;
		result["endTime"] = session.endTime;
		result["duration"] = session.totalDuration;
		result["avgTemperature"] = session.avgTemperature;
		result["minTemperature"] = session.minTemperature;
		result["maxTemperature"] = session.maxTemperature;
		result["completed"] = session.completed;
		
		json jData = json::array();
		for (const auto& point : sessionData) {
			json jPoint;
			jPoint["timestamp"] = point.timestamp;
			jPoint["avgTemp"] = (int)point.avgTemp;
			jPoint["targetTemp"] = (int)point.targetTemp;
			jPoint["pidOutput"] = (int)point.pidOutput;
			jData.push_back(jPoint);
		}
		
		result["data"] = jData;
		return result;
	}

	json result;
	
	string sessionId = requestData["sessionId"];
	string resolution = requestData.contains("resolution") ? requestData["resolution"] : "1m";
	
	// Get session metadata
	string metadataQuery = R"(
		|> range(start: -30d)
		|> filter(fn: (r) => r["_measurement"] == "session_metadata")
		|> filter(fn: (r) => r["session_id"] == ")" + sessionId + R"(")
		|> filter(fn: (r) => r["host"] == ")" + this->Hostname + R"(")
		|> pivot(rowKey:["_time"], columnKey: ["_field"], valueColumn: "_value")
		|> last()
	)";
	
	string metadataResponse = this->queryInfluxDB(metadataQuery);
	
	// Get temperature data with specified resolution
	string tempQuery = R"(
		|> range(start: -30d)
		|> filter(fn: (r) => r["_measurement"] == "temperature")
		|> filter(fn: (r) => r["session_id"] == ")" + sessionId + R"(")
		|> filter(fn: (r) => r["host"] == ")" + this->Hostname + R"(")
		|> aggregateWindow(every: )" + resolution + R"(, fn: mean, createEmpty: false)
		|> pivot(rowKey:["_time"], columnKey: ["_field"], valueColumn: "_value")
		|> sort(columns: ["_time"])
	)";
	
	string tempResponse = this->queryInfluxDB(tempQuery);
	
	// Process the responses and build the result
	result["sessionId"] = sessionId;
	result["data"] = json::array();
	
	// TODO: Parse InfluxDB response and populate data array
	// For now, return basic structure
	
	return result;
}

void BrewEngine::initOneWire()
{
	ESP_LOGI(TAG, "initOneWire: Start");

	onewire_bus_config_t bus_config;
	bus_config.bus_gpio_num = this->oneWire_PIN;

	onewire_bus_rmt_config_t rmt_config;
	rmt_config.max_rx_bytes = 10; // 1byte ROM command + 8byte ROM number + 1byte device command

	ESP_ERROR_CHECK(onewire_new_bus_rmt(&bus_config, &rmt_config, &this->obh));
	ESP_LOGI(TAG, "1-Wire bus installed on GPIO%d", this->oneWire_PIN);

	ESP_LOGI(TAG, "initOneWire: Done");
}

void BrewEngine::detectOnewireTemperatureSensors()
{

	// we need to temp stop our temp read loop while we change the sensor data
	this->skipTempLoop = true;
	vTaskDelay(pdMS_TO_TICKS(2000));

	// sensors are already loaded via json settings, but we need to add handles and status
	onewire_device_iter_handle_t iter = NULL;
	esp_err_t search_result = ESP_OK;

	// create 1-wire device iterator, which is used for device search
	ESP_ERROR_CHECK(onewire_new_device_iter(this->obh, &iter));
	ESP_LOGI(TAG, "Device iterator created, start searching...");

	int i = 0;
	do
	{

		onewire_device_t next_onewire_device = {};

		search_result = onewire_device_iter_get_next(iter, &next_onewire_device);
		if (search_result == ESP_OK)
		{ // found a new device, let's check if we can upgrade it to a DS18B20
			ds18b20_config_t ds_cfg = {};

			ds18b20_device_handle_t newHandle;

			if (ds18b20_new_device(&next_onewire_device, &ds_cfg, &newHandle) == ESP_OK)
			{
				uint64_t sensorId = next_onewire_device.address;

				ESP_LOGI(TAG, "Found a DS18B20[%d], address: %016llX ID:%llu", i, sensorId, sensorId);
				i++;

				if (this->sensors.size() >= ONEWIRE_MAX_DS18B20)
				{
					ESP_LOGI(TAG, "Max DS18B20 number reached, stop searching...");
					break;
				}

				std::map<uint64_t, TemperatureSensor *>::iterator it;
				it = this->sensors.find(sensorId);

				if (it == this->sensors.end())
				{
					ESP_LOGI(TAG, "New Sensor");

					// doesn't exist yet, we need to add it
					auto sensor = new TemperatureSensor();
					sensor->id = sensorId;
					sensor->name = to_string(sensorId);
					sensor->color = "#ffffff";
					sensor->useForControl = true;
					sensor->show = true;
					sensor->connected = true;
					sensor->compensateAbsolute = 0;
					sensor->compensateRelative = 1;
					sensor->sensorType = SENSOR_DS18B20;
					sensor->ds18b20Handle = newHandle;
					this->sensors.insert_or_assign(sensor->id, sensor);
				}
				else
				{
					ESP_LOGI(TAG, "Existing Sensor");
					// just set connected and handle
					TemperatureSensor *sensor = it->second;
					sensor->ds18b20Handle = newHandle;
					sensor->connected = true;
				}

				// set resolution for all DS18B20s
				ds18b20_set_resolution(newHandle, DS18B20_RESOLUTION_12B);
			}
			else
			{
				ESP_LOGI(TAG, "Found an unknown device, address: %016llX", next_onewire_device.address);
			}
		}
	} while (search_result != ESP_ERR_NOT_FOUND);

	ESP_ERROR_CHECK(onewire_del_device_iter(iter));
	ESP_LOGI(TAG, "Searching done, %d DS18B20 device(s) found", this->sensors.size());

	this->skipTempLoop = false;
}

void BrewEngine::initRtdSensors()
{
	ESP_LOGI(TAG, "initRtdSensors: Start");

	if (!this->rtdSensorsEnabled)
	{
		ESP_LOGI(TAG, "RTD sensors disabled in configuration");
		return;
	}

	// Validate SPI pin configuration
	if (this->spi_mosi_pin == GPIO_NUM_NC || this->spi_mosi_pin >= GPIO_NUM_MAX ||
		this->spi_miso_pin == GPIO_NUM_NC || this->spi_miso_pin >= GPIO_NUM_MAX ||
		this->spi_clk_pin == GPIO_NUM_NC || this->spi_clk_pin >= GPIO_NUM_MAX)
	{
		ESP_LOGE(TAG, "Invalid SPI pin configuration for RTD sensors (MOSI:%d, MISO:%d, CLK:%d)", 
				 this->spi_mosi_pin, this->spi_miso_pin, this->spi_clk_pin);
		return;
	}

	// Initialize SPI bus for RTD sensors (only once for all MAX31865 devices)
	esp_err_t ret = max31865_init_bus(SPI2_HOST, (int)this->spi_mosi_pin, (int)this->spi_miso_pin, (int)this->spi_clk_pin);
	if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
	{
		ESP_LOGE(TAG, "Failed to initialize SPI bus for RTD sensors: %s", esp_err_to_name(ret));
		return;
	}

	ESP_LOGI(TAG, "RTD SPI bus initialized successfully");
	ESP_LOGI(TAG, "initRtdSensors: Done");
}

void BrewEngine::detectRtdTemperatureSensors()
{
	ESP_LOGI(TAG, "detectRtdTemperatureSensors: Start");

	if (!this->rtdSensorsEnabled)
	{
		ESP_LOGI(TAG, "RTD sensors disabled, skipping detection");
		return;
	}

	// Validate SPI pin configuration before attempting to use RTD sensors
	if (this->spi_mosi_pin == GPIO_NUM_NC || this->spi_mosi_pin >= GPIO_NUM_MAX ||
		this->spi_miso_pin == GPIO_NUM_NC || this->spi_miso_pin >= GPIO_NUM_MAX ||
		this->spi_clk_pin == GPIO_NUM_NC || this->spi_clk_pin >= GPIO_NUM_MAX)
	{
		ESP_LOGE(TAG, "Invalid SPI pin configuration for RTD sensors (MOSI:%d, MISO:%d, CLK:%d)", 
				 this->spi_mosi_pin, this->spi_miso_pin, this->spi_clk_pin);
		
		// Mark all RTD sensors as disconnected
		for (auto &[sensorId, sensor] : this->sensors)
		{
			if (sensor->sensorType == SENSOR_PT100 || sensor->sensorType == SENSOR_PT1000)
			{
				sensor->connected = false;
			}
		}
		return;
	}

	// we need to temp stop our temp read loop while we change the sensor data
	this->skipTempLoop = true;
	vTaskDelay(pdMS_TO_TICKS(2000));

	// Clean up any existing RTD hardware handles before re-initializing
	this->cleanupRtdSensors();

	// Re-initialize SPI bus to ensure it's properly configured
	esp_err_t bus_ret = max31865_init_bus(SPI2_HOST, (int)this->spi_mosi_pin, (int)this->spi_miso_pin, (int)this->spi_clk_pin);
	if (bus_ret != ESP_OK && bus_ret != ESP_ERR_INVALID_STATE)
	{
		ESP_LOGE(TAG, "Failed to initialize SPI bus for RTD sensors: %s", esp_err_to_name(bus_ret));
		
		// Mark all RTD sensors as disconnected
		for (auto &[sensorId, sensor] : this->sensors)
		{
			if (sensor->sensorType == SENSOR_PT100 || sensor->sensorType == SENSOR_PT1000)
			{
				sensor->connected = false;
			}
		}
		
		this->skipTempLoop = false;
		return;
	}

	// Initialize hardware handles for all RTD sensors loaded from NVS
	int rtdSensorsInitialized = 0;
	
	for (auto &[sensorId, sensor] : this->sensors)
	{
		// Skip non-RTD sensors
		if (sensor->sensorType != SENSOR_PT100 && sensor->sensorType != SENSOR_PT1000)
		{
			continue;
		}

		ESP_LOGI(TAG, "Initializing RTD sensor: %s (ID: %llu)", sensor->name.c_str(), sensorId);
		
		// Extract CS pin from sensor ID (format: 0x31865000 + CS pin)
		int csPin = (int)(sensorId - 0x31865000);
		ESP_LOGI(TAG, "Extracted CS pin %d from sensor ID %llu (0x%llx)", csPin, sensorId, sensorId);
		
		// Validate CS pin
		if (csPin < 0 || csPin >= GPIO_NUM_MAX)
		{
			ESP_LOGE(TAG, "Invalid CS pin %d for RTD sensor %s", csPin, sensor->name.c_str());
			sensor->connected = false;
			continue;
		}

		// Initialize MAX31865 device (SPI bus should already be initialized)
		max31865_t *rtd_sensor = new max31865_t;
		esp_err_t ret = max31865_init_desc(rtd_sensor, SPI2_HOST, csPin);
		
		if (ret == ESP_OK)
		{
			// Configure MAX31865 based on sensor type
			ret = max31865_set_config(rtd_sensor, true, 1, false, false, 0, true, true, 0, 0xFFFF);
			
			if (ret == ESP_OK)
			{
				// Set RTD parameters based on sensor type
				if (sensor->sensorType == SENSOR_PT100)
				{
					rtd_sensor->rtd_nominal = 100;   // PT100
					rtd_sensor->ref_resistor = 430;  // Reference resistor for PT100
				}
				else if (sensor->sensorType == SENSOR_PT1000)
				{
					rtd_sensor->rtd_nominal = 1000;  // PT1000
					rtd_sensor->ref_resistor = 4300; // Reference resistor for PT1000
				}
				
				// Store hardware handle in sensor object
				sensor->max31865Handle = *rtd_sensor;
				sensor->consecutiveFailures = 0; // Initialize failure counter
				sensor->connected = true;
				
				// Add to RTD sensor list
				this->rtdSensors.push_back(rtd_sensor);
				rtdSensorsInitialized++;
				
				ESP_LOGI(TAG, "RTD sensor %s initialized successfully on CS pin %d", sensor->name.c_str(), csPin);
			}
			else
			{
				ESP_LOGE(TAG, "Failed to configure RTD sensor %s: %s", sensor->name.c_str(), esp_err_to_name(ret));
				delete rtd_sensor;
				sensor->connected = false;
			}
		}
		else
		{
			ESP_LOGE(TAG, "Failed to initialize RTD sensor %s on CS pin %d: %s", sensor->name.c_str(), csPin, esp_err_to_name(ret));
			delete rtd_sensor;
			sensor->connected = false;
		}
	}

	this->rtdSensorCount = rtdSensorsInitialized;
	ESP_LOGI(TAG, "RTD detection done, %d RTD sensor(s) initialized", this->rtdSensorCount);
	this->skipTempLoop = false;
}

void BrewEngine::cleanupRtdSensors()
{
	ESP_LOGI(TAG, "Cleaning up RTD sensors");
	
	// Clean up MAX31865 hardware handles
	for (auto rtd_sensor : this->rtdSensors)
	{
		if (rtd_sensor != nullptr)
		{
			// Remove device from SPI bus
			if (rtd_sensor->spi != nullptr)
			{
				spi_bus_remove_device(rtd_sensor->spi);
			}
			delete rtd_sensor;
		}
	}
	
	// Clear the RTD sensor list
	this->rtdSensors.clear();
	this->rtdSensorCount = 0;
	
	ESP_LOGI(TAG, "RTD sensor cleanup completed");
}

bool BrewEngine::reinitializeRtdSensor(TemperatureSensor *sensor)
{
	if (sensor->sensorType != SENSOR_PT100 && sensor->sensorType != SENSOR_PT1000)
	{
		return false;
	}

	// Extract CS pin from sensor ID
	int csPin = (int)(sensor->id - 0x31865000);
	
	// Validate CS pin
	if (csPin < 0 || csPin >= GPIO_NUM_MAX)
	{
		ESP_LOGE(TAG, "Invalid CS pin %d for RTD sensor %s", csPin, sensor->name.c_str());
		return false;
	}

	ESP_LOGI(TAG, "Reinitializing RTD sensor %s on CS pin %d", sensor->name.c_str(), csPin);

	// Clean up the old hardware handle if it exists
	if (sensor->max31865Handle.spi != nullptr)
	{
		// Find and remove the old RTD sensor from the list
		for (auto it = this->rtdSensors.begin(); it != this->rtdSensors.end(); ++it)
		{
			if ((*it)->spi == sensor->max31865Handle.spi)
			{
				// Remove device from SPI bus
				spi_bus_remove_device((*it)->spi);
				delete *it;
				this->rtdSensors.erase(it);
				this->rtdSensorCount--;
				break;
			}
		}
	}

	// Try to re-initialize the MAX31865 device
	max31865_t *rtd_sensor = new max31865_t;
	esp_err_t ret = max31865_init_desc(rtd_sensor, SPI2_HOST, csPin);
	
	if (ret == ESP_OK)
	{
		// Configure MAX31865 based on sensor type
		ret = max31865_set_config(rtd_sensor, true, 1, false, false, 0, true, true, 0, 0xFFFF);
		
		if (ret == ESP_OK)
		{
			// Set RTD parameters based on sensor type
			if (sensor->sensorType == SENSOR_PT100)
			{
				rtd_sensor->rtd_nominal = 100;   // PT100
				rtd_sensor->ref_resistor = 430;  // Reference resistor for PT100
			}
			else if (sensor->sensorType == SENSOR_PT1000)
			{
				rtd_sensor->rtd_nominal = 1000;  // PT1000
				rtd_sensor->ref_resistor = 4300; // Reference resistor for PT1000
			}
			
			// Update sensor with new hardware handle
			sensor->max31865Handle = *rtd_sensor;
			sensor->consecutiveFailures = 0; // Reset failure counter
			sensor->connected = false; // Will be set to true on successful read
			
			// Add to RTD sensor list
			this->rtdSensors.push_back(rtd_sensor);
			this->rtdSensorCount++;
			
			ESP_LOGI(TAG, "RTD sensor %s re-initialized successfully on CS pin %d", sensor->name.c_str(), csPin);
			return true;
		}
		else
		{
			ESP_LOGE(TAG, "Failed to configure re-initialized RTD sensor %s: %s", sensor->name.c_str(), esp_err_to_name(ret));
			delete rtd_sensor;
		}
	}
	else
	{
		ESP_LOGE(TAG, "Failed to re-initialize RTD sensor %s on CS pin %d: %s", sensor->name.c_str(), csPin, esp_err_to_name(ret));
		delete rtd_sensor;
	}
	
	return false;
}

void BrewEngine::start()
{
	// don't start if we are already running
	if (!this->controlRun)
	{
		this->controlRun = true;
		this->inOverTime = false;
		this->boostStatus = Off;
		this->overrideTargetTemperature = std::nullopt;
		// clear old temp log
		this->tempLog.clear();
		
		// sensorTempLogs removed - will use database instead

		// also clear old steps
		for (auto const &step : this->executionSteps)
		{
			delete step.second;
		}
		this->executionSteps.clear();

		if (this->selectedMashScheduleName.empty() == false)
		{
			this->loadSchedule();
			this->currentMashStep = 1; // 0 is current temp, so we can start at 1
			xTaskCreate(&this->controlLoop, "controlloop_task", 4096, this, 5, NULL);
		}
		else
		{

			// if no schedule is selected, we set the boil flag based on temperature
			if ((this->temperatureScale == Celsius && this->targetTemperature >= 100) || (this->temperatureScale == Fahrenheit && this->targetTemperature >= 212))
			{
				this->boilRun = true;
			}
			else
			{
				this->boilRun = false;
			}
		}

		xTaskCreate(&this->pidLoop, "pidloop_task", 8192, this, 5, NULL);

		xTaskCreate(&this->outputLoop, "outputloop_task", 4096, this, 5, NULL);

		this->statusText = "Running";
	}
}

void BrewEngine::loadSchedule()
{
	auto pos = this->mashSchedules.find(this->selectedMashScheduleName);

	if (pos == this->mashSchedules.end())
	{
		ESP_LOGE(TAG, "Program with name: %s not found!", this->selectedMashScheduleName.c_str());
		return;
	}
	auto schedule = pos->second;

	system_clock::time_point prevTime = std::chrono::system_clock::now();

	for (auto const &step : this->executionSteps)
	{
		delete step.second;
	}
	this->executionSteps.clear();

	this->currentExecutionStep = 0;
	this->boilRun = schedule->boil;
	int stepIndex = 0;

	float prevTemp = this->temperature;
	// insert the current as starting point
	auto execStep0 = new ExecutionStep();
	execStep0->time = prevTime;
	execStep0->temperature = prevTemp;
	execStep0->extendIfNeeded = false;
	this->executionSteps.insert(std::make_pair(stepIndex, execStep0));

	string iso_string = this->to_iso_8601(prevTime);
	ESP_LOGI(TAG, "Time:%s, Temp:%f Extend:%d", iso_string.c_str(), prevTemp, execStep0->extendIfNeeded);

	int extendNotifications = 0;

	stepIndex++;

	for (auto const &step : schedule->steps)
	{
		// a step can actualy be 2 different executions, 1 step time that needs substeps calcualted, and one fixed

		if (step->stepTime > 0 || step->extendStepTimeIfNeeded)
		{

			int stepTime = step->stepTime;

			// when the users request step extended, we need a step so 0 isn't valid we default to 1 min
			if (stepTime == 0)
			{
				stepTime = 1;
				extendNotifications += 60;
			}

			auto stepEndTime = prevTime + minutes(stepTime);

			int subStepsInStep;

			// When boost mode is active we don't want substeps this only complicates things
			if (step->allowBoost && this->boostModeUntil > 0)
			{
				subStepsInStep = 1;
			}
			else
			{
				auto secondsInStep = chrono::duration_cast<chrono::seconds>(stepEndTime - prevTime).count();
				subStepsInStep = (secondsInStep / this->stepInterval) - 1;

				// we need atleast one step
				if (subStepsInStep < 1)
				{
					subStepsInStep = 1;
				}
			}

			float tempDiffPerStep = (step->temperature - prevTemp) / (float)subStepsInStep;

			float prevStepTemp = 0;

			for (int j = 0; j < subStepsInStep; j++)
			{
				system_clock::time_point executionStepTime = prevTime;
				executionStepTime += seconds((j + 1) * stepInterval);

				float subStepTemp = prevTemp + (tempDiffPerStep * ((float)j + 1));

				// insert the current as starting point
				auto execStep = new ExecutionStep();
				execStep->time = executionStepTime;
				execStep->temperature = subStepTemp;
				execStep->extendIfNeeded = false;

				if (step->allowBoost && this->boostModeUntil > 0)
				{
					execStep->allowBoost = true;
				}
				else
				{
					execStep->allowBoost = false;
				}

				// set extend if needed on last step if configured
				if (j == (subStepsInStep - 1) && step->extendStepTimeIfNeeded)
				{
					execStep->extendIfNeeded = true;
				}

				float diff = abs(subStepTemp - prevStepTemp);
				// ESP_LOGI(TAG, "Diff:%f, subStepTemp:%f prevStepTemp:%f", diff, subStepTemp, prevStepTemp);

				// only insert if difference or if last step more then 1 degree
				if (diff > 1 || (j == subStepsInStep - 1))
				{
					this->executionSteps.insert(std::make_pair(stepIndex, execStep));
					prevStepTemp = execStep->temperature;
					stepIndex++;

					// Convert the time_point to an ISO 8601 string
					string iso_string = this->to_iso_8601(executionStepTime);

					ESP_LOGI(TAG, "Time:%s, Temp:%f Extend:%d", iso_string.c_str(), subStepTemp, execStep->extendIfNeeded);
				}
			}

			prevTime = stepEndTime;
			prevTemp = prevStepTemp;
		}
		else
		{
			// we start in 10 seconds
			auto stepEndTime = prevTime + seconds(10);

			// go directly to temp
			auto execStep = new ExecutionStep();
			execStep->time = stepEndTime;
			execStep->temperature = (float)step->temperature;
			execStep->extendIfNeeded = step->extendStepTimeIfNeeded;

			this->executionSteps.insert(std::make_pair(stepIndex, execStep));

			stepIndex++;

			// Convert the time_point to an ISO 8601 string
			string iso_string = this->to_iso_8601(prevTime);

			ESP_LOGI(TAG, "Time:%s, Temp:%f Extend:%d", iso_string.c_str(), (float)step->temperature, execStep->extendIfNeeded);

			prevTime = stepEndTime;
			prevTemp = (float)step->temperature;
		}

		// for the hold time we just need add one point
		auto holdEndTime = prevTime + minutes(step->time);

		auto holdStep = new ExecutionStep();
		holdStep->time = holdEndTime;
		holdStep->temperature = (float)step->temperature;
		holdStep->extendIfNeeded = false;

		this->executionSteps.insert(std::make_pair(stepIndex, holdStep));
		stepIndex++;

		prevTime = holdEndTime;
		prevTemp = step->temperature; // is normaly the same but this could change in futrure

		string iso_string2 = this->to_iso_8601(holdEndTime);
		ESP_LOGI(TAG, "Hold Time:%s, Temp:%f ", iso_string2.c_str(), (float)step->temperature);
	}

	// also add notifications
	for (auto const &notification : this->notifications)
	{
		delete notification;
	}
	this->notifications.clear();

	for (auto const &notification : schedule->notifications)
	{
		auto notificationTime = execStep0->time + minutes(notification->timeFromStart) + seconds(extendNotifications);

		// copy notification to new map
		auto newNotification = new Notification();
		newNotification->name = notification->name;
		newNotification->message = notification->message;
		newNotification->timeFromStart = notification->timeFromStart + (extendNotifications / 60); // in minutes
		newNotification->timePoint = notificationTime;

		this->notifications.push_back(newNotification);
	}

	// increate version so client can follow changes
	this->runningVersion++;
}

void BrewEngine::recalculateScheduleAfterOverTime()
{
	ESP_LOGI(TAG, "Recalculate Schedule after OverTime");

	int currentStepIndex = this->currentMashStep;

	auto currentPos = this->executionSteps.find(currentStepIndex);

	if (currentPos == this->executionSteps.end())
	{
		ESP_LOGE(TAG, "Steps not availible anymore");
		this->stop();
		return;
	}

	auto currentStep = currentPos->second;
	system_clock::time_point plannedEnd = currentStep->time;

	system_clock::time_point now = std::chrono::system_clock::now();
	auto extraSeconds = chrono::duration_cast<chrono::seconds>(now - plannedEnd).count();

	for (auto it = currentPos; it != this->executionSteps.end(); ++it)
	{
		auto step = it->second;
		auto newTime = step->time + seconds(extraSeconds);

		string iso_string = this->to_iso_8601(step->time);
		string iso_string2 = this->to_iso_8601(newTime);

		ESP_LOGI(TAG, "Time Changend From: %s, To:%s ", iso_string.c_str(), iso_string2.c_str());

		step->time = newTime;
	}

	// also increase notifications
	for (auto &notification : this->notifications)
	{
		auto newTime = notification->timePoint + seconds(extraSeconds);

		string iso_string = this->to_iso_8601(notification->timePoint);
		string iso_string2 = this->to_iso_8601(newTime);

		ESP_LOGI(TAG, "Notification Time Changend From: %s, To:%s ", iso_string.c_str(), iso_string2.c_str());

		notification->timePoint = newTime;
	}

	// increate version so client can follow changes
	this->runningVersion++;
}

void BrewEngine::stop()
{
	this->controlRun = false;
	this->boostStatus = Off;
	this->inOverTime = false;
	this->statusText = "Idle";
}

void BrewEngine::startStir(const json &stirConfig)
{
	if (!this->stir_PIN)
	{
		ESP_LOGW(TAG, "StirPin is not configured, ignoring startStir!");
		return;
	}

	system_clock::time_point now = std::chrono::system_clock::now();
	this->stirStartCycle = now;

	if (!stirConfig["max"].is_null() && stirConfig["max"].is_number())
	{
		this->stirTimeSpan = stirConfig["max"];
	}

	if (!stirConfig["intervalStart"].is_null() && stirConfig["intervalStart"].is_number())
	{
		this->stirIntervalStart = stirConfig["intervalStart"];
	}

	if (!stirConfig["intervalStop"].is_null() && stirConfig["intervalStop"].is_number())
	{
		this->stirIntervalStop = stirConfig["intervalStop"];
	}

	this->stirRun = true;

	xTaskCreate(&this->stirLoop, "stirloop_task", 4096, this, 10, &this->stirLoopHandle);

	this->stirStatusText = "Running";
}

void BrewEngine::stopStir()
{
	if (!this->stir_PIN)
	{
		ESP_LOGW(TAG, "StirPin is not configured, ignoring stopStir!");
		return;
	}

	this->stirRun = false;

	// stop at once
	gpio_set_level(this->stir_PIN, this->gpioLow);

	this->stirStatusText = "Idle";
}

void BrewEngine::stirLoop(void *arg)
{
	BrewEngine *instance = (BrewEngine *)arg;

	while (instance->run && instance->stirRun)
	{
		if (instance->stirIntervalStart == 0 && instance->stirIntervalStop == instance->stirTimeSpan)
		{
			// always on, just set high and wait for end
			gpio_set_level(instance->stir_PIN, instance->gpioHigh);
		}
		else
		{
			system_clock::time_point now = std::chrono::system_clock::now();

			auto startStirTime = instance->stirStartCycle + minutes(instance->stirIntervalStart);
			auto stopStirTime = instance->stirStartCycle + minutes(instance->stirIntervalStop);

			auto cycleEnd = instance->stirStartCycle + minutes(instance->stirTimeSpan);

			if (now >= startStirTime && now <= stopStirTime)
			{
				gpio_set_level(instance->stir_PIN, instance->gpioHigh);
			}
			else
			{
				gpio_set_level(instance->stir_PIN, instance->gpioLow);
			}

			// string iso_string1 = instance->to_iso_8601(now);
			// string iso_string2 = instance->to_iso_8601(startStirTime);
			// string iso_string3 = instance->to_iso_8601(stopStirTime);
			// string iso_string4 = instance->to_iso_8601(cycleEnd);

			// ESP_LOGI(TAG, "Stir Cycle Now: %s Start:%s Stop:%s CycleEnd: %s", iso_string1.c_str(), iso_string2.c_str(), iso_string3.c_str(), iso_string4.c_str());

			// start next cycle
			if (now >= cycleEnd)
			{
				instance->stirStartCycle = cycleEnd;
			}
		}

		vTaskDelay(pdMS_TO_TICKS(1000));
	}

	vTaskDelete(NULL);
}

void BrewEngine::readLoop(void *arg)
{
	BrewEngine *instance = (BrewEngine *)arg;

	int it = 0;

	while (instance->run)
	{
		vTaskDelay(pdMS_TO_TICKS(1000));

		// When we are changing temp settings we temporarily need to skip our temp loop
		if (instance->skipTempLoop)
		{
			continue;
		}

		int nrOfSensors = 0;
		float sum = 0.0;

		for (auto &[key, sensor] : instance->sensors)
		{
			float temperature;
			string stringId = std::to_string(key);

			// Skip disconnected sensors, except for RTD sensors which we want to retry
			if (!sensor->connected && sensor->sensorType != SENSOR_PT100 && sensor->sensorType != SENSOR_PT1000)
			{
				continue;
			}

			esp_err_t err = ESP_OK;

			// Read temperature based on sensor type
			if (sensor->sensorType == SENSOR_DS18B20)
			{
				// DS18B20 reading
				if (!sensor->ds18b20Handle)
				{
					continue;
				}

				err = ds18b20_trigger_temperature_conversion(sensor->ds18b20Handle);
				if (err != ESP_OK)
				{
					ESP_LOGW(TAG, "Error triggering conversion for DS18B20 [%s], disabling sensor!", stringId.c_str());
					sensor->connected = false;
					sensor->lastTemp = 0;
					instance->currentTemperatures.erase(key);
					continue;
				}

				err = ds18b20_get_temperature(sensor->ds18b20Handle, &temperature);
				if (err != ESP_OK)
				{
					ESP_LOGW(TAG, "Error reading temperature from DS18B20 [%s], disabling sensor!", stringId.c_str());
					sensor->connected = false;
					sensor->lastTemp = 0;
					instance->currentTemperatures.erase(key);
					continue;
				}
			}
			else if (sensor->sensorType == SENSOR_PT100 || sensor->sensorType == SENSOR_PT1000)
			{
				// RTD reading - always attempt to read, even if previously disconnected
				// But first check if the SPI handle is valid
				if (sensor->max31865Handle.spi == nullptr)
				{
					// Invalid SPI handle, mark as disconnected and skip
					sensor->connected = false;
					sensor->lastTemp = -999.0;
					sensor->consecutiveFailures++;
					if (sensor->show)
					{
						instance->currentTemperatures.insert_or_assign(key, -999.0);
					}
					
					// Try to reinitialize after 3 consecutive failures for invalid handles
					if (sensor->consecutiveFailures >= 3)
					{
						ESP_LOGI(TAG, "Attempting to reinitialize RTD sensor %s (invalid handle)", sensor->name.c_str());
						if (instance->reinitializeRtdSensor(sensor))
						{
							sensor->consecutiveFailures = 0;
							ESP_LOGI(TAG, "RTD sensor %s reinitialized successfully", sensor->name.c_str());
						}
						else
						{
							sensor->consecutiveFailures = 0; // Reset counter to avoid spam
						}
					}
					continue;
				}
				
				float rtd_resistance;
				err = max31865_measure(&sensor->max31865Handle, &rtd_resistance, &temperature);
				if (err != ESP_OK)
				{
					// Track consecutive failures for retry logic
					sensor->consecutiveFailures++;
					
					if (err == ESP_ERR_NOT_FOUND)
					{
						// Probe disconnected - show disconnected status but keep trying
						if (sensor->connected)
						{
							ESP_LOGW(TAG, "RTD probe [%s] disconnected", stringId.c_str());
						}
						sensor->lastTemp = -999.0; // Invalid temperature to show disconnected
						sensor->connected = false;  // Mark as disconnected
						if (sensor->show)
						{
							instance->currentTemperatures.insert_or_assign(key, -999.0);
						}
						
						// Try to reinitialize after 5 consecutive failures (5 seconds)
						if (sensor->consecutiveFailures >= 5)
						{
							ESP_LOGI(TAG, "Attempting to reinitialize RTD sensor %s after %d failures", sensor->name.c_str(), sensor->consecutiveFailures);
							if (instance->reinitializeRtdSensor(sensor))
							{
								sensor->consecutiveFailures = 0;
								ESP_LOGI(TAG, "RTD sensor %s reinitialized successfully", sensor->name.c_str());
							}
							else
							{
								sensor->consecutiveFailures = 0; // Reset counter to avoid spam
							}
						}
						
						continue; // Skip this sensor for control calculations
					}
					else
					{
						// Other errors - keep trying but mark as disconnected
						if (sensor->connected)
						{
							ESP_LOGW(TAG, "Error reading temperature from RTD [%s]: %s", stringId.c_str(), esp_err_to_name(err));
						}
						sensor->lastTemp = -999.0; // Invalid temperature
						sensor->connected = false;  // Mark as disconnected
						if (sensor->show)
						{
							instance->currentTemperatures.insert_or_assign(key, -999.0);
						}
						
						// Try to reinitialize after 5 consecutive failures for other errors
						if (sensor->consecutiveFailures >= 5)
						{
							ESP_LOGI(TAG, "Attempting to reinitialize RTD sensor %s after %d failures", sensor->name.c_str(), sensor->consecutiveFailures);
							if (instance->reinitializeRtdSensor(sensor))
							{
								sensor->consecutiveFailures = 0;
								ESP_LOGI(TAG, "RTD sensor %s reinitialized successfully", sensor->name.c_str());
							}
							else
							{
								sensor->consecutiveFailures = 0; // Reset counter to avoid spam
							}
						}
						
						continue; // Skip this sensor for control calculations
					}
				}
				else
				{
					// Successful reading - mark sensor as connected (recovery case)
					if (!sensor->connected)
					{
						ESP_LOGI(TAG, "RTD probe [%s] reconnected", stringId.c_str());
						sensor->connected = true;
					}
					// Reset failure counter on successful read
					sensor->consecutiveFailures = 0;
				}
			}
			else
			{
				ESP_LOGW(TAG, "Unknown sensor type for [%s], skipping", stringId.c_str());
				continue;
			}

			// conversion needed
			if (instance->temperatureScale == Fahrenheit)
			{
				temperature = (temperature * 1.8) + 32;
			}

			ESP_LOGD(TAG, "temperature read from [%s]: %.2f°", stringId.c_str(), temperature);

			// apply compensation
			if (sensor->compensateAbsolute != 0)
			{
				temperature = temperature + sensor->compensateAbsolute;
			}
			if (sensor->compensateRelative != 0 && sensor->compensateRelative != 1)
			{
				temperature = temperature * sensor->compensateRelative;
			}

			if (sensor->useForControl)
			{
				sum += temperature;
				nrOfSensors++;
			}

			sensor->lastTemp = temperature;

			// we also add our temps to a map individualy, might be nice to see bottom and top temp in gui
			if (sensor->show)
			{
				instance->currentTemperatures.insert_or_assign(key, sensor->lastTemp);
				
				// Store individual sensor temperature for persistent logging during control run
				if (instance->controlRun)
				{
					// Individual sensor logging removed - will use database instead
				}
			}
		}

		float avg = sum / nrOfSensors;

		ESP_LOGD(TAG, "Avg Temperature: %.2f°", avg);

		instance->temperature = avg;

		// when controlrun is true we need to keep out data
		if (instance->controlRun)
		{
			time_t current_raw_time = time(0);
			
			// Temperature logging removed - will use database instead
			
			// Add statistics data point every 6 cycles to reduce overhead
			it++;
			if (it > 5)
			{
				it = 0;
				instance->statisticsManager->AddDataPoint(current_raw_time, (int8_t)avg, (int8_t)instance->targetTemperature, instance->pidOutput);
				ESP_LOGD(TAG, "Logging: %.1f°", avg);
			}

			if (instance->mqttEnabled)
			{
				string iso_datetime = to_iso_8601(std::chrono::system_clock::now());
				json jPayload;
				jPayload["time"] = iso_datetime;
				jPayload["temp"] = instance->temperature;
				jPayload["target"] = instance->targetTemperature;
				jPayload["output"] = instance->pidOutput;
				string payload = jPayload.dump();

				esp_mqtt_client_publish(instance->mqttClient, instance->mqttTopic.c_str(), payload.c_str(), 0, 1, 1);
			}

			// Send to InfluxDB (with interval check)
			if (instance->influxdbEnabled)
			{
				auto now = system_clock::now();
				auto timeSinceLastSend = duration_cast<seconds>(now - instance->lastInfluxDBSend).count();
				
				if (timeSinceLastSend >= instance->influxdbSendInterval)
				{
					instance->lastInfluxDBSend = now;
				string fields = "temperature=" + to_string(instance->temperature) + 
								",target_temperature=" + to_string(instance->targetTemperature) + 
								",pid_output=" + to_string(instance->pidOutput);
				
				string tags = "status=" + instance->escapeInfluxDBTagValue(instance->statusText);
				if (instance->controlRun && !instance->selectedMashScheduleName.empty())
				{
					tags += ",schedule=" + instance->escapeInfluxDBTagValue(instance->selectedMashScheduleName);
				}
				
				instance->sendToInfluxDB("temperature", fields, tags);
				
				// Send individual sensor readings
				for (auto const &[sensorId, sensor] : instance->sensors)
				{
					if (sensor->show && sensor->lastTemp != -999)
					{
						string sensorFields = "temperature=" + to_string(sensor->lastTemp);
						string sensorTags = "sensor_id=" + to_string(sensorId) + 
										   ",sensor_name=" + instance->escapeInfluxDBTagValue(sensor->name) + 
										   ",sensor_type=" + to_string(sensor->sensorType);
						
						instance->sendToInfluxDB("sensor_temperature", sensorFields, sensorTags);
					}
				}
				}
			}
		}
	}

	vTaskDelete(NULL);
}

void BrewEngine::pidLoop(void *arg)
{
	BrewEngine *instance = (BrewEngine *)arg;

	double kP, kI, kD;
	if (instance->boilRun)
	{
		kP = instance->boilkP;
		kI = instance->boilkI;
		kD = instance->boilkD;
	}
	else
	{
		kP = instance->mashkP;
		kI = instance->mashkI;
		kD = instance->mashkD;
	}

	PIDController pid(kP, kI, kD);
	pid.setMin(0);
	pid.setMax(100);
	pid.debug = false;

	uint totalWattage = 0;

	// we calculate the total wattage we have availible, depens on heaters and on mash or boil
	for (auto &heater : instance->heaters)
	{

		if (instance->boilRun && heater->useForBoil)
		{
			totalWattage += heater->watt;
			heater->enabled = true;
		}
		else if (!instance->boilRun && heater->useForMash)
		{
			totalWattage += heater->watt;
			heater->enabled = true;
		}
		else
		{
			heater->enabled = false;
		}
	}

	while (instance->run && instance->controlRun)
	{
		// Output is %
		int outputPercent = (int)pid.getOutput((double)instance->temperature, (double)instance->targetTemperature);
		instance->pidOutput = outputPercent;
		ESP_LOGI(TAG, "Pid Output: %d Target: %f", instance->pidOutput, instance->targetTemperature);

		// Manual override and boost
		if (instance->manualOverrideOutput.has_value())
		{
			// Here we don't override the pidOutput display since we want the user to see the pid values even when overriding
			outputPercent = instance->manualOverrideOutput.value();
		}
		else if (instance->boostStatus == Boost)
		{
			outputPercent = 100;
			instance->pidOutput = 100;
		}
		else if (instance->boostStatus == Rest)
		{
			outputPercent = 0;
			instance->pidOutput = 0;
		}

		// set all to 0
		for (auto &heater : instance->heaters)
		{
			heater->burnTime = 0;
		}

		// calc the wattage we need
		int outputWatt = (totalWattage / 100) * outputPercent;

		// we need to calculate our burn time per output
		for (auto &heater : instance->heaters)
		{
			if (!heater->enabled)
			{
				continue;
			}

			if (outputWatt < 0)
			{
				break;
			}

			// we can complete it with this heater
			if (heater->watt > outputWatt)
			{
				heater->burnTime = (int)(((double)outputWatt / (double)heater->watt) * 100);
				ESP_LOGD(TAG, "Pid Calc Heater %s: OutputWatt: %d Burn: %d", heater->name.c_str(), outputWatt, heater->burnTime);
				break;
			}
			else
			{
				// we can't complete it, take out part and continue
				outputWatt -= heater->watt;
				heater->burnTime = 100;
				ESP_LOGD(TAG, "Pid Calc Heater %s: OutputWatt: %d Burn: 100", heater->name.c_str(), outputWatt);
			}
		}

		// we keep going for the desired pidlooptime and set the burn by percent
		for (int i = 0; i < instance->pidLoopTime; i++)
		{
			if (!instance->run || !instance->controlRun)
			{
				break;
			}

			for (auto &heater : instance->heaters)
			{
				if (!heater->enabled)
				{
					continue;
				}

				int burnUntil = 0;

				if (heater->burnTime > 0)
				{
					burnUntil = ((double)heater->burnTime / 100) * (double)instance->pidLoopTime; // convert % back to seconds
				}

				if (burnUntil > i) // on
				{
					if (heater->burn != true) // only when not current, we don't want to spam the logs
					{
						heater->burn = true;
						ESP_LOGD(TAG, "Heater %s: On", heater->name.c_str());
					}
				}
				else // off
				{
					if (heater->burn != false) // only when not current, we don't want to spam the logs
					{
						heater->burn = false;
						ESP_LOGD(TAG, "Heater %s: Off", heater->name.c_str());
					}
				}
			}

			// when our target changes we also update our pid target
			if (instance->resetPitTime)
			{
				ESP_LOGI(TAG, "Reset Pid Timer");
				instance->resetPitTime = false;
				break;
			}

			vTaskDelay(pdMS_TO_TICKS(1000));
		}
	}

	instance->pidOutput = 0;

	vTaskDelete(NULL);
}

void BrewEngine::outputLoop(void *arg)
{
	BrewEngine *instance = (BrewEngine *)arg;

	for (auto const &heater : instance->heaters)
	{
		gpio_set_level(heater->pinNr, instance->gpioLow);
	}

	while (instance->run && instance->controlRun)
	{
		vTaskDelay(pdMS_TO_TICKS(1000));

		for (auto const &heater : instance->heaters)
		{
			if (heater->burn)
			{
				ESP_LOGD(TAG, "Output %s: On", heater->name.c_str());
				gpio_set_level(heater->pinNr, instance->gpioHigh);
			}
			else
			{
				ESP_LOGD(TAG, "Output %s: Off", heater->name.c_str());
				gpio_set_level(heater->pinNr, instance->gpioLow);
			}
		}
	}

	// set outputs off and quit thread
	for (auto const &heater : instance->heaters)
	{
		gpio_set_level(heater->pinNr, instance->gpioLow);
	}

	vTaskDelete(NULL);
}

void BrewEngine::controlLoop(void *arg)
{
	BrewEngine *instance = (BrewEngine *)arg;

	// the pid needs to reset one step later so the next temp is set, oherwise it has a delay
	bool resetPIDNextStep = false;

	// For boost mode to see if temp starts to drop
	float prevTemperature = instance->temperature;
	uint boostUntil = 0;

	while (instance->run && instance->controlRun)
	{

		system_clock::time_point now = std::chrono::system_clock::now();

		if (instance->executionSteps.size() >= instance->currentMashStep)
		{ // there are more steps
			int nextStepIndex = instance->currentMashStep;

			auto nextStep = instance->executionSteps.at(nextStepIndex);

			system_clock::time_point nextAction = nextStep->time;

			bool gotoNextStep = false;

			// set target when not overriden
			if (instance->overrideTargetTemperature.has_value())
			{
				instance->targetTemperature = instance->overrideTargetTemperature.value();
			}
			else
			{
				instance->targetTemperature = nextStep->temperature;
			}

			uint secondsToGo = 0;
			// if its smaller 0 is ok!
			if (nextAction > now)
			{
				secondsToGo = chrono::duration_cast<chrono::seconds>(nextAction - now).count();
			}

			// Boost mode logic
			if (nextStep->allowBoost)
			{
				if (boostUntil == 0)
				{
					boostUntil = (uint)((nextStep->temperature / 100) * (float)instance->boostModeUntil);
				}

				if (instance->boostStatus == Off && instance->temperature < boostUntil)
				{

					ESP_LOGI(TAG, "Boost Start Until: %d", boostUntil);
					instance->logRemote("Boost Start");
					instance->boostStatus = Boost;
				}
				else if (instance->boostStatus == Boost && instance->temperature >= boostUntil)
				{
					// When in boost mode we wait unit boost temp is reched, pid is locked to 100% in boost mode
					ESP_LOGI(TAG, "Boost Rest Start");
					instance->logRemote("Boost Rest Start");
					instance->boostStatus = Rest;
				}
				else if (instance->boostStatus == Rest && instance->temperature < prevTemperature)
				{
					// When in boost rest mode, we wait until temperature drops pid is locked to 0%
					ESP_LOGI(TAG, "Boost Rest End");
					instance->logRemote("Boost Rest End");
					instance->boostStatus = Off;

					// Reset pid
					instance->resetPitTime = true;
				}
			}

			if (secondsToGo < 1)
			{ // change temp and increment Currentstep

				// string iso_string = instance->to_iso_8601(nextStep->time);
				// ESP_LOGI(TAG, "Control Time:%s, TempCur:%f, TempTarget:%d, Extend:%d, Overtime: %d", iso_string.c_str(), instance->temperature, nextStep->temperature, nextStep->extendIfNeeded, instance->inOverTime);

				if (nextStep->extendIfNeeded == true && instance->inOverTime == false && (nextStep->temperature - instance->temperature) >= instance->tempMargin)
				{
					// temp must be reached, we keep going but need to triger a recaluclation event when done
					ESP_LOGI(TAG, "OverTime Start");
					instance->logRemote("OverTime Start");
					instance->inOverTime = true;
				}
				else if (instance->inOverTime == true && (nextStep->temperature - instance->temperature) <= instance->tempMargin)
				{
					// we reached out temp after overtime, we need to recalc the rest and start going again
					ESP_LOGI(TAG, "OverTime Done");
					instance->logRemote("OverTime Done");
					instance->inOverTime = false;
					instance->recalculateScheduleAfterOverTime();
					gotoNextStep = true;
				}
				else if (instance->inOverTime == false)
				{
					ESP_LOGI(TAG, "Going to next Step");
					gotoNextStep = true;
					// also reset override on step change
					instance->overrideTargetTemperature = std::nullopt;
				}

				// else when in overtime just keep going until we reach temp
			}

			// the pid needs to reset one step later so the next temp is set, oherwise it has a delay
			if (resetPIDNextStep)
			{
				resetPIDNextStep = false;
				instance->resetPitTime = true;
			}

			if (gotoNextStep)
			{
				instance->currentMashStep++;

				// Also reset boost
				instance->boostStatus = Off;

				resetPIDNextStep = true;
			}

			// notifications, but only when not in overtime
			if (!instance->inOverTime && !instance->notifications.empty())
			{
				// filter out items that are not done
				auto isNotDone = [](Notification *notification)
				{ return notification->done == false; };

				auto notDone = instance->notifications | views::filter(isNotDone);

				if (!notDone.empty())
				{
					// they are sorted so we just have to check the first one
					auto first = notDone.front();

					if (now > first->timePoint)
					{
						ESP_LOGI(TAG, "Notify %s", first->name.c_str());

						string buzzerName = "buzzer" + first->name;
						xTaskCreate(&instance->buzzer, buzzerName.c_str(), 1024, instance, 10, NULL);

						first->done = true;
					}
				}
			}
		}
		else
		{
			// last step need to stop
			ESP_LOGI(TAG, "Program Finished");
			instance->stop();
		}

		// For boost mode to see if temp starts to drop
		prevTemperature = instance->temperature;

		vTaskDelay(pdMS_TO_TICKS(1000));
	}

	vTaskDelete(NULL);
}

string BrewEngine::bootIntoRecovery()
{
	// recovery is our factory
	esp_partition_subtype_t t = ESP_PARTITION_SUBTYPE_APP_FACTORY;
	const esp_partition_t *factory_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP, t, NULL);

	if (factory_partition == NULL)
	{
		ESP_LOGE(TAG, "Factory Partition not found!");
		return "Error: Factory Partition not found!";
	}

	if (esp_ota_set_boot_partition(factory_partition) == ESP_OK)
	{
		return "Device is booting into recovery hold on!";
	}

	return "Error: Unable to chage boot Partition!";
}

void BrewEngine::reboot(void *arg)
{
	vTaskDelay(2000 / portTICK_PERIOD_MS);
	esp_restart();
}

void BrewEngine::buzzer(void *arg)
{
	BrewEngine *instance = (BrewEngine *)arg;

	if (instance->buzzer_PIN > 0)
	{
		auto buzzerMs = instance->buzzerTime * 1000;
		gpio_set_level(instance->buzzer_PIN, instance->gpioHigh);
		vTaskDelay(buzzerMs / portTICK_PERIOD_MS);
		gpio_set_level(instance->buzzer_PIN, instance->gpioLow);
	}

	vTaskDelete(NULL);
}

string BrewEngine::processCommand(const string &payLoad)
{
	ESP_LOGD(TAG, "payLoad %s", payLoad.c_str());

	json jCommand = json::parse(payLoad);
	string command = jCommand["command"];
	json data = jCommand["data"];

	ESP_LOGD(TAG, "processCommand %s", command.c_str());
	ESP_LOGD(TAG, "data %s", data.dump().c_str());

	json resultData = {};
	string message = "";
	bool success = true;

	if (command == "Data")
	{
		time_t lastLogDateTime = time(0);

		json jTempLog = json::array({});
		if (!this->tempLog.empty())
		{
			auto lastLog = this->tempLog.rend();
			lastLogDateTime = lastLog->first;

			// If we have a last date we only need to send the log increment
			if (!data["lastDate"].is_null() && data["lastDate"].is_number())
			{
				time_t lastClientDate = (time_t)data["lastDate"];
				ESP_LOGD(TAG, "lastClientDate %s", ctime(&lastClientDate));

				// most efficient seems to loop reverse and add until date is reached
				for (auto iter = this->tempLog.rbegin(); iter != this->tempLog.rend(); ++iter)
				{
					if (iter->first > lastClientDate)
					{
						json jTempLogItem;
						jTempLogItem["time"] = iter->first;
						// Handle both old format (integers) and new format (tenths)
						// New format values are typically > 100 (e.g., 270 for 27.0°C)
						// Old format values are typically < 100 (e.g., 27 for 27°C)
						double tempValue;
						if (iter->second > 100 || iter->second < 0) {
							// New format: stored as tenths of degrees
							tempValue = (double)(iter->second) / 10.0;
						} else {
							// Old format: stored as whole degrees
							tempValue = (double)(iter->second);
						}
						jTempLogItem["temp"] = tempValue;
						jTempLog.push_back(jTempLogItem);
					}
					else
					{
						break;
					}
				}
			}
			else
			{
				for (auto iter = this->tempLog.rbegin(); iter != this->tempLog.rend(); ++iter)
				{
					json jTempLogItem;
					jTempLogItem["time"] = iter->first;
					// Handle both old format (integers) and new format (tenths)
					// New format values are typically > 100 (e.g., 270 for 27.0°C)
					// Old format values are typically < 100 (e.g., 27 for 27°C)
					double tempValue;
					if (iter->second > 100 || iter->second < 0) {
						// New format: stored as tenths of degrees
						tempValue = (double)(iter->second) / 10.0;
					} else {
						// Old format: stored as whole degrees
						tempValue = (double)(iter->second);
					}
					jTempLogItem["temp"] = tempValue;
					jTempLog.push_back(jTempLogItem);
				}
			}
		}

		// currenttemps is an array of current temps, they are not necessarily all used for control
		json jCurrentTemps = json::array({});
		for (auto const &[key, val] : this->currentTemperatures)
		{
			json jCurrentTemp;
			jCurrentTemp["sensor"] = to_string(key);			   // js doesn't support unint64
			jCurrentTemp["temp"] = (double)((int)(val * 10)) / 10; // round float to 1 digit for display
			jCurrentTemps.push_back(jCurrentTemp);
		}

		// sensorTempLogs removed - will fetch from database instead to save memory
		resultData = {
			{"temp", (double)((int)(this->temperature * 10)) / 10}, // round float to 1 digit for display
			{"temps", jCurrentTemps},
			{"targetTemp", (double)((int)(this->targetTemperature * 10)) / 10}, // round float to 1 digit for display,
			{"manualOverrideTargetTemp", nullptr},
			{"output", this->pidOutput},
			{"manualOverrideOutput", nullptr},
			{"status", this->statusText},
			{"stirStatus", this->stirStatusText},
			{"lastLogDateTime", lastLogDateTime},
			{"tempLog", json::array({})}, // Empty array for backward compatibility
			{"sensorTempLogs", json::array({})}, // Empty array - will fetch from database instead
			{"runningVersion", this->runningVersion},
			{"inOverTime", this->inOverTime},
			{"boostStatus", this->boostStatus},
		};

		if (this->manualOverrideOutput.has_value())
		{
			resultData["manualOverrideOutput"] = this->manualOverrideOutput.value();
		}

		if (this->overrideTargetTemperature.has_value())
		{
			resultData["manualOverrideTargetTemp"] = this->overrideTargetTemperature.value();
		}
	}
	else if (command == "GetRunningSchedule")
	{
		json jRunningSchedule;
		jRunningSchedule["version"] = this->runningVersion;

		json jExecutionSteps = json::array({});
		for (auto const &[key, val] : this->executionSteps)
		{
			json jExecutionStep = val->to_json();
			jExecutionSteps.push_back(jExecutionStep);
		}
		jRunningSchedule["steps"] = jExecutionSteps;

		json jNotifications = json::array({});
		for (auto &notification : this->notifications)
		{
			json jNotification = notification->to_json();
			jNotifications.push_back(jNotification);
		}
		jRunningSchedule["notifications"] = jNotifications;

		resultData = jRunningSchedule;
	}
	else if (command == "SetTemp")
	{

		if (data["targetTemp"].is_null())
		{
			this->overrideTargetTemperature = std::nullopt;

			// when not in a program also direclty set targtetemp
			if (this->selectedMashScheduleName.empty() == true)
			{
				this->targetTemperature = 0;
			}
		}
		else if (data["targetTemp"].is_number())
		{

			this->overrideTargetTemperature = (float)data["targetTemp"];

			// when not in a program also direclty set targtetemp
			if (this->selectedMashScheduleName.empty() == true)
			{
				this->targetTemperature = this->overrideTargetTemperature.value();
			}
		}
		else
		{
			this->overrideTargetTemperature = std::nullopt;

			message = "Incorrect data, integer or float expected!";
			success = false;
		}
	}
	else if (command == "SetOverrideOutput")
	{

		if (data["output"].is_null() == false && data["output"].is_number())
		{
			this->manualOverrideOutput = (int)data["output"];
		}
		else
		{
			this->manualOverrideOutput = std::nullopt;
		}

		// reset so effect is immidiate
		this->resetPitTime = true;
	}
	else if (command == "Start")
	{
		if (data["selectedMashSchedule"].is_null())
		{
			this->selectedMashScheduleName.clear();
		}
		else
		{
			this->selectedMashScheduleName = (string)data["selectedMashSchedule"];
		}

		this->start();
		this->statisticsManager->StartSession(this->selectedMashScheduleName);
		
		// Log session start to InfluxDB
		if (this->influxdbEnabled)
		{
			time_t startTime = time(0);
			string fields = "schedule_name=\"" + this->selectedMashScheduleName + "\"" +
							",start_time=" + to_string(startTime) +
							",completed=false";
			this->sendToInfluxDB("session_metadata", fields);
		}
	}
	else if (command == "StartStir")
	{
		this->startStir(data);
	}
	else if (command == "Stop")
	{
		this->stop();
		this->statisticsManager->EndSession();
		
		// Log session end to InfluxDB
		if (this->influxdbEnabled)
		{
			time_t endTime = time(0);
			string fields = "end_time=" + to_string(endTime) +
							",completed=true";
			this->sendToInfluxDB("session_metadata", fields);
		}
	}
	else if (command == "StopStir")
	{
		this->stopStir();
	}
	else if (command == "GetMashSchedules")
	{

		json jSchedules = json::array({});

		for (auto const &[key, val] : this->mashSchedules)
		{
			json jSchedule = val->to_json();
			jSchedules.push_back(jSchedule);
		}

		resultData = jSchedules;
	}
	else if (command == "SaveMashSchedule")
	{
		this->setMashSchedule(data);

		this->saveMashSchedules();
	}
	else if (command == "SetMashSchedule") // used by import function to set but not save
	{
		this->setMashSchedule(data);
	}
	else if (command == "DeleteMashSchedule")
	{
		string deleteName = (string)data["name"];

		auto pos = this->mashSchedules.find(deleteName);

		if (pos == this->mashSchedules.end())
		{
			message = "Schedule with name: " + deleteName + " not found";
			success = false;
		}
		else
		{
			this->mashSchedules.erase(pos);
			this->saveMashSchedules();
		}
	}
	else if (command == "GetPIDSettings")
	{
		resultData = {
			{"kP", this->mashkP},
			{"kI", this->mashkI},
			{"kD", this->mashkD},
			{"boilkP", this->boilkP},
			{"boilkI", this->boilkI},
			{"boilkD", this->boilkD},
			{"pidLoopTime", this->pidLoopTime},
			{"stepInterval", this->stepInterval},
			{"boostModeUntil", this->boostModeUntil},
		};
	}
	else if (command == "SavePIDSettings")
	{
		this->mashkP = data["kP"].get<double>();
		this->mashkI = data["kI"].get<double>();
		this->mashkD = data["kD"].get<double>();
		this->boilkP = data["boilkP"].get<double>();
		this->boilkI = data["boilkI"].get<double>();
		this->boilkD = data["boilkD"].get<double>();
		this->pidLoopTime = data["pidLoopTime"].get<uint16_t>();
		this->stepInterval = data["stepInterval"].get<uint16_t>();
		this->boostModeUntil = data["boostModeUntil"].get<uint8_t>();
		this->savePIDSettings();
	}
	else if (command == "GetTempSettings")
	{
		// Convert sensors to json
		json jSensors = json::array({});

		for (auto const &[key, val] : this->sensors)
		{
			json jSensor = val->to_json();
			jSensors.push_back(jSensor);
		}

		resultData = jSensors;
	}
	else if (command == "SaveTempSettings")
	{
		this->saveTempSensorSettings(data);
	}
	else if (command == "DetectTempSensors")
	{
		this->detectOnewireTemperatureSensors();
	}
	else if (command == "AddRtdSensor")
	{
		if (!this->rtdSensorsEnabled)
		{
			success = false;
			message = "RTD sensors are not enabled in system settings";
		}
		else
		{
			string name = data["name"];
			int csPin = data["csPin"];
			int sensorType = data["sensorType"];
			bool useForControl = data["useForControl"];
			bool show = data["show"];
			
			// Generate unique ID for RTD sensor
			uint64_t rtdSensorId = 0x31865000 + csPin; // Base ID + CS pin
			
			// Check if sensor already exists
			if (this->sensors.find(rtdSensorId) != this->sensors.end())
			{
				success = false;
				message = "RTD sensor with CS pin " + to_string(csPin) + " already exists";
			}
			else
			{
				// Validate GPIO pin
				if (csPin < 0 || csPin >= GPIO_NUM_MAX)
				{
					success = false;
					message = "Invalid CS pin number: " + to_string(csPin);
				}
				else
				{
					// Initialize MAX31865 device (SPI bus should already be initialized)
					max31865_t *rtd_sensor = new max31865_t;
					esp_err_t ret = max31865_init_desc(rtd_sensor, SPI2_HOST, csPin);
					
					if (ret == ESP_OK)
					{
						// Configure MAX31865
						ret = max31865_set_config(rtd_sensor, true, 1, false, false, 0, true, true, 0, 0xFFFF);
					}
					
					if (ret == ESP_OK)
					{
						// Set RTD parameters based on sensor type
						if (sensorType == SENSOR_PT100)
						{
							rtd_sensor->rtd_nominal = 100;
							rtd_sensor->ref_resistor = 430;
						}
						else if (sensorType == SENSOR_PT1000)
						{
							rtd_sensor->rtd_nominal = 1000;
							rtd_sensor->ref_resistor = 4300;
						}
						
						// Create temperature sensor object
						auto sensor = new TemperatureSensor();
						sensor->id = rtdSensorId;
						sensor->name = name;
						sensor->color = (sensorType == SENSOR_PT100) ? "#00C853" : "#FF9800"; // Green for PT100, Orange for PT1000
						sensor->useForControl = useForControl;
						sensor->show = show;
						sensor->connected = true;
						sensor->compensateAbsolute = 0;
						sensor->compensateRelative = 1;
						sensor->sensorType = (SensorType)sensorType;
						sensor->max31865Handle = *rtd_sensor;
						sensor->consecutiveFailures = 0; // Initialize failure counter
						
						this->sensors.insert_or_assign(sensor->id, sensor);
						this->rtdSensors.push_back(rtd_sensor);
						this->rtdSensorCount++;
						
						// Save sensor settings
						json jSensors = json::array({});
						for (auto const &[key, val] : this->sensors)
						{
							json jSensor = val->to_json();
							jSensors.push_back(jSensor);
						}
						this->saveTempSensorSettings(jSensors);
						
						ESP_LOGI(TAG, "RTD sensor added successfully: %s (CS pin %d)", name.c_str(), csPin);
						message = "RTD sensor added successfully";
					}
					else
					{
						delete rtd_sensor;
						success = false;
						message = "Failed to initialize MAX31865: " + string(esp_err_to_name(ret));
						ESP_LOGE(TAG, "Failed to initialize RTD sensor: %s", esp_err_to_name(ret));
					}
				}
			}
		}
	}
	else if (command == "GetHeaterSettings")
	{
		// Convert heaters to json
		json jHeaters = json::array({});

		for (auto const &heater : this->heaters)
		{
			json jHeater = heater->to_json();
			jHeaters.push_back(jHeater);
		}

		resultData = jHeaters;
	}
	else if (command == "SaveHeaterSettings")
	{
		if (this->controlRun)
		{
			message = "You cannot save heater settings while running!";
			success = false;
		}
		else
		{
			this->saveHeaterSettings(data);
		}
	}
	else if (command == "GetWifiSettings")
	{
		// get data from wifi-connect
		if (this->GetWifiSettingsJson)
		{
			resultData = this->GetWifiSettingsJson();
		}
	}
	else if (command == "SaveWifiSettings")
	{
		// save via wifi-connect
		if (this->SaveWifiSettingsJson)
		{
			this->SaveWifiSettingsJson(data);
		}
		message = "Please restart device for changes to have effect!";
	}
	else if (command == "ScanWifi")
	{
		// scans for networks
		if (this->ScanWifiJson)
		{
			resultData = this->ScanWifiJson();
		}
	}
	else if (command == "GetSystemSettings")
	{
		resultData = {
			{"onewirePin", this->oneWire_PIN},
			{"stirPin", this->stir_PIN},
			{"buzzerPin", this->buzzer_PIN},
			{"buzzerTime", this->buzzerTime},
			{"invertOutputs", this->invertOutputs},
			{"mqttUri", this->mqttUri},
			{"temperatureScale", this->temperatureScale},
			{"rtdSensorsEnabled", this->rtdSensorsEnabled},
			{"spiMosiPin", this->spi_mosi_pin},
			{"spiMisoPin", this->spi_miso_pin},
			{"spiClkPin", this->spi_clk_pin},
			{"spiCsPin", this->spi_cs_pin},
			{"influxdbUrl", this->influxdbUrl},
			{"influxdbToken", this->influxdbToken},
			{"influxdbOrg", this->influxdbOrg},
			{"influxdbBucket", this->influxdbBucket},
			{"influxdbSendInterval", this->influxdbSendInterval},
		};
	}
	else if (command == "SaveSystemSettings")
	{
		this->saveSystemSettingsJson(data);
		message = "Please restart device for changes to have effect!";
	}
	else if (command == "TestInfluxDB")
	{
		if (this->influxdbUrl.empty() || this->influxdbToken.empty() || 
		    this->influxdbOrg.empty() || this->influxdbBucket.empty())
		{
			message = "InfluxDB configuration incomplete";
			success = false;
		}
		else
		{
			// Test connection with a dummy data point
			string testData = "test_connection,source=brew_engine value=1";
			this->sendToInfluxDB("test_connection", "value=1", "");
			message = "InfluxDB connection test completed - check logs for results";
		}
	}
	else if (command == "Reboot")
	{
		xTaskCreate(&this->reboot, "reboot_task", 1024, this, 5, NULL);
	}
	else if (command == "FactoryReset")
	{
		this->settingsManager->FactoryReset();
		message = "Device will restart shortly, reconnect to factory wifi settings to continue!";
		xTaskCreate(&this->reboot, "reboot_task", 1024, this, 5, NULL);
	}
	else if (command == "BootIntoRecovery")
	{
		message = this->bootIntoRecovery();

		if (message.find("Error") != std::string::npos)
		{
			success = false;
		}
		else
		{
			xTaskCreate(&this->reboot, "reboot_task", 1024, this, 5, NULL);
		}
	}
	else if (command == "GetStatistics")
	{
		if (this->influxdbEnabled)
		{
			resultData = this->getInfluxDBStatistics(data);
		}
		else
		{
			vector<BrewSession> sessions = this->statisticsManager->GetSessionList();
			json jSessions = json::array();
			
			for (const auto& session : sessions) {
				json jSession;
				jSession["sessionId"] = session.sessionId;
				jSession["scheduleName"] = session.scheduleName;
				jSession["startTime"] = session.startTime;
				jSession["endTime"] = session.endTime;
			jSession["duration"] = session.totalDuration;
			jSession["dataPoints"] = session.dataPoints;
			jSession["avgTemperature"] = session.avgTemperature;
			jSession["minTemperature"] = session.minTemperature;
			jSession["maxTemperature"] = session.maxTemperature;
			jSession["completed"] = session.completed;
			jSessions.push_back(jSession);
			}
			
			resultData["sessions"] = jSessions;
			
			map<string, uint32_t> stats = this->statisticsManager->GetSessionStats();
			resultData["stats"] = stats;
			
			json jConfig;
			jConfig["maxSessions"] = this->statisticsManager->GetMaxSessions();
			jConfig["currentSessionActive"] = this->statisticsManager->IsSessionActive();
			if (this->statisticsManager->IsSessionActive()) {
				jConfig["currentSessionId"] = this->statisticsManager->GetCurrentSessionId();
				jConfig["currentDataPoints"] = this->statisticsManager->GetCurrentSessionDataPoints();
			}
			resultData["config"] = jConfig;
		}
	}
	else if (command == "GetSessionData")
	{
		if (data["sessionId"].is_null()) {
			message = "Session ID required";
			success = false;
		}
		else {
			if (this->influxdbEnabled)
			{
				resultData = this->getInfluxDBSessionData(data);
			}
			else
			{
				uint32_t sessionId = data["sessionId"];
				BrewSession session = this->statisticsManager->GetSessionById(sessionId);
				
				if (session.sessionId == 0) {
					message = "Session not found";
					success = false;
				}
				else {
					vector<TempDataPoint> sessionData = this->statisticsManager->GetSessionData(sessionId);
				
				json jSession;
				jSession["sessionId"] = session.sessionId;
				jSession["scheduleName"] = session.scheduleName;
				jSession["startTime"] = session.startTime;
				jSession["endTime"] = session.endTime;
				jSession["duration"] = session.totalDuration;
				jSession["avgTemperature"] = session.avgTemperature;
				jSession["minTemperature"] = session.minTemperature;
				jSession["maxTemperature"] = session.maxTemperature;
				jSession["completed"] = session.completed;
				
				json jData = json::array();
				for (const auto& point : sessionData) {
					json jPoint;
					jPoint["timestamp"] = point.timestamp;
					jPoint["avgTemp"] = (int)point.avgTemp;
					jPoint["targetTemp"] = (int)point.targetTemp;
					jPoint["pidOutput"] = (int)point.pidOutput;
					jData.push_back(jPoint);
				}
				
					jSession["data"] = jData;
					resultData = jSession;
				}
			}
		}
	}
	else if (command == "ExportSession")
	{
		if (data["sessionId"].is_null()) {
			message = "Session ID required";
			success = false;
		}
		else {
			uint32_t sessionId = data["sessionId"];
			string format = data.value("format", "json");
			
			if (format == "json") {
				string exportData = this->statisticsManager->ExportSessionToJson(sessionId);
				if (exportData == "{}") {
					message = "Session not found";
					success = false;
				}
				else {
					resultData["exportData"] = exportData;
					resultData["format"] = "json";
				}
			}
			else if (format == "csv") {
				string exportData = this->statisticsManager->ExportSessionToCsv(sessionId);
				if (exportData.empty()) {
					message = "Session not found or no data";
					success = false;
				}
				else {
					resultData["exportData"] = exportData;
					resultData["format"] = "csv";
				}
			}
			else {
				message = "Invalid format. Use 'json' or 'csv'";
				success = false;
			}
		}
	}
	else if (command == "SetStatisticsConfig")
	{
		if (!data["maxSessions"].is_null()) {
			uint8_t maxSessions = data["maxSessions"];
			this->statisticsManager->SetMaxSessions(maxSessions);
		}
		
		resultData["maxSessions"] = this->statisticsManager->GetMaxSessions();
		message = "Statistics configuration updated";
	}

	json jResultPayload;
	jResultPayload["data"] = resultData;
	jResultPayload["success"] = success;

	if (message != "")
	{
		jResultPayload["message"] = message;
	}

	// Log memory usage before JSON serialization
	ESP_LOGI(TAG, "Free heap before JSON serialization: %lu bytes", esp_get_free_heap_size());
	ESP_LOGI(TAG, "Min free heap: %lu bytes", esp_get_minimum_free_heap_size());

	string resultPayload;
	try {
		resultPayload = jResultPayload.dump();
		ESP_LOGD(TAG, "JSON serialization successful, size: %zu bytes", resultPayload.length());
	} catch (const std::exception& e) {
		ESP_LOGE(TAG, "JSON serialization failed: %s", e.what());
		ESP_LOGE(TAG, "Free heap after failure: %lu bytes", esp_get_free_heap_size());
		
		// Return minimal error response
		json errorResponse;
		errorResponse["success"] = false;
		errorResponse["message"] = "Memory allocation error during JSON serialization";
		errorResponse["data"] = json::object();
		resultPayload = errorResponse.dump();
	}

	return resultPayload;
}

httpd_handle_t BrewEngine::startWebserver(void)
{

	httpd_uri_t indexUri;
	indexUri.uri = "/";
	indexUri.method = HTTP_GET;
	indexUri.handler = this->indexGetHandler;

	httpd_uri_t logoUri;
	logoUri.uri = "/logo.svg";
	logoUri.method = HTTP_GET;
	logoUri.handler = this->logoGetHandler;

	httpd_uri_t manifestUri;
	manifestUri.uri = "/manifest.json";
	manifestUri.method = HTTP_GET;
	manifestUri.handler = this->manifestGetHandler;

	httpd_uri_t postUri;
	postUri.uri = "/api";
	postUri.method = HTTP_POST;
	postUri.handler = this->apiPostHandler;

	httpd_uri_t optionsUri;
	optionsUri.uri = "/api";
	optionsUri.method = HTTP_OPTIONS;
	optionsUri.handler = this->apiOptionsHandler;

	httpd_uri_t otherUri;
	otherUri.uri = "/*";
	otherUri.method = HTTP_GET;
	otherUri.handler = this->otherGetHandler;

	httpd_handle_t server = NULL;
	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	// whiout this the esp crashed whitout a proper warning
	config.stack_size = 20480;
	config.uri_match_fn = httpd_uri_match_wildcard;

	// Start the httpd server
	ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
	if (httpd_start(&server, &config) == ESP_OK)
	{
		// Set URI handlers
		httpd_register_uri_handler(server, &indexUri);
		httpd_register_uri_handler(server, &logoUri);
		httpd_register_uri_handler(server, &manifestUri);
		httpd_register_uri_handler(server, &otherUri);
		httpd_register_uri_handler(server, &postUri);
		httpd_register_uri_handler(server, &optionsUri);
		return server;
	}

	ESP_LOGI(TAG, "Error starting server!");
	return NULL;
}

void BrewEngine::logRemote(const string &message)
{
	if (this->mqttEnabled)
	{
		string iso_datetime = this->to_iso_8601(std::chrono::system_clock::now());
		json jPayload;
		jPayload["time"] = iso_datetime;
		jPayload["level"] = "Debug";
		jPayload["message"] = message;
		string payload = jPayload.dump();

		esp_mqtt_client_publish(this->mqttClient, this->mqttTopicLog.c_str(), payload.c_str(), 0, 1, 1);
	}
}

void BrewEngine::stopWebserver(httpd_handle_t server)
{
	// Stop the httpd server
	httpd_stop(server);
}

esp_err_t BrewEngine::indexGetHandler(httpd_req_t *req)
{
	// ESP_LOGI(TAG, "index_get_handler");
	extern const unsigned char index_html_start[] asm("_binary_index_html_gz_start");
	extern const unsigned char index_html_end[] asm("_binary_index_html_gz_end");
	const size_t index_html_size = (index_html_end - index_html_start);
	httpd_resp_set_type(req, "text/html");
	httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
	httpd_resp_send(req, (const char *)index_html_start, index_html_size);

	return ESP_OK;
}

esp_err_t BrewEngine::logoGetHandler(httpd_req_t *req)
{
	extern const unsigned char logo_svg_file_start[] asm("_binary_logo_svg_gz_start");
	extern const unsigned char logo_svg_file_end[] asm("_binary_logo_svg_gz_end");
	const size_t logo_svg_file_size = (logo_svg_file_end - logo_svg_file_start);
	httpd_resp_set_type(req, "image/svg+xml");
	httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
	httpd_resp_send(req, (const char *)logo_svg_file_start, logo_svg_file_size);

	return ESP_OK;
}

esp_err_t BrewEngine::manifestGetHandler(httpd_req_t *req)
{
	extern const unsigned char manifest_json_file_start[] asm("_binary_manifest_json_start");
	extern const unsigned char manifest_json_file_end[] asm("_binary_manifest_json_end");
	const size_t manifest_json_file_size = (manifest_json_file_end - manifest_json_file_start);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_send(req, (const char *)manifest_json_file_start, manifest_json_file_size);

	return ESP_OK;
}

esp_err_t BrewEngine::otherGetHandler(httpd_req_t *req)
{
	httpd_resp_set_status(req, "307 Temporary Redirect");
	httpd_resp_set_hdr(req, "Location", "/");
	httpd_resp_send(req, "<html><body>Wrong</body></html>", 0); // Response body can be empty

	return ESP_OK;
}

esp_err_t BrewEngine::apiPostHandler(httpd_req_t *req)
{
	string stringBuffer;
	char buf[256];
	uint32_t ret;
	uint32_t remaining = req->content_len;

	while (remaining > 0)
	{
		// Read the data
		int nBytes = (std::min<uint32_t>)(remaining, sizeof(buf));

		if ((ret = httpd_req_recv(req, buf, nBytes)) <= 0)
		{
			if (ret == HTTPD_SOCK_ERR_TIMEOUT)
			{
				// Timeout, just continue
				continue;
			}

			return ESP_FAIL;
		}

		size_t bytes_read = ret;

		remaining -= bytes_read;

		// append to buffer
		stringBuffer.append((char *)buf, bytes_read);
	}

	string commandResult = mainInstance->processCommand(stringBuffer);

	const char *returnBuf = commandResult.c_str();
	httpd_resp_set_type(req, "text/plain");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_sendstr(req, returnBuf);

	return ESP_OK;
}

// needed for cors
esp_err_t BrewEngine::apiOptionsHandler(httpd_req_t *req)
{
	string commandResult = "";
	const char *returnBuf = commandResult.c_str();

	httpd_resp_set_type(req, "text/plain");
	httpd_resp_set_hdr(req, "Access-Control-Max-Age", "1728000");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, PATCH, OPTIONS");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization,Content-Type,Accept,Origin,User-Agent,DNT,Cache-Control,X-Mx-ReqToken,Keep-Alive,X-Requested-With,If-Modified-Since");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_status(req, "204");
	httpd_resp_sendstr(req, returnBuf);

	return ESP_OK;
}

string BrewEngine::to_iso_8601(std::chrono::time_point<std::chrono::system_clock> t)
{

	// convert to time_t which will represent the number of
	// seconds since the UNIX epoch, UTC 00:00:00 Thursday, 1st. January 1970
	auto epoch_seconds = std::chrono::system_clock::to_time_t(t);

	// Format this as date time to seconds resolution
	// e.g. 2016-08-30T08:18:51
	std::stringstream stream;
	stream << std::put_time(gmtime(&epoch_seconds), "%FT%T");

	// If we now convert back to a time_point we will get the time truncated
	// to whole seconds
	auto truncated = std::chrono::system_clock::from_time_t(epoch_seconds);

	// Now we subtract this seconds count from the original time to
	// get the number of extra microseconds..
	auto delta_us = std::chrono::duration_cast<std::chrono::microseconds>(t - truncated).count();

	// And append this to the output stream as fractional seconds
	// e.g. 2016-08-30T08:18:51.867479
	stream << "." << std::fixed << std::setw(6) << std::setfill('0') << delta_us;

	return stream.str();
}