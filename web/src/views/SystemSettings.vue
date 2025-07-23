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
  influxdbUrl: "",
  influxdbToken: "",
  influxdbOrg: "",
  influxdbBucket: "",
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

const testInfluxDB = async () => {
  if (!systemSettings.value.influxdbUrl || !systemSettings.value.influxdbToken || 
      !systemSettings.value.influxdbOrg || !systemSettings.value.influxdbBucket) {
    alert.value = "Please fill in all InfluxDB configuration fields";
    alertType.value = "warning";
    return;
  }

  const requestData = {
    command: "TestInfluxDB",
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
          <h3>InfluxDB Configuration</h3>
        </v-col>
      </v-row>

      <v-row>
        <v-col cols="12" md="3">
          <v-text-field v-model="systemSettings.influxdbUrl" placeholder="http://localhost:8086"
            label="InfluxDB URL">
            <template v-slot:append>
              <v-tooltip text="InfluxDB server URL">
                <template v-slot:activator="{ props }">
                  <v-icon size="small" v-bind="props">{{ mdiHelp }}</v-icon>
                </template>
              </v-tooltip>
            </template>
          </v-text-field>
        </v-col>
        <v-col cols="12" md="3">
          <v-text-field v-model="systemSettings.influxdbToken" type="password"
            label="InfluxDB Token">
            <template v-slot:append>
              <v-tooltip text="InfluxDB authentication token">
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
          <v-text-field v-model="systemSettings.influxdbOrg" 
            label="InfluxDB Organization">
            <template v-slot:append>
              <v-tooltip text="InfluxDB organization name">
                <template v-slot:activator="{ props }">
                  <v-icon size="small" v-bind="props">{{ mdiHelp }}</v-icon>
                </template>
              </v-tooltip>
            </template>
          </v-text-field>
        </v-col>
        <v-col cols="12" md="3">
          <v-text-field v-model="systemSettings.influxdbBucket" 
            label="InfluxDB Bucket">
            <template v-slot:append>
              <v-tooltip text="InfluxDB bucket name for data storage">
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
          <v-btn color="info" variant="outlined" class="mt-2" @click="testInfluxDB">
            Test InfluxDB Connection
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
