<script lang="ts" setup>
import WebConn from "@/helpers/webConn";
import { ISystemSettings } from "@/interfaces/ISystemSettings";
import { mdiHelp } from "@mdi/js";
import { inject, onBeforeUnmount, onMounted, ref } from "vue";
import { useI18n } from "vue-i18n";
const { t } = useI18n({ useScope: "global" });

const webConn = inject<WebConn>("webConn");

const systemSettings = ref<ISystemSettings>({
  // add default value, vue has issues with null values atm
  onewirePin: 0,
  stirPin: 0,
  buzzerPin: 0,
  buzzerTime: 2,
  invertOutputs: false,
  mqttUri: "",
  firebaseUrl: "",
  firebaseApiKey: "",
  firebaseAuthToken: "",
  firebaseEmail: "",
  firebasePassword: "",
  firebaseAuthMethod: "email", // default to email method
  firebaseSendInterval: 10,
  firebaseDatabaseEnabled: true,
  temperatureScale: 0,
  rtdSensorsEnabled: false,
  spiMosiPin: 23,
  spiMisoPin: 19,
  spiClkPin: 18,
  spiCsPin: 5,
});

// is same as enum TemperatureScale, but this wel never change, converting enum to options would be wastefull
const temperatureScales = [
  { title: t("systemSettings.celsius"), value: 0 },
  { title: t("systemSettings.fahrenheit"), value: 1 },
];

const alert = ref<string>("");
const alertType = ref<"error" | "success" | "warning" | "info">("info");

const dialogFactoryReset = ref<boolean>(false);

const getData = async () => {
  const requestData = {
    command: "GetSystemSettings",
    data: null,
  };

  const apiResult = await webConn?.doPostRequest(requestData);

  if (apiResult === undefined || apiResult.success === false) {
    return;
  }
  systemSettings.value = apiResult.data;
};

onMounted(() => {
  getData();
});

onBeforeUnmount(() => {});

const save = async () => {
  if (systemSettings.value == null) {
    return;
  }

  const requestData = {
    command: "SaveSystemSettings",
    data: systemSettings.value,
  };

  const result = await webConn?.doPostRequest(requestData);
  if (result?.message != null) {
    alert.value = result?.message;
    alertType.value = "warning";
    window.scrollTo(0, 0);
  }
};

const recovery = async () => {
  const requestData = {
    command: "BootIntoRecovery",
  };

  const result = await webConn?.doPostRequest(requestData);
  if (result?.message != null) {
    alertType.value = "warning";
    alert.value = result?.message;
    window.scrollTo(0, 0);
  }

  if (result?.success) {
    setTimeout(() => {
      document.location.href = "/";
    }, 10000);
  }
};

const reboot = async () => {
  const requestData = {
    command: "Reboot",
  };

  const result = await webConn?.doPostRequest(requestData);
  if (result?.message != null) {
    alertType.value = "warning";
    alert.value = result?.message;
    window.scrollTo(0, 0);
  }

  if (result?.success) {
    setTimeout(() => {
      document.location.href = "/";
    }, 10000);
  }
};

const factoryReset = async () => {
  const requestData = {
    command: "FactoryReset",
  };

  dialogFactoryReset.value = false;

  const result = await webConn?.doPostRequest(requestData);
  if (result?.message != null) {
    alertType.value = "warning";
    alert.value = result?.message;
    window.scrollTo(0, 0);
  }

  if (result?.success) {
    setTimeout(() => {
      document.location.href = "/";
    }, 10000);
  }
};

const scaleChanged = () => {
  alertType.value = "info";
  alert.value = t("systemSettings.mash_cant_be_converted");
};

