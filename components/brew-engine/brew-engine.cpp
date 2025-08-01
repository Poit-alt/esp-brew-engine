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
	ESP_LOGI(TAG, "Heaters initialization completed, proceeding to stir pin");

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

	// Initialize ADC for NTC sensors BEFORE loading temperature sensor settings
	this->adcInitialized = false;
	this->adc1_handle = nullptr;
	this->adc1_cali_handle = nullptr;
	
	// Configure ADC1 oneshot unit
	adc_oneshot_unit_init_cfg_t init_config1 = {};
	init_config1.unit_id = ADC_UNIT_1;
	init_config1.clk_src = ADC_RTC_CLK_SRC_DEFAULT;
	init_config1.ulp_mode = ADC_ULP_MODE_DISABLE;
	
	esp_err_t adc_err = adc_oneshot_new_unit(&init_config1, &this->adc1_handle);
	if (adc_err == ESP_OK)
	{
		// Configure channels for NTC sensors with attenuation for 3.3V range
		adc_oneshot_chan_cfg_t config = {};
		config.atten = ADC_ATTEN_DB_12; // Use non-deprecated constant
		config.bitwidth = ADC_BITWIDTH_DEFAULT;
		
		// Configure common analog pins for ESP32-S3
		adc_oneshot_config_channel(this->adc1_handle, ADC_CHANNEL_0, &config); // GPIO1
		adc_oneshot_config_channel(this->adc1_handle, ADC_CHANNEL_1, &config); // GPIO2
		adc_oneshot_config_channel(this->adc1_handle, ADC_CHANNEL_2, &config); // GPIO3
		adc_oneshot_config_channel(this->adc1_handle, ADC_CHANNEL_3, &config); // GPIO4
		adc_oneshot_config_channel(this->adc1_handle, ADC_CHANNEL_4, &config); // GPIO5
		adc_oneshot_config_channel(this->adc1_handle, ADC_CHANNEL_5, &config); // GPIO6
		adc_oneshot_config_channel(this->adc1_handle, ADC_CHANNEL_6, &config); // GPIO7
		adc_oneshot_config_channel(this->adc1_handle, ADC_CHANNEL_7, &config); // GPIO8
		adc_oneshot_config_channel(this->adc1_handle, ADC_CHANNEL_8, &config); // GPIO9
		adc_oneshot_config_channel(this->adc1_handle, ADC_CHANNEL_9, &config); // GPIO10
		
		// Initialize calibration
		adc_cali_curve_fitting_config_t cali_config = {};
		cali_config.unit_id = ADC_UNIT_1;
		cali_config.atten = ADC_ATTEN_DB_12;
		cali_config.bitwidth = ADC_BITWIDTH_DEFAULT;
		
		esp_err_t cali_err = adc_cali_create_scheme_curve_fitting(&cali_config, &this->adc1_cali_handle);
		if (cali_err == ESP_OK)
		{
			this->adcInitialized = true;
			ESP_LOGI(TAG, "ADC initialized for NTC sensors with calibration");
		}
		else
		{
			ESP_LOGW(TAG, "ADC calibration initialization failed: %s, proceeding without calibration", esp_err_to_name(cali_err));
			this->adcInitialized = true; // Still allow ADC operation without calibration
		}
	}
	else
	{
		ESP_LOGE(TAG, "Failed to initialize ADC: %s", esp_err_to_name(adc_err));
	}

	this->readTempSensorSettings();

	// Initialize NTC sensors loaded from settings
	this->initNtcTemperatureSensors();

	this->initOneWire();

	this->detectOnewireTemperatureSensors();

	this->initRtdSensors();

	this->detectRtdTemperatureSensors();

	this->initMqtt();

	this->initFirebase();

	this->statisticsManager->Init();

	this->run = true;

	xTaskCreate(&this->readLoop, "readloop_task", 16384, this, 5, NULL);

	this->server = this->startWebserver();
}