const testFirebase = async () => {
  if (!systemSettings.value.firebaseDatabaseEnabled) {
    alert.value = "Firebase database is disabled";
    alertType.value = "warning";
    return;
  }
  if (!systemSettings.value.firebaseUrl) {
    alert.value = "Please fill in Firebase URL";
    alertType.value = "warning";
    return;
  }
  // API Key is always required
  if (!systemSettings.value.firebaseApiKey) {
    alert.value = "Please fill in Firebase API Key";
    alertType.value = "warning";
    return;
  }
  
  // Validate based on selected authentication method
  if (systemSettings.value.firebaseAuthMethod === 'email') {
    if (!systemSettings.value.firebaseEmail) {
      alert.value = "Please fill in Firebase Email";
      alertType.value = "warning";
      return;
    }
    if (!systemSettings.value.firebasePassword) {
      alert.value = "Please fill in Firebase Password";
      alertType.value = "warning";
      return;
    }
  } else if (systemSettings.value.firebaseAuthMethod === 'token') {
    if (!systemSettings.value.firebaseAuthToken) {
      alert.value = "Please fill in Firebase Custom Token";
      alertType.value = "warning";
      return;
    }
  }

  const requestData = {
    command: "TestFirebase",
    data: null,
  };

  const result = await webConn?.doPostRequest(requestData);
  if (result?.message != null) {
    alert.value = result?.message;
    alertType.value = result?.success ? "success" : "error";
    window.scrollTo(0, 0);
  }
};
</script>