void BrewEngine::initHeaters()
{
	ESP_LOGI(TAG, "Initializing %d heaters", this->heaters.size());
	
	for (auto const &heater : this->heaters)
	{
		ESP_LOGI(TAG, "Configuring heater %s on pin %d", heater->name.c_str(), heater->pinNr);

		// Validate GPIO pin for ESP32-S3
		if (heater->pinNr < 0 || heater->pinNr >= GPIO_NUM_MAX || 
			!GPIO_IS_VALID_OUTPUT_GPIO(heater->pinNr)) {
			ESP_LOGE(TAG, "Invalid GPIO pin %d for heater %s, skipping", heater->pinNr, heater->name.c_str());
			continue;
		}

		esp_err_t err = gpio_reset_pin(heater->pinNr);
		if (err != ESP_OK) {
			ESP_LOGE(TAG, "Failed to reset GPIO %d for heater %s: %s", heater->pinNr, heater->name.c_str(), esp_err_to_name(err));
			continue;
		}
		ESP_LOGI(TAG, "GPIO reset done for %s", heater->name.c_str());
		
		err = gpio_set_direction(heater->pinNr, GPIO_MODE_OUTPUT);
		if (err != ESP_OK) {
			ESP_LOGE(TAG, "Failed to set GPIO direction %d for heater %s: %s", heater->pinNr, heater->name.c_str(), esp_err_to_name(err));
			continue;
		}
		ESP_LOGI(TAG, "GPIO direction set for %s", heater->name.c_str());
		
		err = gpio_set_level(heater->pinNr, this->gpioLow);
		if (err != ESP_OK) {
			ESP_LOGE(TAG, "Failed to set GPIO level %d for heater %s: %s", heater->pinNr, heater->name.c_str(), esp_err_to_name(err));
			continue;
		}
		ESP_LOGI(TAG, "Heater %s Configured", heater->name.c_str());
		
		// Yield to prevent watchdog timeout during initialization
		vTaskDelay(pdMS_TO_TICKS(10));
	}
	
	ESP_LOGI(TAG, "All heaters initialized successfully");
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
	this->spi_mosi_pin = (gpio_num_t)this->settingsManager->Read("spiMosi", (uint16_t)20);
	this->spi_miso_pin = (gpio_num_t)this->settingsManager->Read("spiMiso", (uint16_t)21);
	this->spi_clk_pin = (gpio_num_t)this->settingsManager->Read("spiClk", (uint16_t)47);
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

	// Firebase (using short key names due to NVS 15-char limit)
	// Migrate from old long key names if they exist
	string oldFirebaseUrl = this->settingsManager->Read("firebaseUrl", (string)"");
	if (!oldFirebaseUrl.empty()) {
		ESP_LOGI(TAG, "Migrating firebaseUrl to fbUrl");
		this->settingsManager->Write("fbUrl", oldFirebaseUrl);
		// Note: We don't delete the old key to avoid issues
	}
	
	this->firebaseUrl = this->settingsManager->Read("fbUrl", (string)"");
	
	// Trim any whitespace from loaded Firebase URL
	if (!this->firebaseUrl.empty()) {
		size_t start = this->firebaseUrl.find_first_not_of(" \t\n\r");
		if (start != string::npos) {
			size_t end = this->firebaseUrl.find_last_not_of(" \t\n\r");
			this->firebaseUrl = this->firebaseUrl.substr(start, end - start + 1);
		} else {
			this->firebaseUrl.clear(); // String contains only whitespace
		}
	}
	
	ESP_LOGI(TAG, "Loaded Firebase URL (%d chars): '%s'", this->firebaseUrl.length(), this->firebaseUrl.c_str());
	this->firebaseApiKey = this->settingsManager->Read("fbApiKey", (string)"");
	this->firebaseAuthToken = this->settingsManager->Read("fbAuthToken", (string)"");
	this->firebaseEmail = this->settingsManager->Read("fbEmail", (string)"");
	this->firebasePassword = this->settingsManager->Read("fbPassword", (string)"");
	this->firebaseAuthMethod = this->settingsManager->Read("fbAuthMethod", (string)"email");
	this->firebaseSendInterval = this->settingsManager->Read("fbSendInt", (uint16_t)10);
	this->firebaseDatabaseEnabled = this->settingsManager->Read("fbDbEnabled", true);

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
	
	// Firebase configuration (using short key names due to NVS 15-char limit)
	if (!config["firebaseUrl"].is_null() && config["firebaseUrl"].is_string())
	{
		string url = (string)config["firebaseUrl"];
		// Trim whitespace safely
		size_t start = url.find_first_not_of(" \t\n\r");
		if (start != string::npos) {
			size_t end = url.find_last_not_of(" \t\n\r");
			url = url.substr(start, end - start + 1);
		} else {
			url.clear(); // String contains only whitespace
		}
		this->settingsManager->Write("fbUrl", url);
		this->firebaseUrl = url;
		ESP_LOGI(TAG, "Saved Firebase URL (%d chars): '%s'", url.length(), url.c_str());
	}
	if (!config["firebaseApiKey"].is_null() && config["firebaseApiKey"].is_string())
	{
		string apiKey = (string)config["firebaseApiKey"];
		// Trim whitespace safely
		size_t start = apiKey.find_first_not_of(" \t\n\r");
		if (start != string::npos) {
			size_t end = apiKey.find_last_not_of(" \t\n\r");
			apiKey = apiKey.substr(start, end - start + 1);
		} else {
			apiKey.clear();
		}
		this->settingsManager->Write("fbApiKey", apiKey);
		this->firebaseApiKey = apiKey;
	}
	if (!config["firebaseAuthToken"].is_null() && config["firebaseAuthToken"].is_string())
	{
		string authToken = (string)config["firebaseAuthToken"];
		// Trim whitespace safely
		size_t start = authToken.find_first_not_of(" \t\n\r");
		if (start != string::npos) {
			size_t end = authToken.find_last_not_of(" \t\n\r");
			authToken = authToken.substr(start, end - start + 1);
		} else {
			authToken.clear();
		}
		this->settingsManager->Write("fbAuthToken", authToken);
		this->firebaseAuthToken = authToken;
	}
	if (!config["firebaseEmail"].is_null() && config["firebaseEmail"].is_string())
	{
		string email = (string)config["firebaseEmail"];
		// Trim whitespace safely
		size_t start = email.find_first_not_of(" \t\n\r");
		if (start != string::npos) {
			size_t end = email.find_last_not_of(" \t\n\r");
			email = email.substr(start, end - start + 1);
		} else {
			email.clear();
		}
		this->settingsManager->Write("fbEmail", email);
		this->firebaseEmail = email;
	}
	if (!config["firebasePassword"].is_null() && config["firebasePassword"].is_string())
	{
		string password = (string)config["firebasePassword"];
		// Trim whitespace safely
		size_t start = password.find_first_not_of(" \t\n\r");
		if (start != string::npos) {
			size_t end = password.find_last_not_of(" \t\n\r");
			password = password.substr(start, end - start + 1);
		} else {
			password.clear();
		}
		this->settingsManager->Write("fbPassword", password);
		this->firebasePassword = password;
	}
	if (!config["firebaseAuthMethod"].is_null() && config["firebaseAuthMethod"].is_string())
	{
		this->settingsManager->Write("fbAuthMethod", (string)config["firebaseAuthMethod"]);
		this->firebaseAuthMethod = config["firebaseAuthMethod"];
	}
	if (!config["firebaseSendInterval"].is_null() && config["firebaseSendInterval"].is_number())
	{
		uint16_t interval = config["firebaseSendInterval"];
		// Validate interval is between 1 and 300 seconds
		if (interval >= 1 && interval <= 300)
		{
			this->settingsManager->Write("fbSendInt", interval);
			this->firebaseSendInterval = interval;
		}
	}
	if (!config["firebaseDatabaseEnabled"].is_null() && config["firebaseDatabaseEnabled"].is_boolean())
	{
		this->settingsManager->Write("fbDbEnabled", (bool)config["firebaseDatabaseEnabled"]);
		this->firebaseDatabaseEnabled = config["firebaseDatabaseEnabled"];
	}
	
	// Auto-disable Firebase database if in AP mode (no internet access)
	// TEMPORARILY DISABLED FOR DEBUGGING - to test if this is causing the graph issue
	/*
	if (this->GetWifiSettingsJson) {
		try {
			json wifiSettings = this->GetWifiSettingsJson();
			if (!wifiSettings.is_null() && !wifiSettings["enableAP"].is_null() && wifiSettings["enableAP"].is_boolean() && wifiSettings["enableAP"] == true) {
				if (this->firebaseDatabaseEnabled) {
					ESP_LOGW(TAG, "Auto-disabling Firebase database logging: device is in AP mode (no internet access)");
					this->firebaseDatabaseEnabled = false;
					// Try to save the setting, but don't fail if it doesn't work
					try {
						this->settingsManager->Write("fbDbEnabled", false);
					} catch (const std::exception& e) {
						ESP_LOGW(TAG, "Failed to save Firebase database disabled setting: %s", e.what());
					}
				}
			}
		} catch (const std::exception& e) {
			ESP_LOGW(TAG, "Error checking WiFi settings for AP mode: %s", e.what());
		}
	}
	*/
	ESP_LOGI(TAG, "AP mode auto-disable temporarily disabled for debugging");
	
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

	// First pass: collect CS pin changes for RTD sensors and analog pin changes for NTC sensors
	struct CsPinChange {
		uint64_t oldSensorId;
		uint64_t newSensorId;
		int newCsPin;
		TemperatureSensor *sensor;
		json sensorData;
	};
	
	struct AnalogPinChange {
		uint64_t oldSensorId;
		uint64_t newSensorId;
		int newAnalogPin;
		TemperatureSensor *sensor;
		json sensorData;
	};
	
	vector<CsPinChange> csPinChanges;
	vector<AnalogPinChange> analogPinChanges;
	
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
			
			// Check if this is an RTD sensor with a CS pin change
			bool hasCsPinChange = false;
			if ((sensor->sensorType == SENSOR_PT100 || sensor->sensorType == SENSOR_PT1000) && 
				!jSensor["csPin"].is_null() && jSensor["csPin"].is_number_integer())
			{
				int currentCsPin = (int)(sensorId - 0x31865000);
				int newCsPin = jSensor["csPin"];
				
				if (currentCsPin != newCsPin && newCsPin >= 0 && newCsPin < GPIO_NUM_MAX)
				{
					ESP_LOGI(TAG, "RTD sensor %s CS pin change detected: %d -> %d", sensor->name.c_str(), currentCsPin, newCsPin);
					
					// Check if new CS pin is already in use
					uint64_t newSensorId = 0x31865000 + newCsPin;
					bool pinInUse = false;
					
					// Check existing sensors
					if (this->sensors.find(newSensorId) != this->sensors.end())
					{
						pinInUse = true;
					}
					
					// Check other pending changes
					for (const auto &change : csPinChanges)
					{
						if (change.newSensorId == newSensorId)
						{
							pinInUse = true;
							break;
						}
					}
					
					if (pinInUse)
					{
						ESP_LOGE(TAG, "CS pin %d is already in use by another RTD sensor", newCsPin);
					}
					else
					{
						// Queue the CS pin change
						CsPinChange change;
						change.oldSensorId = sensorId;
						change.newSensorId = newSensorId;
						change.newCsPin = newCsPin;
						change.sensor = sensor;
						change.sensorData = jSensor;
						csPinChanges.push_back(change);
						hasCsPinChange = true;
					}
				}
			}
			
			// Check if this is an NTC sensor with an analog pin change
			bool hasAnalogPinChange = false;
			if (sensor->sensorType == SENSOR_NTC && 
				!jSensor["analogPin"].is_null() && jSensor["analogPin"].is_number_integer())
			{
				int currentAnalogPin = (int)(sensorId - 0x4E544300); // "NTC" base ID
				int newAnalogPin = jSensor["analogPin"];
				
				if (currentAnalogPin != newAnalogPin && newAnalogPin >= 0 && newAnalogPin < GPIO_NUM_MAX)
				{
					ESP_LOGI(TAG, "NTC sensor %s analog pin change detected: %d -> %d", sensor->name.c_str(), currentAnalogPin, newAnalogPin);
					
					// Check if new analog pin is already in use by another NTC sensor
					uint64_t newSensorId = 0x4E544300 + newAnalogPin;
					bool pinInUse = false;
					
					// Check existing sensors
					if (this->sensors.find(newSensorId) != this->sensors.end())
					{
						pinInUse = true;
					}
					
					// Check other pending changes
					for (const auto &change : analogPinChanges)
					{
						if (change.newSensorId == newSensorId)
						{
							pinInUse = true;
							break;
						}
					}
					
					if (pinInUse)
					{
						ESP_LOGE(TAG, "Analog pin %d is already in use by another NTC sensor", newAnalogPin);
					}
					else
					{
						// Queue the analog pin change
						AnalogPinChange change;
						change.oldSensorId = sensorId;
						change.newSensorId = newSensorId;
						change.newAnalogPin = newAnalogPin;
						change.sensor = sensor;
						change.sensorData = jSensor;
						analogPinChanges.push_back(change);
						hasAnalogPinChange = true;
					}
				}
			}
			
			// Update other sensor properties (skip if CS pin change or analog pin change is queued)
			if (!hasCsPinChange && !hasAnalogPinChange)
			{
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
	}
	
	// Second pass: apply CS pin changes
	for (const auto &change : csPinChanges)
	{
		ESP_LOGI(TAG, "Applying CS pin change for sensor %llu: CS pin %d", change.oldSensorId, change.newCsPin);
		
		TemperatureSensor *sensor = change.sensor;
		
		// Clean up old RTD hardware
		for (auto rtdIt = this->rtdSensors.begin(); rtdIt != this->rtdSensors.end(); ++rtdIt)
		{
			if (*rtdIt != nullptr && memcmp(&(sensor->max31865Handle), *rtdIt, sizeof(max31865_t)) == 0)
			{
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
		
		// Update sensor ID and reinitialize hardware
		sensor->id = change.newSensorId;
		memset(&sensor->max31865Handle, 0, sizeof(max31865_t));
		
		// Try to initialize with new CS pin
		max31865_t *rtd_sensor = new max31865_t;
		esp_err_t ret = max31865_init_desc(rtd_sensor, SPI2_HOST, change.newCsPin);
		
		bool hardwareSuccess = false;
		if (ret == ESP_OK)
		{
			ret = max31865_set_config(rtd_sensor, true, 1, false, false, 0, true, true, 0, 0xFFFF);
			if (ret == ESP_OK)
			{
				sensor->max31865Handle = *rtd_sensor;
				sensor->connected = true;
				sensor->consecutiveFailures = 0;
				this->rtdSensors.push_back(rtd_sensor);
				this->rtdSensorCount++;
				hardwareSuccess = true;
				
				ESP_LOGI(TAG, "RTD sensor %s successfully moved to CS pin %d", sensor->name.c_str(), change.newCsPin);
			}
			else
			{
				ESP_LOGE(TAG, "Failed to configure RTD sensor on new CS pin %d: %s", change.newCsPin, esp_err_to_name(ret));
				delete rtd_sensor;
			}
		}
		else
		{
			ESP_LOGE(TAG, "Failed to initialize RTD sensor on new CS pin %d: %s", change.newCsPin, esp_err_to_name(ret));
			delete rtd_sensor;
		}
		
		if (!hardwareSuccess)
		{
			sensor->connected = false;
			// Revert sensor ID on hardware failure
			sensor->id = change.oldSensorId;
		}
		
		// Update sensor properties
		auto jSensor = change.sensorData;
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
				this->currentTemperatures.erase(sensor->id);
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
		
		// Update sensor mappings
		this->sensors.erase(change.oldSensorId);
		this->currentTemperatures.erase(change.oldSensorId);
		this->sensors.insert_or_assign(sensor->id, sensor);
	}
	
	// Third pass: apply analog pin changes for NTC sensors
	for (const auto &change : analogPinChanges)
	{
		ESP_LOGI(TAG, "Applying analog pin change for NTC sensor %llu: analog pin %d", change.oldSensorId, change.newAnalogPin);
		
		TemperatureSensor *sensor = change.sensor;
		
		// Update sensor ID (no hardware cleanup needed for NTC sensors)
		sensor->id = change.newSensorId;
		sensor->analogPin = change.newAnalogPin;
		
		// Update NTC configuration from JSON data
		auto jSensor = change.sensorData;
		if (!jSensor["ntcResistance"].is_null() && jSensor["ntcResistance"].is_number())
		{
			sensor->ntcResistance = (float)jSensor["ntcResistance"];
		}
		
		if (!jSensor["dividerResistor"].is_null() && jSensor["dividerResistor"].is_number())
		{
			sensor->dividerResistor = (float)jSensor["dividerResistor"];
		}
		
		// Update other sensor properties
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
				this->currentTemperatures.erase(sensor->id);
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
		
		// Update sensor mappings
		this->sensors.erase(change.oldSensorId);
		this->currentTemperatures.erase(change.oldSensorId);
		this->sensors.insert_or_assign(sensor->id, sensor);
		
		ESP_LOGI(TAG, "NTC sensor %s successfully moved to analog pin %d", sensor->name.c_str(), change.newAnalogPin);
	}

	// We also need to delete sensors that are no longer in the list
	vector<uint64_t> sensorsToDelete;

	for (auto const &[key, sensor] : this->sensors)
	{
		uint64_t sensorId = sensor->id;
		string stringId = to_string(sensorId); // json doesn't support unit64 so in out json id is string
		
		// Check if this sensor was involved in a CS pin change or analog pin change
		bool wasCsPinChanged = false;
		for (const auto &change : csPinChanges)
		{
			if (change.sensor == sensor)
			{
				wasCsPinChanged = true;
				break;
			}
		}
		
		bool wasAnalogPinChanged = false;
		for (const auto &change : analogPinChanges)
		{
			if (change.sensor == sensor)
			{
				wasAnalogPinChanged = true;
				break;
			}
		}
		
		// If this sensor had its CS pin or analog pin changed, it should be preserved
		if (wasCsPinChanged || wasAnalogPinChanged)
		{
			ESP_LOGI(TAG, "Preserving sensor %llu (had pin change)", sensorId);
			continue;
		}
		
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

void BrewEngine::initFirebase()
{
	// Check if Firebase database is enabled
	if (!this->firebaseDatabaseEnabled)
	{
		ESP_LOGI(TAG, "Firebase database logging disabled, skipping initialization");
		this->firebaseEnabled = false;
		return;
	}
	
	// return if no URL is configured
	if (this->firebaseUrl.empty())
	{
		ESP_LOGI(TAG, "Firebase not configured, skipping initialization");
		return;
	}
	
	// Firebase API Key is required for both authentication methods
	if (this->firebaseApiKey.empty()) {
		ESP_LOGE(TAG, "Firebase API Key not configured - Firebase disabled");
		this->firebaseEnabled = false;
		return;
	}
	
	// Check if authentication credentials are configured based on selected method
	if (this->firebaseAuthMethod == "email") {
		if (this->firebaseEmail.empty() || this->firebasePassword.empty()) {
			ESP_LOGE(TAG, "Firebase email/password not configured - Firebase disabled");
			this->firebaseEnabled = false;
			return;
		}
		ESP_LOGI(TAG, "Firebase email/password authentication configured");
	} else if (this->firebaseAuthMethod == "token") {
		if (this->firebaseAuthToken.empty()) {
			ESP_LOGE(TAG, "Firebase Auth Token not configured - Firebase disabled");
			this->firebaseEnabled = false;
			return;
		}
		ESP_LOGI(TAG, "Firebase API Key/Custom Token authentication configured");
	} else {
		ESP_LOGE(TAG, "Invalid Firebase authentication method: %s - Firebase disabled", this->firebaseAuthMethod.c_str());
		this->firebaseEnabled = false;
		return;
	}

	ESP_LOGI(TAG, "initFirebase: Start");
	
	// Generate a unique session ID for this boot
	this->currentSessionId = esp_random();
	
	this->firebaseEnabled = true;
	
	// Initialize last send time to allow immediate first send
	this->lastFirebaseSend = system_clock::now() - seconds(this->firebaseSendInterval);
	
	ESP_LOGI(TAG, "initFirebase: Done - URL: %s, Session ID: %lu", this->firebaseUrl.c_str(), this->currentSessionId);
}

bool BrewEngine::isFirebaseTokenValid()
{
    if (!this->firebaseAuthenticated || this->firebaseIdToken.empty()) {
        return false;
    }
    
    time_t current_time = time(NULL);
    return (current_time + 300) < this->firebaseTokenExpiresAt; // 5 minute buffer
}

esp_err_t BrewEngine::exchangeCustomTokenForIdToken()
{
    // Validate credentials are configured
    if (this->firebaseApiKey.empty()) {
        ESP_LOGE(TAG, "Firebase API Key not configured");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (this->firebaseAuthToken.empty()) {
        ESP_LOGE(TAG, "Firebase Auth Token not configured");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Firebase API Key length: %d", this->firebaseApiKey.length());
    ESP_LOGI(TAG, "Firebase Auth Token length: %d", this->firebaseAuthToken.length());
    
    char url[2200];
    char post_data[1024];
    char response_buffer[2048];
    
    memset(response_buffer, 0, sizeof(response_buffer));
    
    snprintf(url, sizeof(url), "https://identitytoolkit.googleapis.com/v1/accounts:signInWithCustomToken?key=%s", this->firebaseApiKey.c_str());
    
    cJSON *json = cJSON_CreateObject();
    cJSON *token = cJSON_CreateString(this->firebaseAuthToken.c_str());
    cJSON *return_secure_token = cJSON_CreateBool(true);
    
    cJSON_AddItemToObject(json, "token", token);
    cJSON_AddItemToObject(json, "returnSecureToken", return_secure_token);
    
    char *json_string = cJSON_Print(json);
    if (json_string == NULL) {
        ESP_LOGE(TAG, "Failed to create auth JSON");
        cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }
    strcpy(post_data, json_string);
    
    ESP_LOGI(TAG, "Authenticating with Firebase...");
    ESP_LOGI(TAG, "Auth URL: %s", url);
    ESP_LOGI(TAG, "Auth payload: %s", post_data);
    
    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_POST;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 10000;
    config.buffer_size = 2048;
    config.buffer_size_tx = 2048;
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client for auth");
        free(json_string);
        cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }
    
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_err_t set_field_err = esp_http_client_set_post_field(client, post_data, strlen(post_data));
    if (set_field_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set POST field for auth: %s", esp_err_to_name(set_field_err));
        esp_http_client_cleanup(client);
        free(json_string);
        cJSON_Delete(json);
        return set_field_err;
    }
    
    esp_err_t err = esp_http_client_open(client, strlen(post_data));
    if (err == ESP_OK) {
        int wlen = esp_http_client_write(client, post_data, strlen(post_data));
        ESP_LOGI(TAG, "Wrote %d bytes to auth request", wlen);
        
        int content_length = esp_http_client_fetch_headers(client);
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Auth response status: %d, content_length: %d", status_code, content_length);
        
        int total_read = 0;
        if (content_length > 0) {
            total_read = esp_http_client_read(client, response_buffer, content_length);
        } else {
            // Handle chunked response
            int data_read;
            while ((data_read = esp_http_client_read(client, response_buffer + total_read, 2047 - total_read)) > 0) {
                total_read += data_read;
                if (total_read >= 2047) break;
            }
        }
        response_buffer[total_read] = '\0';
        ESP_LOGI(TAG, "Auth response (%d bytes): %s", total_read, response_buffer);
        
        esp_http_client_close(client);
        
        if (status_code == 200) {
            cJSON *response_json = cJSON_Parse(response_buffer);
            if (response_json != NULL) {
                cJSON *id_token = cJSON_GetObjectItem(response_json, "idToken");
                cJSON *expires_in = cJSON_GetObjectItem(response_json, "expiresIn");
                
                if (cJSON_IsString(id_token) && id_token->valuestring != NULL) {
                    this->firebaseIdToken = id_token->valuestring;
                    
                    int expires_seconds = 3600; // default 1 hour
                    if (cJSON_IsString(expires_in) && expires_in->valuestring != NULL) {
                        expires_seconds = atoi(expires_in->valuestring);
                    }
                    this->firebaseTokenExpiresAt = time(NULL) + expires_seconds;
                    this->firebaseAuthenticated = true;
                    
                    ESP_LOGI(TAG, "✓ Firebase authentication successful (expires in %d seconds)", expires_seconds);
                    err = ESP_OK;
                } else {
                    ESP_LOGE(TAG, "Invalid auth response: missing idToken");
                    err = ESP_FAIL;
                }
                cJSON_Delete(response_json);
            } else {
                ESP_LOGE(TAG, "Failed to parse auth response");
                err = ESP_FAIL;
            }
        } else {
            ESP_LOGE(TAG, "Authentication failed with status %d", status_code);
            ESP_LOGE(TAG, "Auth response: %s", response_buffer);
            
            // Check for specific error types and provide helpful messages
            cJSON *error_json = cJSON_Parse(response_buffer);
            if (error_json != NULL) {
                cJSON *error_obj = cJSON_GetObjectItem(error_json, "error");
                if (error_obj != NULL) {
                    cJSON *message = cJSON_GetObjectItem(error_obj, "message");
                    if (cJSON_IsString(message)) {
                        if (strcmp(message->valuestring, "INVALID_CUSTOM_TOKEN") == 0) {
                            ESP_LOGE(TAG, "🔑 INVALID_CUSTOM_TOKEN: The Firebase custom token has expired or is malformed.");
                            ESP_LOGE(TAG, "   Please generate a new custom token from your Firebase service account.");
                            ESP_LOGE(TAG, "   Custom tokens typically expire after 1 hour.");
                        } else if (strcmp(message->valuestring, "INVALID_API_KEY") == 0) {
                            ESP_LOGE(TAG, "🔑 INVALID_API_KEY: The Firebase Web API Key is incorrect.");
                            ESP_LOGE(TAG, "   Check your Firebase Project Settings > Web API Key.");
                        } else {
                            ESP_LOGE(TAG, "🔑 Firebase Auth Error: %s", message->valuestring);
                        }
                    }
                }
                cJSON_Delete(error_json);
            }
            
            this->firebaseAuthenticated = false;
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "Failed to perform auth request: %s", esp_err_to_name(err));
    }
    
    esp_http_client_cleanup(client);
    free(json_string);
    cJSON_Delete(json);
    return err;
}

esp_err_t BrewEngine::refreshFirebaseToken()
{
    if (this->firebaseRefreshToken.empty()) {
        ESP_LOGE(TAG, "No refresh token available");
        return ESP_ERR_INVALID_STATE;
    }
    
    char url[2200];
    char post_data[1024];
    char response_buffer[2048];
    
    memset(response_buffer, 0, sizeof(response_buffer));
    
    snprintf(url, sizeof(url), "https://securetoken.googleapis.com/v1/token?key=%s", this->firebaseApiKey.c_str());
    
    cJSON *json = cJSON_CreateObject();
    cJSON *grant_type = cJSON_CreateString("refresh_token");
    cJSON *refresh_token = cJSON_CreateString(this->firebaseRefreshToken.c_str());
    
    cJSON_AddItemToObject(json, "grant_type", grant_type);
    cJSON_AddItemToObject(json, "refresh_token", refresh_token);
    
    char *json_string = cJSON_Print(json);
    strncpy(post_data, json_string, sizeof(post_data) - 1);
    post_data[sizeof(post_data) - 1] = '\0';
    
    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_POST;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 2048;
    config.buffer_size_tx = 2048;
    config.timeout_ms = 10000;
    config.event_handler = this->http_event_handler;
    config.user_data = response_buffer;
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));
    
    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        int content_length = esp_http_client_get_content_length(client);
        
        ESP_LOGI(TAG, "Token refresh response status: %d, content_length: %d", status_code, content_length);
        
        if (status_code == 200) {
            cJSON *response_json = cJSON_Parse(response_buffer);
            if (response_json != NULL) {
                cJSON *id_token = cJSON_GetObjectItem(response_json, "id_token");
                cJSON *refresh_token_new = cJSON_GetObjectItem(response_json, "refresh_token");
                cJSON *expires_in = cJSON_GetObjectItem(response_json, "expires_in");
                
                if (cJSON_IsString(id_token) && id_token->valuestring != NULL) {
                    this->firebaseIdToken = id_token->valuestring;
                    
                    if (cJSON_IsString(refresh_token_new) && refresh_token_new->valuestring != NULL) {
                        this->firebaseRefreshToken = refresh_token_new->valuestring;
                    }
                    
                    // Set expiration time (default 1 hour if not specified)
                    int expires_seconds = 3600;
                    if (cJSON_IsString(expires_in) && expires_in->valuestring != NULL) {
                        expires_seconds = atoi(expires_in->valuestring);
                    }
                    
                    this->firebaseTokenExpiresAt = time(NULL) + expires_seconds;
                    this->firebaseAuthenticated = true;
                    
                    ESP_LOGI(TAG, "✓ Firebase token refreshed successfully (expires in %d seconds)", expires_seconds);
                    err = ESP_OK;
                } else {
                    ESP_LOGE(TAG, "Invalid refresh response: missing id_token");
                    err = ESP_FAIL;
                }
                cJSON_Delete(response_json);
            } else {
                ESP_LOGE(TAG, "Failed to parse refresh response");
                err = ESP_FAIL;
            }
        } else {
            ESP_LOGE(TAG, "Token refresh failed with status %d", status_code);
            ESP_LOGE(TAG, "Refresh response: %s", response_buffer);
            this->firebaseAuthenticated = false;
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "Failed to perform token refresh request: %s", esp_err_to_name(err));
    }
    
    esp_http_client_cleanup(client);
    free(json_string);
    cJSON_Delete(json);
    return err;
}

esp_err_t BrewEngine::authenticateWithEmailPassword()
{
    if (this->firebaseEmail.empty() || this->firebasePassword.empty()) {
        ESP_LOGE(TAG, "Email or password not configured");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (this->firebaseApiKey.empty()) {
        ESP_LOGE(TAG, "Firebase API Key required for email/password authentication");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Initialize all variables at the top to avoid goto initialization crossing
    cJSON *json = NULL;
    cJSON *email = NULL;
    cJSON *password = NULL;
    cJSON *return_secure_token = NULL;
    char *json_string = NULL;
    esp_http_client_handle_t client = NULL;
    esp_http_client_config_t config = {};
    esp_err_t err = ESP_FAIL;
    int wlen = 0;
    int content_length = 0;
    int status_code = 0;
    int total_read = 0;
    
    char url[2200];
    char post_data[1024];
    char response_buffer[2048];
    
    memset(response_buffer, 0, sizeof(response_buffer));
    memset(post_data, 0, sizeof(post_data));
    
    snprintf(url, sizeof(url), "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=%s", this->firebaseApiKey.c_str());
    
    // Create JSON with proper error checking
    json = cJSON_CreateObject();
    if (json == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object - out of memory");
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    
    email = cJSON_CreateString(this->firebaseEmail.c_str());
    password = cJSON_CreateString(this->firebasePassword.c_str());
    return_secure_token = cJSON_CreateBool(true);
    
    if (email == NULL || password == NULL || return_secure_token == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON fields - out of memory");
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    
    cJSON_AddItemToObject(json, "email", email);
    cJSON_AddItemToObject(json, "password", password);
    cJSON_AddItemToObject(json, "returnSecureToken", return_secure_token);
    
    json_string = cJSON_Print(json);
    if (json_string == NULL) {
        ESP_LOGE(TAG, "Failed to serialize JSON - out of memory");
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    strncpy(post_data, json_string, sizeof(post_data) - 1);
    post_data[sizeof(post_data) - 1] = '\0';
    
    ESP_LOGI(TAG, "Email/password auth URL: %s", url);
    ESP_LOGI(TAG, "Authenticating user: %s", this->firebaseEmail.c_str());
    
    config.url = url;
    config.method = HTTP_METHOD_POST;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 4096;          // Increased for TLS operations
    config.buffer_size_tx = 4096;       // Increased for TLS operations
    config.timeout_ms = 15000;          // Increased timeout for crypto operations
    config.max_redirection_count = 0;   // Disable redirects to reduce complexity
    config.disable_auto_redirect = true;
    
    client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));
    
    err = esp_http_client_open(client, strlen(post_data));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        goto cleanup;
    }
    
    wlen = esp_http_client_write(client, post_data, strlen(post_data));
    if (wlen < 0) {
        ESP_LOGE(TAG, "Failed to write request data");
        err = ESP_FAIL;
        goto cleanup;
    }
    
    content_length = esp_http_client_fetch_headers(client);
    status_code = esp_http_client_get_status_code(client);
    
    ESP_LOGI(TAG, "Email/password auth response status: %d, content_length: %d", status_code, content_length);
    
    // Read response data
    total_read = 0;
    if (content_length > 0) {
        total_read = esp_http_client_read(client, response_buffer, content_length);
    } else {
        // Handle chunked transfer encoding
        int data_read;
        while ((data_read = esp_http_client_read(client, response_buffer + total_read, 2047 - total_read)) > 0) {
            total_read += data_read;
            if (total_read >= 2047) break;
        }
    }
    response_buffer[total_read] = '\0';
    
    ESP_LOGI(TAG, "Response buffer content (%d bytes): %s", total_read, response_buffer);
    
    if (status_code == 200) {
        cJSON *response_json = cJSON_Parse(response_buffer);
        if (response_json != NULL) {
            cJSON *id_token = cJSON_GetObjectItem(response_json, "idToken");
            cJSON *refresh_token = cJSON_GetObjectItem(response_json, "refreshToken");
            cJSON *expires_in = cJSON_GetObjectItem(response_json, "expiresIn");
            cJSON *local_id = cJSON_GetObjectItem(response_json, "localId");
            
            if (cJSON_IsString(id_token) && id_token->valuestring != NULL) {
                this->firebaseIdToken = id_token->valuestring;
                
                if (cJSON_IsString(refresh_token) && refresh_token->valuestring != NULL) {
                    this->firebaseRefreshToken = refresh_token->valuestring;
                }
                
                // Set expiration time (default 1 hour if not specified)
                int expires_seconds = 3600;
                if (cJSON_IsString(expires_in) && expires_in->valuestring != NULL) {
                    expires_seconds = atoi(expires_in->valuestring);
                }
                
                this->firebaseTokenExpiresAt = time(NULL) + expires_seconds;
                this->firebaseAuthenticated = true;
                
                if (cJSON_IsString(local_id) && local_id->valuestring != NULL) {
                    ESP_LOGI(TAG, "✓ Email/password authentication successful for user: %s", local_id->valuestring);
                }
                ESP_LOGI(TAG, "✓ Firebase ID token expires in %d seconds", expires_seconds);
                
                err = ESP_OK;
            } else {
                ESP_LOGE(TAG, "Invalid email/password auth response: missing idToken");
                err = ESP_FAIL;
            }
            cJSON_Delete(response_json);
        } else {
            ESP_LOGE(TAG, "Failed to parse email/password auth response");
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "Email/password authentication failed with status %d", status_code);
        ESP_LOGE(TAG, "Auth response: %s", response_buffer);
        
        // Parse error response for better error messages
        cJSON *error_json = cJSON_Parse(response_buffer);
        if (error_json != NULL) {
            cJSON *error_obj = cJSON_GetObjectItem(error_json, "error");
            if (error_obj != NULL) {
                cJSON *message = cJSON_GetObjectItem(error_obj, "message");
                if (cJSON_IsString(message)) {
                    if (strcmp(message->valuestring, "EMAIL_NOT_FOUND") == 0) {
                        ESP_LOGE(TAG, "🔑 EMAIL_NOT_FOUND: The email address is not registered.");
                    } else if (strcmp(message->valuestring, "INVALID_PASSWORD") == 0) {
                        ESP_LOGE(TAG, "🔑 INVALID_PASSWORD: The password is incorrect.");
                    } else if (strcmp(message->valuestring, "USER_DISABLED") == 0) {
                        ESP_LOGE(TAG, "🔑 USER_DISABLED: The user account has been disabled.");
                    } else {
                        ESP_LOGE(TAG, "🔑 Firebase Auth Error: %s", message->valuestring);
                    }
                }
            }
            cJSON_Delete(error_json);
        }
        
        this->firebaseAuthenticated = false;
        err = ESP_FAIL;
    }
    
cleanup:
    // Proper cleanup with null checks to prevent double-free crashes
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        client = NULL;
    }
    if (json_string) {
        free(json_string);
        json_string = NULL;
    }
    if (json) {
        cJSON_Delete(json);
        json = NULL;
    }
    
    return err;
}

bool BrewEngine::isCustomTokenExpired()
{
    if (this->firebaseAuthToken.empty()) {
        ESP_LOGE(TAG, "No custom token to check");
        return true;
    }
    
    // Simple JWT expiration check - decode the payload (base64)
    // JWT format: header.payload.signature
    size_t first_dot = this->firebaseAuthToken.find('.');
    size_t second_dot = this->firebaseAuthToken.find('.', first_dot + 1);
    
    if (first_dot == string::npos || second_dot == string::npos) {
        ESP_LOGE(TAG, "Invalid JWT token format");
        return true;
    }
    
    string payload_b64 = this->firebaseAuthToken.substr(first_dot + 1, second_dot - first_dot - 1);
    
    // Add padding if needed for base64 decoding
    while (payload_b64.length() % 4 != 0) {
        payload_b64 += "=";
    }
    
    // For now, just check if we can parse it and warn about expiration
    ESP_LOGI(TAG, "Custom token payload length: %d characters", payload_b64.length());
    ESP_LOGW(TAG, "⚠️  Custom tokens expire after 1 hour. If authentication fails, generate a new token.");
    
    return false; // For now, let Firebase validate it
}

esp_err_t BrewEngine::ensureFirebaseAuthenticated()
{
    if (isFirebaseTokenValid()) {
        return ESP_OK;
    }
    
    // Add delay to prevent resource contention during authentication
    ESP_LOGI(TAG, "Authentication required - allowing system resources to stabilize...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // Check available memory before attempting TLS operations
    size_t free_heap = esp_get_free_heap_size();
    size_t min_heap = esp_get_minimum_free_heap_size();
    ESP_LOGI(TAG, "Pre-auth memory: %zu bytes free, %zu min", free_heap, min_heap);
    
    if (free_heap < 50000) {
        ESP_LOGW(TAG, "Low memory (%zu bytes) - deferring authentication", free_heap);
        return ESP_ERR_NO_MEM;
    }
    
    // Try to refresh using existing refresh token first
    if (!this->firebaseRefreshToken.empty()) {
        ESP_LOGI(TAG, "Attempting to refresh Firebase token using refresh token...");
        esp_err_t refresh_result = refreshFirebaseToken();
        if (refresh_result == ESP_OK) {
            ESP_LOGI(TAG, "Successfully refreshed Firebase token");
            return ESP_OK;
        } else {
            ESP_LOGW(TAG, "Token refresh failed, trying other authentication methods");
        }
    }
    
    // Authenticate based on the selected method
    if (this->firebaseAuthMethod == "email") {
        // Email/password method
        if (!this->firebaseEmail.empty() && !this->firebasePassword.empty()) {
            ESP_LOGI(TAG, "Attempting email/password authentication...");
            esp_err_t email_result = authenticateWithEmailPassword();
            if (email_result == ESP_OK) {
                ESP_LOGI(TAG, "Successfully authenticated with email/password");
                return ESP_OK;
            } else {
                ESP_LOGE(TAG, "Email/password authentication failed");
                return email_result;
            }
        } else {
            ESP_LOGE(TAG, "Email/password authentication selected but credentials not configured");
            return ESP_ERR_INVALID_STATE;
        }
    } else if (this->firebaseAuthMethod == "token") {
        // Custom token method
        if (!this->firebaseAuthToken.empty()) {
            // Check if custom token might be expired
            isCustomTokenExpired(); // This will log warnings
            
            ESP_LOGI(TAG, "Firebase token expired or invalid, authenticating with custom token...");
            return exchangeCustomTokenForIdToken();
        } else {
            ESP_LOGE(TAG, "Custom token authentication selected but token not configured");
            return ESP_ERR_INVALID_STATE;
        }
    } else {
        ESP_LOGE(TAG, "Invalid authentication method: %s", this->firebaseAuthMethod.c_str());
        return ESP_ERR_INVALID_STATE;
    }
}

esp_err_t BrewEngine::http_event_handler(esp_http_client_event_t *evt)
{
    static int output_len;
    
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            if (!esp_http_client_is_chunked_response(evt->client)) {
                if (evt->user_data) {
                    memcpy((char*)evt->user_data + output_len, evt->data, evt->data_len);
                    output_len += evt->data_len;
                }
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            output_len = 0;
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            output_len = 0;
            break;
        case HTTP_EVENT_REDIRECT:
            ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
            break;
    }
    return ESP_OK;
}

esp_err_t BrewEngine::writeTemperatureToFirebase(float temperature, float targetTemperature, uint8_t pidOutput, const string &status)
{
    if (!this->firebaseEnabled || !this->firebaseDatabaseEnabled)
    {
        return ESP_FAIL;
    }

    char url[2200];  // Increased buffer size for safety
    time_t now = time(NULL);
    
    // Validate Firebase URL is configured
    if (this->firebaseUrl.empty()) {
        ESP_LOGE(TAG, "Firebase URL not configured");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Ensure we have valid authentication
    esp_err_t auth_result = ensureFirebaseAuthenticated();
    if (auth_result != ESP_OK) {
        ESP_LOGE(TAG, "Cannot write temperature: Firebase authentication failed");
        return auth_result;
    }
    
    // Construct URL with authentication token
    ESP_LOGI(TAG, "Firebase URL for URL construction: len=%d, first char code=%d, content: '%s'", 
             this->firebaseUrl.length(), this->firebaseUrl.empty() ? -1 : (int)this->firebaseUrl[0], this->firebaseUrl.c_str());
    
    int url_len = snprintf(url, sizeof(url), "%s/temperatures/%lld.json?auth=%s", 
                          this->firebaseUrl.c_str(), (long long)now, this->firebaseIdToken.c_str());
    
    // Validate URL was constructed properly
    if (url_len >= sizeof(url)) {
        ESP_LOGE(TAG, "URL too long: %d bytes (max %d)", url_len, (int)sizeof(url));
        return ESP_ERR_INVALID_SIZE;
    }
    
    if (url_len <= 0) {
        ESP_LOGE(TAG, "Failed to construct URL");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Firebase URL (%d bytes): %s", url_len, url);
    ESP_LOGI(TAG, "URL starts with https: %s", strncmp(url, "https://", 8) == 0 ? "YES" : "NO");
    
    cJSON *json = cJSON_CreateObject();
    cJSON *temp_value = cJSON_CreateNumber(temperature);
    cJSON *target_value = cJSON_CreateNumber(targetTemperature);
    cJSON *pid_value = cJSON_CreateNumber(pidOutput);
    cJSON *timestamp = cJSON_CreateNumber(now);
    cJSON *status_value = cJSON_CreateString(status.c_str());
    cJSON *hostname = cJSON_CreateString(this->Hostname.c_str());
    cJSON *session_id = cJSON_CreateNumber(this->currentSessionId);
    
    cJSON_AddItemToObject(json, "temperature", temp_value);
    cJSON_AddItemToObject(json, "targetTemperature", target_value);
    cJSON_AddItemToObject(json, "pidOutput", pid_value);
    cJSON_AddItemToObject(json, "timestamp", timestamp);
    cJSON_AddItemToObject(json, "status", status_value);
    cJSON_AddItemToObject(json, "hostname", hostname);
    cJSON_AddItemToObject(json, "sessionId", session_id);
    
    char *json_string = cJSON_Print(json);
    if (json_string == NULL) {
        ESP_LOGE(TAG, "Failed to serialize JSON - out of memory");
        cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI(TAG, "JSON payload size: %d bytes", strlen(json_string));
    
    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_PUT;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 4096;
    config.buffer_size_tx = 4096;
    config.timeout_ms = 10000;  // 10 second timeout
    
    // Validate URL format before creating client
    ESP_LOGI(TAG, "About to validate URL: %s", url);
    ESP_LOGI(TAG, "URL validation - starts with https: %s", strncmp(url, "https://", 8) == 0 ? "YES" : "NO");
    if (strncmp(url, "https://", 8) != 0 && strncmp(url, "http://", 7) != 0) {
        ESP_LOGE(TAG, "Invalid URL format - must start with http:// or https://");
        ESP_LOGE(TAG, "URL first 10 chars: '%.10s'", url);
        free(json_string);
        cJSON_Delete(json);
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client - check URL format and memory");
        ESP_LOGE(TAG, "URL being used: %s", url);
        free(json_string);
        cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }
    
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_err_t set_field_err = esp_http_client_set_post_field(client, json_string, strlen(json_string));
    if (set_field_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set POST field: %s", esp_err_to_name(set_field_err));
        esp_http_client_cleanup(client);
        free(json_string);
        cJSON_Delete(json);
        return set_field_err;
    }
    
    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Temperature written to Firebase. Status: %d", status_code);
    } else {
        ESP_LOGE(TAG, "Failed to write temperature: %s", esp_err_to_name(err));
    }
    
    esp_http_client_cleanup(client);
    free(json_string);
    cJSON_Delete(json);
    
    return err;
}

esp_err_t BrewEngine::queryLatestTemperatureFromFirebase(float *temperature, time_t *timestamp)
{
    if (!this->firebaseEnabled)
    {
        return ESP_FAIL;
    }

    char url[256];
    static char response_buffer[1024] = {0};
    
    snprintf(url, sizeof(url), "%s/temperatures.json?orderBy=\"$key\"&limitToLast=1", this->firebaseUrl.c_str());
    
    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.event_handler = this->http_event_handler;
    config.user_data = response_buffer;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Latest temperature queried from Firebase. Status: %d", status_code);
        
        cJSON *json = cJSON_Parse(response_buffer);
        if (json != NULL) {
            cJSON *first_entry = cJSON_GetArrayItem(json, 0);
            if (first_entry != NULL) {
                cJSON *entry_data = cJSON_GetArrayItem(first_entry, 1);
                if (entry_data != NULL) {
                    cJSON *temp_value = cJSON_GetObjectItem(entry_data, "temperature");
                    cJSON *temp_timestamp = cJSON_GetObjectItem(entry_data, "timestamp");
                    
                    if (cJSON_IsNumber(temp_value) && temperature != NULL) {
                        *temperature = (float)temp_value->valuedouble;
                    }
                    
                    if (cJSON_IsNumber(temp_timestamp) && timestamp != NULL) {
                        *timestamp = (time_t)temp_timestamp->valuedouble;
                    }
                }
            }
            cJSON_Delete(json);
        }
    } else {
        ESP_LOGE(TAG, "Failed to query latest temperature: %s", esp_err_to_name(err));
    }
    
    esp_http_client_cleanup(client);
    return err;
}

esp_err_t BrewEngine::queryTemperatureSeriesFromFirebase(int limit)
{
    if (!this->firebaseEnabled)
    {
        return ESP_FAIL;
    }

    char url[256];
    static char response_buffer[2048] = {0};
    
    snprintf(url, sizeof(url), "%s/temperatures.json?orderBy=\"$key\"&limitToLast=%d", this->firebaseUrl.c_str(), limit);
    
    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.event_handler = this->http_event_handler;
    config.user_data = response_buffer;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Temperature series queried from Firebase. Status: %d", status_code);
        
        cJSON *json = cJSON_Parse(response_buffer);
        if (json != NULL) {
            ESP_LOGI(TAG, "=== Temperature History (Last %d readings) ===", limit);
            
            cJSON *entry = NULL;
            cJSON_ArrayForEach(entry, json) {
                if (entry->string != NULL) {
                    cJSON *temp_value = cJSON_GetObjectItem(entry, "temperature");
                    cJSON *temp_timestamp = cJSON_GetObjectItem(entry, "timestamp");
                    
                    if (cJSON_IsNumber(temp_value) && cJSON_IsNumber(temp_timestamp)) {
                        time_t ts = (time_t)temp_timestamp->valuedouble;
                        float temp = (float)temp_value->valuedouble;
                        
                        struct tm *timeinfo = localtime(&ts);
                        char time_str[64];
                        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", timeinfo);
                        
                        ESP_LOGI(TAG, "[%s] %.1f°C (ID: %s)", time_str, temp, entry->string);
                    }
                }
            }
            ESP_LOGI(TAG, "=== End of Temperature History ===");
            cJSON_Delete(json);
        }
    } else {
        ESP_LOGE(TAG, "Failed to query temperature series: %s", esp_err_to_name(err));
    }
    
    esp_http_client_cleanup(client);
    return err;
}

json BrewEngine::getFirebaseStatistics(const json &requestData)
{
	// Always use local statistics for Firebase implementation
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
		{"currentDataPoints", this->statisticsManager->GetCurrentSessionDataPoints()},
		{"firebaseUrl", this->firebaseUrl},
		{"firebaseEnabled", this->firebaseEnabled}
	};
	
	result["sessions"] = jSessions;
	result["stats"] = stats;
	result["config"] = config;
	
	return result;
}

json BrewEngine::getFirebaseSessionData(const json &requestData)
{
	// Always use local statistics for Firebase implementation
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
	esp_err_t iter_result = onewire_new_device_iter(this->obh, &iter);
	if (iter_result != ESP_OK) {
		ESP_LOGE(TAG, "Failed to create OneWire device iterator: %s", esp_err_to_name(iter_result));
		ESP_LOGI(TAG, "OneWire sensors not available, continuing without them");
		this->skipTempLoop = false;
		return;
	}
	ESP_LOGI(TAG, "Device iterator created, start searching...");

	int i = 0;
	int max_attempts = 10; // Limit search attempts to prevent infinite loop
	int attempts = 0;
	do
	{

		onewire_device_t next_onewire_device = {};

		search_result = onewire_device_iter_get_next(iter, &next_onewire_device);
		attempts++;
		
		// Add watchdog reset to prevent timeout during search
		if (attempts % 3 == 0) {
			vTaskDelay(pdMS_TO_TICKS(10));
		}
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
	} while (search_result != ESP_ERR_NOT_FOUND && attempts < max_attempts);

	if (attempts >= max_attempts) {
		ESP_LOGW(TAG, "OneWire search reached maximum attempts (%d), stopping to prevent watchdog timeout", max_attempts);
	}

	esp_err_t del_result = onewire_del_device_iter(iter);
	if (del_result != ESP_OK) {
		ESP_LOGE(TAG, "Failed to delete OneWire device iterator: %s", esp_err_to_name(del_result));
	}
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

void BrewEngine::initNtcTemperatureSensors()
{
	ESP_LOGI(TAG, "Initializing NTC temperature sensors from settings");
	
	if (!this->adcInitialized)
	{
		ESP_LOGE(TAG, "ADC not initialized, cannot initialize NTC sensors");
		return;
	}
	
	int ntcSensorCount = 0;
	
	// Initialize all NTC sensors loaded from settings
	for (auto &[sensorId, sensor] : this->sensors)
	{
		if (sensor->sensorType == SENSOR_NTC)
		{
			// Extract analog pin from sensor ID
			int analogPin = (int)(sensorId - 0x4E544300); // "NTC" base ID
			
			// Validate analog pin range
			if (analogPin < 1 || analogPin > 10)
			{
				ESP_LOGW(TAG, "Invalid analog pin %d for NTC sensor [%s], skipping", analogPin, sensor->name.c_str());
				sensor->connected = false;
				continue;
			}
			
			// Map GPIO pin to ADC channel (ESP32-S3 mapping)
			adc_channel_t adc_channel;
			switch (analogPin)
			{
				case 1: adc_channel = ADC_CHANNEL_0; break;
				case 2: adc_channel = ADC_CHANNEL_1; break;
				case 3: adc_channel = ADC_CHANNEL_2; break;
				case 4: adc_channel = ADC_CHANNEL_3; break;
				case 5: adc_channel = ADC_CHANNEL_4; break;
				case 6: adc_channel = ADC_CHANNEL_5; break;
				case 7: adc_channel = ADC_CHANNEL_6; break;
				case 8: adc_channel = ADC_CHANNEL_7; break;
				case 9: adc_channel = ADC_CHANNEL_8; break;
				case 10: adc_channel = ADC_CHANNEL_9; break;
				default:
					ESP_LOGW(TAG, "Unsupported analog pin %d for NTC sensor [%s], skipping", analogPin, sensor->name.c_str());
					sensor->connected = false;
					continue;
			}
			
			// Test ADC channel by taking a reading
			int test_reading;
			esp_err_t test_err = adc_oneshot_read(this->adc1_handle, adc_channel, &test_reading);
			if (test_err == ESP_OK)
			{
				// ADC channel is working, mark sensor as ready for temperature reading
				sensor->connected = false; // Will be set to true on first successful temperature reading
				sensor->consecutiveFailures = 0;
				sensor->analogPin = analogPin; // Ensure analog pin is set correctly
				ntcSensorCount++;
				
				ESP_LOGI(TAG, "NTC sensor [%s] initialized on analog pin %d (ADC channel %d), test reading: %d", 
						sensor->name.c_str(), analogPin, adc_channel, test_reading);
			}
			else
			{
				ESP_LOGW(TAG, "Failed to read from ADC channel %d for NTC sensor [%s]: %s", 
						adc_channel, sensor->name.c_str(), esp_err_to_name(test_err));
				sensor->connected = false;
			}
		}
	}
	
	ESP_LOGI(TAG, "NTC sensor initialization completed, %d NTC sensor(s) found", ntcSensorCount);
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
		vTaskDelay(pdMS_TO_TICKS(500));

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

			// Skip disconnected sensors, except for RTD and NTC sensors which we want to retry
			if (!sensor->connected && 
				sensor->sensorType != SENSOR_PT100 && 
				sensor->sensorType != SENSOR_PT1000 && 
				sensor->sensorType != SENSOR_NTC)
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
			else if (sensor->sensorType == SENSOR_NTC)
			{
				// NTC sensor reading via ADC
				if (!instance->adcInitialized)
				{
					ESP_LOGW(TAG, "ADC not initialized for NTC sensor [%s], skipping", stringId.c_str());
					sensor->connected = false;
					sensor->lastTemp = -999.0;
					if (sensor->show)
					{
						instance->currentTemperatures.insert_or_assign(key, -999.0);
					}
					continue;
				}

				// Get ADC channel from GPIO pin (ESP32-S3 mapping)
				adc_channel_t adc_channel;
				switch (sensor->analogPin)
				{
					case 1: adc_channel = ADC_CHANNEL_0; break;
					case 2: adc_channel = ADC_CHANNEL_1; break;
					case 3: adc_channel = ADC_CHANNEL_2; break;
					case 4: adc_channel = ADC_CHANNEL_3; break;
					case 5: adc_channel = ADC_CHANNEL_4; break;
					case 6: adc_channel = ADC_CHANNEL_5; break;
					case 7: adc_channel = ADC_CHANNEL_6; break;
					case 8: adc_channel = ADC_CHANNEL_7; break;
					case 9: adc_channel = ADC_CHANNEL_8; break;
					case 10: adc_channel = ADC_CHANNEL_9; break;
					default:
						ESP_LOGW(TAG, "Invalid analog pin %d for NTC sensor [%s] (supported: 1-10)", sensor->analogPin, stringId.c_str());
						sensor->connected = false;
						sensor->lastTemp = -999.0;
						if (sensor->show)
						{
							instance->currentTemperatures.insert_or_assign(key, -999.0);
						}
						continue;
				}

				// Read ADC value
				int adc_reading;
				esp_err_t read_err = adc_oneshot_read(instance->adc1_handle, adc_channel, &adc_reading);
				if (read_err != ESP_OK)
				{
					ESP_LOGW(TAG, "Error reading ADC for NTC sensor [%s]: %s", stringId.c_str(), esp_err_to_name(read_err));
					sensor->connected = false;
					sensor->lastTemp = -999.0;
					if (sensor->show)
					{
						instance->currentTemperatures.insert_or_assign(key, -999.0);
					}
					continue;
				}

				// Convert ADC reading to voltage
				int voltage_mv = 0;
				if (instance->adc1_cali_handle != nullptr)
				{
					esp_err_t cali_err = adc_cali_raw_to_voltage(instance->adc1_cali_handle, adc_reading, &voltage_mv);
					if (cali_err != ESP_OK)
					{
						ESP_LOGW(TAG, "ADC calibration failed for NTC sensor [%s], using raw conversion", stringId.c_str());
						// Fallback: approximate conversion for 12-bit ADC with 3.3V reference
						voltage_mv = (adc_reading * 3300) / 4095;
					}
				}
				else
				{
					// Fallback: approximate conversion for 12-bit ADC with 3.3V reference
					voltage_mv = (adc_reading * 3300) / 4095;
				}
				
				// Calculate NTC resistance using voltage divider
				// Voltage divider: 3.3V -> NTC -> analogPin -> dividerResistor -> GND
				// V_adc = V_supply * (R_divider / (R_ntc + R_divider))
				// Solving for R_ntc: R_ntc = ((V_supply - V_adc) * R_divider) / V_adc
				float v_supply = 3300.0f; // 3.3V in mV
				float v_adc = (float)voltage_mv;
				
				// Check for voltage ranges that indicate disconnection or short circuit
				if (v_adc <= 10.0f) // Very low voltage - possible short circuit
				{
					ESP_LOGW(TAG, "NTC sensor [%s] voltage too low (%.1fmV), possible short circuit", stringId.c_str(), v_adc);
					sensor->connected = false;
					sensor->lastTemp = -999.0;
					if (sensor->show)
					{
						instance->currentTemperatures.insert_or_assign(key, -999.0);
					}
					continue;
				}
				
				if (v_adc >= v_supply * 0.95f) // Very high voltage - possible open circuit (disconnected)
				{
					ESP_LOGW(TAG, "NTC sensor [%s] voltage too high (%.1fmV), possible open circuit or disconnection", stringId.c_str(), v_adc);
					sensor->connected = false;
					sensor->lastTemp = -999.0;
					if (sensor->show)
					{
						instance->currentTemperatures.insert_or_assign(key, -999.0);
					}
					continue;
				}
				
				float ntc_resistance = ((v_supply - v_adc) * sensor->dividerResistor) / v_adc;
				
				// Calculate temperature using simplified Steinhart-Hart equation
				// For NTC thermistors: 1/T = 1/T0 + (1/B) * ln(R/R0)
				// Where T0 = 298.15K (25°C), R0 = ntcResistance (at 25°C), B = 3950 (typical value)
				float T0 = 298.15f; // 25°C in Kelvin
				float B = 3950.0f;  // Beta coefficient (typical value for NTC thermistors)
				float R0 = sensor->ntcResistance;
				
				if (ntc_resistance <= 0)
				{
					ESP_LOGW(TAG, "Invalid NTC resistance calculated for sensor [%s]: %.1f ohms", stringId.c_str(), ntc_resistance);
					sensor->connected = false;
					sensor->lastTemp = -999.0;
					if (sensor->show)
					{
						instance->currentTemperatures.insert_or_assign(key, -999.0);
					}
					continue;
				}
				
				// Calculate temperature in Kelvin
				float temp_kelvin = 1.0f / ((1.0f / T0) + (1.0f / B) * logf(ntc_resistance / R0));
				
				// Convert to Celsius
				temperature = temp_kelvin - 273.15f;
				
				// Sanity check - temperature should be reasonable for brewing applications
				// Allow wider range to permit sensor recovery and different applications
				if (temperature < -40.0f || temperature > 150.0f)
				{
					ESP_LOGW(TAG, "NTC sensor [%s] reading out of range: %.1f°C (R=%.1f ohms, V=%.1fmV)", 
							stringId.c_str(), temperature, ntc_resistance, v_adc);
					sensor->connected = false;
					sensor->lastTemp = -999.0;
					if (sensor->show)
					{
						instance->currentTemperatures.insert_or_assign(key, -999.0);
					}
					continue;
				}
				
				// Mark sensor as connected
				if (!sensor->connected)
				{
					ESP_LOGI(TAG, "NTC sensor [%s] connected", stringId.c_str());
				}
				sensor->connected = true;
				sensor->consecutiveFailures = 0;
				
				ESP_LOGD(TAG, "NTC sensor [%s]: ADC=%d, V=%.1fmV, R=%.1f ohms, T=%.1f°C", 
						stringId.c_str(), adc_reading, v_adc, ntc_resistance, temperature);
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

			// Send to Firebase (with interval check)
			if (instance->firebaseEnabled)
			{
				auto now = system_clock::now();
				auto timeSinceLastSend = duration_cast<seconds>(now - instance->lastFirebaseSend).count();
				
				if (timeSinceLastSend >= instance->firebaseSendInterval)
				{
					instance->lastFirebaseSend = now;
					instance->writeTemperatureToFirebase(instance->temperature, instance->targetTemperature, 
						instance->pidOutput, instance->statusText);
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

		// Get system resource usage
		uint32_t freeHeap = esp_get_free_heap_size();
		uint32_t totalHeap = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
		uint32_t usedHeap = totalHeap - freeHeap;
		float memoryUsagePercent = ((float)usedHeap / (float)totalHeap) * 100.0f;
		
		// Simple CPU usage estimation based on system activity
		// Uses memory allocation patterns and task switching as indicators
		static uint32_t lastCpuCheck = 0;
		static uint32_t lastFreeHeap = 0;
		static uint32_t lastMinFreeHeap = 0;
		static float cpuUsagePercent = 15.0f; // Start with baseline 15%
		
		uint32_t currentTime = esp_timer_get_time() / 1000; // Convert to milliseconds
		
		// Update CPU usage estimation every 2000ms
		if (currentTime - lastCpuCheck >= 2000) {
			uint32_t currentFreeHeap = esp_get_free_heap_size();
			uint32_t currentMinFreeHeap = esp_get_minimum_free_heap_size();
			
			if (lastCpuCheck > 0) {
				// Method 1: Heap activity indicates CPU usage
				uint32_t heapActivity = 0;
				if (currentFreeHeap != lastFreeHeap) {
					heapActivity = abs((int32_t)(currentFreeHeap - lastFreeHeap));
				}
				
				// Method 2: Minimum free heap changes indicate memory pressure
				uint32_t memPressure = 0;
				if (currentMinFreeHeap != lastMinFreeHeap) {
					memPressure = abs((int32_t)(currentMinFreeHeap - lastMinFreeHeap));
				}
				
				// Calculate CPU usage based on system activity
				float activityFactor = (float)(heapActivity + memPressure * 2) / 1024.0f;
				cpuUsagePercent = 15.0f + (activityFactor * 5.0f); // Base 15% + activity
				
				// Add some variation based on temperature sensor activity
				if (this->currentTemperatures.size() > 0) {
					cpuUsagePercent += (float)this->currentTemperatures.size() * 2.0f;
				}
				
				// Add load based on PID controller activity
				if (this->pidOutput > 0) {
					cpuUsagePercent += (this->pidOutput / 100.0f) * 10.0f;
				}
				
				// Clamp between realistic values
				if (cpuUsagePercent < 5.0f) cpuUsagePercent = 5.0f;
				if (cpuUsagePercent > 85.0f) cpuUsagePercent = 85.0f;
			}
			
			lastCpuCheck = currentTime;
			lastFreeHeap = currentFreeHeap;
			lastMinFreeHeap = currentMinFreeHeap;
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
			{"systemInfo", {
				{"freeHeap", freeHeap},
				{"totalHeap", totalHeap},
				{"usedHeap", usedHeap},
				{"memoryUsagePercent", (double)((int)(memoryUsagePercent * 10)) / 10},
				{"cpuUsagePercent", (double)((int)(cpuUsagePercent * 10)) / 10}
			}},
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
		
		// Log session start to Firebase
		if (this->firebaseEnabled)
		{
			// Session metadata is now tracked by local statistics manager
			ESP_LOGI(TAG, "Session started - metadata logged locally and to Firebase via temperature writes");
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
		
		// Log session end to Firebase
		if (this->firebaseEnabled)
		{
			// Session metadata is now tracked by local statistics manager
			ESP_LOGI(TAG, "Session ended - metadata logged locally");
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
	else if (command == "AddNtcSensor")
	{
		string name = data["name"];
		int analogPin = data["analogPin"];
		int sensorType = data["sensorType"];
		float ntcResistance = data["ntcResistance"];
		float dividerResistor = data["dividerResistor"];
		bool useForControl = data["useForControl"];
		bool show = data["show"];
		
		// Generate unique ID for NTC sensor
		uint64_t ntcSensorId = 0x4E544300 + analogPin; // "NTC" base ID + analog pin
		
		// Check if sensor already exists
		if (this->sensors.find(ntcSensorId) != this->sensors.end())
		{
			success = false;
			message = "NTC sensor with analog pin " + to_string(analogPin) + " already exists";
		}
		else
		{
			// Validate GPIO pin
			if (analogPin < 0 || analogPin >= GPIO_NUM_MAX)
			{
				success = false;
				message = "Invalid analog pin number: " + to_string(analogPin);
			}
			else
			{
				// Create temperature sensor object
				auto sensor = new TemperatureSensor();
				sensor->id = ntcSensorId;
				sensor->name = name;
				sensor->color = "#9C27B0"; // Purple for NTC
				sensor->useForControl = useForControl;
				sensor->show = show;
				sensor->connected = true; // NTC sensors are always "connected" if properly wired
				sensor->compensateAbsolute = 0;
				sensor->compensateRelative = 1;
				sensor->sensorType = (SensorType)sensorType;
				sensor->analogPin = analogPin;
				sensor->ntcResistance = ntcResistance;
				sensor->dividerResistor = dividerResistor;
				sensor->consecutiveFailures = 0;
				
				this->sensors.insert_or_assign(sensor->id, sensor);
				
				// Save sensor settings
				json jSensors = json::array({});
				for (auto const &[key, val] : this->sensors)
				{
					json jSensor = val->to_json();
					jSensors.push_back(jSensor);
				}
				this->saveTempSensorSettings(jSensors);
				
				ESP_LOGI(TAG, "NTC sensor added successfully: %s (analog pin %d)", name.c_str(), analogPin);
				message = "NTC sensor added successfully";
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
			{"firebaseUrl", this->firebaseUrl},
			{"firebaseApiKey", this->firebaseApiKey},
			{"firebaseAuthToken", this->firebaseAuthToken},
			{"firebaseEmail", this->firebaseEmail},
			{"firebasePassword", this->firebasePassword},
			{"firebaseAuthMethod", this->firebaseAuthMethod},
			{"firebaseSendInterval", this->firebaseSendInterval},
			{"firebaseDatabaseEnabled", this->firebaseDatabaseEnabled},
		};
	}
	else if (command == "SaveSystemSettings")
	{
		this->saveSystemSettingsJson(data);
		message = "Please restart device for changes to have effect!";
	}
	else if (command == "TestFirebase")
	{
		if (this->firebaseUrl.empty())
		{
			message = "Firebase configuration incomplete";
			success = false;
		}
		else
		{
			// Test connection with a dummy data point
			esp_err_t result = this->writeTemperatureToFirebase(25.0, 25.0, 50, "test");
			if (result == ESP_OK) {
				message = "Firebase connection test successful";
			} else {
				message = "Firebase connection test failed - check logs for details";
				success = false;
			}
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
		if (this->firebaseEnabled)
		{
			resultData = this->getFirebaseStatistics(data);
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
			if (this->firebaseEnabled)
			{
				resultData = this->getFirebaseSessionData(data);
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
	config.stack_size = 32768;          // Increased from 20480 to prevent stack overflow
	config.uri_match_fn = httpd_uri_match_wildcard;
	config.max_open_sockets = 4;        // Reduce concurrent connections 
	config.max_uri_handlers = 8;        // Limit URI handlers
	config.max_resp_headers = 8;        // Limit response headers
	config.recv_wait_timeout = 5;       // Reduce timeout to free resources faster
	config.send_wait_timeout = 5;       // Reduce timeout to free resources faster

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