<template>
  <v-container class="spacing-playground pa-6" fluid>
    <v-alert :type="alertType" v-if="alert" closable @click:close="alert = ''">{{ alert }}</v-alert>
    <v-form fast-fail @submit.prevent>

      <v-row>
        <v-col cols="12" md="3">
          <v-text-field requierd v-model.number="systemSettings.onewirePin" :label='t("systemSettings.onewire_pin")'>
            <template v-slot:append>
              <v-tooltip :text='t("systemSettings.onewire_pin_tooltip")'>
                <template v-slot:activator="{ props }">
                  <v-icon size="small" v-bind="props">{{ mdiHelp }}</v-icon>
                </template>
              </v-tooltip>
            </template>
          </v-text-field>
        </v-col>
      </v-row>

      <v-row>
        <v-col cols="12" md="3">
          <v-text-field v-model.number="systemSettings.stirPin" :label='t("systemSettings.stir_pin")'>
            <template v-slot:append>
              <v-tooltip :text='t("systemSettings.stir_pin_tooltip")'>
                <template v-slot:activator="{ props }">
                  <v-icon size="small" v-bind="props">{{ mdiHelp }}</v-icon>
                </template>
              </v-tooltip>
            </template>
          </v-text-field>
        </v-col>
      </v-row>

      <v-row>
        <v-col cols="12" md="3">
          <v-text-field v-model.number="systemSettings.buzzerPin" :label='t("systemSettings.buzzer_pin")'>
            <template v-slot:append>
              <v-tooltip :text='t("systemSettings.buzzer_pin_tooltip")'>
                <template v-slot:activator="{ props }">
                  <v-icon size="small" v-bind="props">{{ mdiHelp }}</v-icon>
                </template>
              </v-tooltip>
            </template>
          </v-text-field>
        </v-col>
        <v-col cols="12" md="3">
          <v-text-field v-model.number="systemSettings.buzzerTime" :label='t("systemSettings.buzzer_time")'>
            <template v-slot:append>
              <v-tooltip :text='t("systemSettings.buzzer_time_tooltip")'>
                <template v-slot:activator="{ props }">
                  <v-icon size="small" v-bind="props">{{ mdiHelp }}</v-icon>
                </template>
              </v-tooltip>
            </template>
          </v-text-field>
        </v-col>
      </v-row>

      <v-row>
        <v-col cols="12" md="3">
          <v-checkbox v-model="systemSettings.invertOutputs" :label='t("systemSettings.invert")'>
            <template v-slot:append>
              <v-tooltip :text='t("systemSettings.invert_tooltip")'>
                <template v-slot:activator="{ props }">
                  <v-icon size="small" v-bind="props">{{ mdiHelp }}</v-icon>
                </template>
              </v-tooltip>
            </template>
          </v-checkbox>
        </v-col>
      </v-row>

      <v-row>
        <v-col cols="12" md="3">
          <v-text-field v-model="systemSettings.mqttUri" placeholder="mqtt://user:password@192.168.0.1:1883"
            :label='t("systemSettings.mqtt_uri")'>
            <template v-slot:append>
              <v-tooltip :text='t("systemSettings.mqtt_tooltip")'>
                <template v-slot:activator="{ props }">
                  <v-icon size="small" v-bind="props">{{ mdiHelp }}</v-icon>
                </template>
              </v-tooltip>
            </template>
          </v-text-field>
        </v-col>
      </v-row>

      <v-row>
        <v-col cols="12" md="12">
          <h3>Firebase Configuration</h3>
          <p class="text-body-2 text-medium-emphasis mb-2">
            Choose your Firebase authentication method below.
          </p>
        </v-col>
      </v-row>

      <v-row>
        <v-col cols="12" md="6">
          <v-checkbox v-model="systemSettings.firebaseDatabaseEnabled" label="Enable Firebase Database Logging">
            <template v-slot:append>
              <v-tooltip text="Enable or disable Firebase database logging. Automatically disabled in AP mode since internet connection is required.">
                <template v-slot:activator="{ props }">
                  <v-icon size="small" v-bind="props">{{ mdiHelp }}</v-icon>
                </template>
              </v-tooltip>
            </template>
          </v-checkbox>
        </v-col>
      </v-row>

      <v-row v-if="systemSettings.firebaseDatabaseEnabled">
        <v-col cols="12">
          <v-radio-group v-model="systemSettings.firebaseAuthMethod" inline>
            <template v-slot:label>
              <div class="text-subtitle-1 fw-bold">Authentication Method:</div>
            </template>
            <v-radio label="Email & Password (Recommended)" value="email">
              <template v-slot:label>
                <div>
                  <strong>Email & Password</strong> <span class="text-success">(Recommended)</span>
                  <div class="text-caption text-medium-emphasis">
                    Standard Firebase user authentication - no token expiration issues
                  </div>
                </div>
              </template>
            </v-radio>
            <v-radio label="API Key & Custom Token" value="token">
              <template v-slot:label>
                <div>
                  <strong>API Key & Custom Token</strong>
                  <div class="text-caption text-medium-emphasis">
                    Service account authentication - requires token regeneration every hour
                  </div>
                </div>
              </template>
            </v-radio>
          </v-radio-group>
        </v-col>
      </v-row>

      <v-row v-if="systemSettings.firebaseDatabaseEnabled">
        <v-col cols="12" md="6">
          <v-text-field v-model="systemSettings.firebaseUrl" placeholder="https://your-project.firebasedatabase.app"
            label="Firebase URL"
            @blur="systemSettings.firebaseUrl = systemSettings.firebaseUrl.trim()">
            <template v-slot:append>
              <v-tooltip text="Firebase Realtime Database URL">
                <template v-slot:activator="{ props }">
                  <v-icon size="small" v-bind="props">{{ mdiHelp }}</v-icon>
                </template>
              </v-tooltip>
            </template>
          </v-text-field>
        </v-col>
      </v-row>

      <v-row v-if="systemSettings.firebaseDatabaseEnabled">
        <v-col cols="12">
          <v-textarea v-model="systemSettings.firebaseApiKey" 
            placeholder="AIzaSyC..."
            label="Firebase Web API Key"
            rows="2"
            variant="outlined"
            hide-details="auto"
            @blur="systemSettings.firebaseApiKey = systemSettings.firebaseApiKey.trim()">
            <template v-slot:append>
              <v-tooltip text="Firebase Web API Key from Project Settings > General > Web API Key. Paste the full key here.">
                <template v-slot:activator="{ props }">
                  <v-icon size="small" v-bind="props">{{ mdiHelp }}</v-icon>
                </template>
              </v-tooltip>
            </template>
          </v-textarea>
        </v-col>
      </v-row>

      <v-row v-if="systemSettings.firebaseAuthMethod === 'token' && systemSettings.firebaseDatabaseEnabled">
        <v-col cols="12">
          <v-textarea v-model="systemSettings.firebaseAuthToken" 
            placeholder="eyJhbGciOiJS..."
            label="Firebase Custom Token"
            rows="3"
            variant="outlined"
            hide-details="auto"
            @blur="systemSettings.firebaseAuthToken = systemSettings.firebaseAuthToken.trim()">
            <template v-slot:append>
              <v-tooltip text="Firebase Custom Token (JWT) generated from your service account. This token is very long - paste the complete token here.">
                <template v-slot:activator="{ props }">
                  <v-icon size="small" v-bind="props">{{ mdiHelp }}</v-icon>
                </template>
              </v-tooltip>
            </template>
          </v-textarea>
        </v-col>
      </v-row>

      <v-row v-if="systemSettings.firebaseAuthMethod === 'email' && systemSettings.firebaseDatabaseEnabled">
        <v-col cols="12" md="6">
          <v-text-field v-model="systemSettings.firebaseEmail" 
            placeholder="device@yourdomain.com"
            label="Firebase Email"
            type="email"
            variant="outlined"
            hide-details="auto"
            @blur="systemSettings.firebaseEmail = systemSettings.firebaseEmail.trim()">
            <template v-slot:append>
              <v-tooltip text="Firebase user email for email/password authentication.">
                <template v-slot:activator="{ props }">
                  <v-icon size="small" v-bind="props">{{ mdiHelp }}</v-icon>
                </template>
              </v-tooltip>
            </template>
          </v-text-field>
        </v-col>
        <v-col cols="12" md="6">
          <v-text-field v-model="systemSettings.firebasePassword" 
            placeholder="password123"
            label="Firebase Password"
            type="password"
            variant="outlined"
            hide-details="auto">
            <template v-slot:append>
              <v-tooltip text="Firebase user password for email/password authentication.">
                <template v-slot:activator="{ props }">
                  <v-icon size="small" v-bind="props">{{ mdiHelp }}</v-icon>
                </template>
              </v-tooltip>
            </template>
          </v-text-field>
        </v-col>
      </v-row>

      <v-row v-if="systemSettings.firebaseDatabaseEnabled">
        <v-col cols="12" md="3">
          <v-text-field v-model.number="systemSettings.firebaseSendInterval" 
            label="Send Interval (seconds)" type="number" min="1" max="300">
            <template v-slot:append>
              <v-tooltip text="How often to send data to Firebase (1-300 seconds)">
                <template v-slot:activator="{ props }">
                  <v-icon size="small" v-bind="props">{{ mdiHelp }}</v-icon>
                </template>
              </v-tooltip>
            </template>
          </v-text-field>
        </v-col>
        <v-col cols="12" md="3">
          <v-btn color="info" variant="outlined" class="mt-2" @click="testFirebase">
            Test Firebase Connection
          </v-btn>
        </v-col>
      </v-row>

      <v-row>
        <v-col cols="12" md="3">
          <v-select :label='t("systemSettings.temperature_scale")' v-model="systemSettings.temperatureScale"
            :items="temperatureScales" @blur="scaleChanged" />
        </v-col>
      </v-row>

      <v-row>
        <v-col cols="12" md="12">
          <h3>RTD Sensor Configuration (MAX31865)</h3>
        </v-col>
      </v-row>

      <v-row>
        <v-col cols="12" md="3">
          <v-checkbox v-model="systemSettings.rtdSensorsEnabled" label="Enable RTD Sensors">
            <template v-slot:append>
              <v-tooltip text="Enable PT100/PT1000 RTD sensors via MAX31865 amplifier">
                <template v-slot:activator="{ props }">
                  <v-icon size="small" v-bind="props">{{ mdiHelp }}</v-icon>
                </template>
              </v-tooltip>
            </template>
          </v-checkbox>
        </v-col>
      </v-row>

      <v-row v-if="systemSettings.rtdSensorsEnabled">
        <v-col cols="12" md="3">
          <v-text-field v-model.number="systemSettings.spiMosiPin" label="SPI MOSI Pin">
            <template v-slot:append>
              <v-tooltip text="SPI Master Out Slave In pin for MAX31865">
                <template v-slot:activator="{ props }">
                  <v-icon size="small" v-bind="props">{{ mdiHelp }}</v-icon>
                </template>
              </v-tooltip>
            </template>
          </v-text-field>
        </v-col>
        <v-col cols="12" md="3">
          <v-text-field v-model.number="systemSettings.spiMisoPin" label="SPI MISO Pin">
            <template v-slot:append>
              <v-tooltip text="SPI Master In Slave Out pin for MAX31865">
                <template v-slot:activator="{ props }">
                  <v-icon size="small" v-bind="props">{{ mdiHelp }}</v-icon>
                </template>
              </v-tooltip>
            </template>
          </v-text-field>
        </v-col>
        <v-col cols="12" md="3">
          <v-text-field v-model.number="systemSettings.spiClkPin" label="SPI Clock Pin">
            <template v-slot:append>
              <v-tooltip text="SPI Clock pin for MAX31865">
                <template v-slot:activator="{ props }">
                  <v-icon size="small" v-bind="props">{{ mdiHelp }}</v-icon>
                </template>
              </v-tooltip>
            </template>
          </v-text-field>
        </v-col>
        <v-col cols="12" md="3">
          <v-text-field v-model.number="systemSettings.spiCsPin" label="SPI CS Pin">
            <template v-slot:append>
              <v-tooltip text="SPI Chip Select pin for MAX31865">
                <template v-slot:activator="{ props }">
                  <v-icon size="small" v-bind="props">{{ mdiHelp }}</v-icon>
                </template>
              </v-tooltip>
            </template>
          </v-text-field>
        </v-col>
      </v-row>

      <v-row>
        <v-col cols="12" md="3">
          <v-btn color="success" class="mt-4 mr-2" @click="save">{{ t("general.save") }} </v-btn>
        </v-col>
      </v-row>

      <v-row>
        <v-col cols="12" md="12" />
      </v-row>

      <v-row>
        <v-col cols="12" md="3">
          <v-btn name="reboot" color="warning" variant="outlined" class="mt-4 mr-2" @click="reboot">
            {{ t("systemSettings.reboot") }} </v-btn>
        </v-col>
        <v-col cols="12" md="3">
          <v-btn name="factoryreset" color="error" variant="outlined" class="mt-4 mr-2"
            @click="dialogFactoryReset = true">
            {{ t("systemSettings.factory_reset") }} </v-btn>

          <v-dialog v-model="dialogFactoryReset" max-width="500px">
            <v-card>
              <v-card-title class="text-h5">{{ t("systemSettings.factory_reset_title") }}</v-card-title>
              <v-card-text>{{ t("systemSettings.factory_reset_text") }}</v-card-text>
              <v-card-actions>
                <v-spacer />
                <v-btn variant="outlined" @click="dialogFactoryReset = false">{{ t("systemSettings.factory_reset_no")
                  }}</v-btn>
                <v-btn variant="outlined" color="red" @click="factoryReset">{{ t("systemSettings.factory_reset_yes")
                  }}</v-btn>
                <v-spacer />
              </v-card-actions>
            </v-card>
          </v-dialog>
        </v-col>
      </v-row>

      <v-row>
        <v-col cols="12" md="12">
          <label for="recovery">{{ t("systemSettings.recovery_text") }}</label>
          <br />
          <v-btn name="recovery" color="warning" variant="outlined" class="mt-4 mr-2" @click="recovery">
            {{ t("systemSettings.recovery") }} </v-btn>
        </v-col>
      </v-row>
    </v-form>
  </v-container>
</template>
