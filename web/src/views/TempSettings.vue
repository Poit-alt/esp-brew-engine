<script lang="ts" setup>
import WebConn from "@/helpers/webConn";
import { ITempSensor } from "@/interfaces/ITempSensor";
import { mdiDelete, mdiHelp, mdiPalette, mdiPencil, mdiPlus, mdiThermometer } from "@mdi/js";
import { inject, onBeforeUnmount, onMounted, ref } from "vue";
import { useI18n } from "vue-i18n";
const { t } = useI18n({ useScope: "global" });

const webConn = inject<WebConn>("webConn");

const tempSensors = ref<Array<ITempSensor>>([]);

const tableHeaders = ref<Array<any>>([
  { title: t("tempSettings.id"), key: "id", align: "start" },
  { title: t("tempSettings.name"), key: "name", align: "start" },
  { title: "Type", key: "sensorType", align: "start" },
  { title: "CS Pin", key: "csPin", align: "start" },
  { title: t("tempSettings.color"), key: "color", align: "start" },
  { title: t("tempSettings.compensate_abs"), key: "compensateAbsolute", align: "start" },
  { title: t("tempSettings.compensate_rel"), key: "compensateRelative", align: "start" },
  { title: t("tempSettings.show"), key: "show", align: "end" },
  { title: t("tempSettings.use_for_control"), key: "useForControl", align: "end" },
  { title: t("tempSettings.connected"), key: "connected", align: "end" },
  { title: t("tempSettings.last_temp"), key: "lastTemp", align: "end" },
  { title: t("tempSettings.actions"), key: "actions", align: "end", sortable: false },
]);

const dialog = ref<boolean>(false);
const dialogDelete = ref<boolean>(false);

const alert = ref<string>("");
const alertType = ref<"error" | "success" | "warning" | "info">("info");

const defaultSensor: ITempSensor = {
  id: "",
  name: t("tempSettings.new_sensor"),
  color: "#ffffff",
  useForControl: false,
  show: false,
  connected: false,
  compensateAbsolute: 0.0,
  compensateRelative: 1,
  lastTemp: 0,
  sensorType: 0,
  csPin: 5,
};

const sensorTypes = [
  { title: "DS18B20", value: 0 },
  { title: "PT100", value: 1 },
  { title: "PT1000", value: 2 },
];

const editedItem = ref<ITempSensor>(defaultSensor);

// RTD sensor management
const dialogAddRtd = ref<boolean>(false);
const newRtdSensor = ref({
  name: "RTD Sensor",
  csPin: 5,
  sensorType: 1, // PT100
  useForControl: false,
  show: true
});

const rtdSensorTypes = [
  { title: "PT100", value: 1 },
  { title: "PT1000", value: 2 },
];

const getData = async () => {
  const requestData = {
    command: "GetTempSettings",
    data: null,
  };

  const apiResult = await webConn?.doPostRequest(requestData);

  if (apiResult === undefined || apiResult.success === false) {
    return;
  }

  tempSensors.value = apiResult.data;
};

const detectTempSensors = async () => {
  const requestData = {
    command: "DetectTempSensors",
    data: null,
  };

  alert.value = t("tempSettings.msg_scan");
  alertType.value = "info";

  const apiResult = await webConn?.doPostRequest(requestData);

  alert.value = ""; // clear alert

  if (apiResult === undefined || apiResult.success === false) {
    return;
  }

  // after successfull detect get new data
  getData();
};

onMounted(() => {
  getData();
});

onBeforeUnmount(() => {});

const closeDialog = async () => {
  dialog.value = false;
};

const closeDeleteDialog = async () => {
  dialogDelete.value = false;
};

const editItem = async (item: ITempSensor) => {
  editedItem.value = item;
  dialog.value = true;
};

const openDeleteDialog = async (item: ITempSensor) => {
  editedItem.value = item;
  dialogDelete.value = true;
};

const deleteItemOk = async () => {
  // filter out deleted
  tempSensors.value = tempSensors.value.filter((s) => s.id !== editedItem.value.id);
  closeDeleteDialog();
};

const save = async () => {
  if (tempSensors.value == null) {
    return;
  }

  const requestData = {
    command: "SaveTempSettings",
    data: tempSensors.value,
  };

  await webConn?.doPostRequest(requestData);
  // todo capture result and log errors
};

const addRtdSensor = async () => {
  const requestData = {
    command: "AddRtdSensor",
    data: {
      name: newRtdSensor.value.name,
      csPin: newRtdSensor.value.csPin,
      sensorType: newRtdSensor.value.sensorType,
      useForControl: newRtdSensor.value.useForControl,
      show: newRtdSensor.value.show
    }
  };

  const result = await webConn?.doPostRequest(requestData);
  if (result?.success) {
    dialogAddRtd.value = false;
    await getData(); // Refresh the sensor list
    alertType.value = "success";
    alert.value = "RTD sensor added successfully";
  } else {
    alertType.value = "error";
    alert.value = result?.message || "Failed to add RTD sensor";
  }
};

const closeAddRtdDialog = () => {
  dialogAddRtd.value = false;
  // Reset form
  newRtdSensor.value = {
    name: "RTD Sensor",
    csPin: 5,
    sensorType: 1,
    useForControl: false,
    show: true
  };
};
</script>

<template>
  <v-container class="pa-6" fluid>
    <v-alert :type="alertType" v-if="alert" closable @click:close="alert = ''">{{ alert }}</v-alert>
    
    <v-row class="mb-4">
      <v-col cols="12">
        <v-alert type="info" variant="tonal">
          <h4>Temperature Sensor Control</h4>
          <p class="mt-2">
            <strong>Use for Control:</strong> Check this to use the sensor for PID heating control. You can select multiple sensors - they will be averaged for control purposes.
            <br>
            <strong>Show:</strong> Display the sensor on the temperature chart.
            <br>
            <strong>Sensor Types:</strong> DS18B20 (OneWire), PT100 (RTD), PT1000 (RTD via MAX31865)
          </p>
        </v-alert>
      </v-col>
    </v-row>

    <!-- RTD Sensor Management Section -->
    <v-row class="mb-4">
      <v-col cols="12">
        <v-card>
          <v-card-title>
            <v-icon class="mr-2">{{ mdiThermometer }}</v-icon>
            PT100/PT1000 RTD Sensors
          </v-card-title>
          <v-card-subtitle>
            Add high-precision RTD temperature sensors using MAX31865 amplifiers
          </v-card-subtitle>
          <v-card-text>
            <v-row>
              <v-col cols="12">
                <p class="text-body-2 mb-3">
                  <strong>Note:</strong> Make sure RTD sensors are enabled in System Settings and SPI MOSI/MISO/CLK pins are configured before adding sensors. Each sensor needs its own CS pin.
                </p>
                <v-btn color="primary" @click="dialogAddRtd = true">
                  <v-icon class="mr-1">{{ mdiPlus }}</v-icon>
                  Add RTD Sensor
                </v-btn>
              </v-col>
            </v-row>
          </v-card-text>
        </v-card>
      </v-col>
    </v-row>
    
    <v-form fast-fail @submit.prevent>
      <v-data-table :headers="tableHeaders" :items="tempSensors" density="compact" item-value="name">
        <template v-slot:top>
          <v-toolbar density="compact">
            <v-toolbar-title>{{ t('tempSettings.temp_sensors') }}</v-toolbar-title>
            <v-spacer />
            <v-btn color="secondary" variant="outlined" class="mr-5" @click="getData()">
              {{ t('general.refresh') }}
            </v-btn>
            <v-btn color="secondary" variant="outlined" class="mr-5" @click="detectTempSensors()">
              {{ t('tempSettings.detect') }}
            </v-btn>

            <v-dialog v-model="dialog" max-width="500px">
              <v-card>
                <v-toolbar density="compact" color="dialog-header">
                  <v-toolbar-title>{{ t('general.edit') }}</v-toolbar-title>
                </v-toolbar>

                <v-card-text>
                  <v-container>
                    <v-row>
                      <v-col cols="12" md="6">
                        <v-text-field v-model="editedItem.name" :label='t("tempSettings.name")' />
                      </v-col>
                      <v-col cols="12" md="6">
                        <v-select v-model="editedItem.sensorType" :items="sensorTypes" label="Sensor Type" />
                      </v-col>
                    </v-row>
                    <v-row v-if="editedItem.sensorType === 1 || editedItem.sensorType === 2">
                      <v-col cols="12" md="6">
                        <v-text-field v-model.number="editedItem.csPin" label="SPI CS Pin" type="number" hint="GPIO pin for Chip Select" />
                      </v-col>
                      <v-col cols="12" md="6">
                        <v-alert type="warning" variant="tonal" density="compact">
                          Changing CS pin requires sensor restart
                        </v-alert>
                      </v-col>
                    </v-row>
                    <v-row>
                      <v-col cols="12" md="6">
                        <v-switch v-model="editedItem.show" :label='t("tempSettings.show")' color="green">
                          <template v-slot:append>
                            <v-tooltip text="Show sensor on temperature chart">
                              <template v-slot:activator="{ props }">
                                <v-icon size="small" v-bind="props">{{ mdiHelp }}</v-icon>
                              </template>
                            </v-tooltip>
                          </template>
                        </v-switch>
                      </v-col>
                      <v-col cols="12" md="6">
                        <v-switch v-model="editedItem.useForControl" :label='t("tempSettings.enabled")' color="red">
                          <template v-slot:append>
                            <v-tooltip text="Use sensor for PID control and heating regulation">
                              <template v-slot:activator="{ props }">
                                <v-icon size="small" v-bind="props">{{ mdiHelp }}</v-icon>
                              </template>
                            </v-tooltip>
                          </template>
                        </v-switch>
                      </v-col>
                    </v-row>
                    <v-row>
                      <v-col cols="12">
                        <v-color-picker v-model="editedItem.color" hide-inputs :label='t("tempSettings.color")' />
                      </v-col>
                    </v-row>
                    <v-row>
                      <v-col cols="12" md="6">
                        <v-text-field type="number" v-model.number="editedItem.compensateAbsolute" :label='t("tempSettings.compensate_abs")' 
                          hint="Absolute temperature compensation in °C" />
                      </v-col>
                      <v-col cols="12" md="6">
                        <v-text-field type="number" v-model.number="editedItem.compensateRelative" :label='t("tempSettings.compensate_rel")' 
                          hint="Relative temperature compensation factor" />
                      </v-col>
                    </v-row>
                  </v-container>
                </v-card-text>

                <v-card-actions>
                  <v-spacer />
                  <v-btn color="blue-darken-1" variant="text" @click="closeDialog">
                    {{ t('general.close') }}
                  </v-btn>
                </v-card-actions>
              </v-card>
            </v-dialog>
            <v-dialog v-model="dialogDelete" max-width="500px">
              <v-card>
                <v-card-title class="text-h5">{{ t('general.delete_message') }}</v-card-title>
                <v-card-actions>
                  <v-spacer />
                  <v-btn color="blue-darken-1" variant="text" @click="closeDeleteDialog">{{ t('general.cancel')}}</v-btn>
                  <v-btn color="blue-darken-1" variant="text" @click="deleteItemOk">{{ t('general.ok') }}</v-btn>
                  <v-spacer />
                </v-card-actions>
              </v-card>
            </v-dialog>
            
            <!-- Add RTD Sensor Dialog -->
            <v-dialog v-model="dialogAddRtd" max-width="600px">
              <v-card>
                <v-toolbar density="compact" color="primary">
                  <v-toolbar-title>Add RTD Sensor</v-toolbar-title>
                </v-toolbar>
                
                <v-card-text class="pt-4">
                  <v-container>
                    <v-row>
                      <v-col cols="12" md="6">
                        <v-text-field 
                          v-model="newRtdSensor.name" 
                          label="Sensor Name"
                          hint="Give your RTD sensor a descriptive name"
                          persistent-hint />
                      </v-col>
                      <v-col cols="12" md="6">
                        <v-select 
                          v-model="newRtdSensor.sensorType" 
                          :items="rtdSensorTypes"
                          label="RTD Sensor Type"
                          hint="Select PT100 or PT1000 based on your sensor"
                          persistent-hint />
                      </v-col>
                    </v-row>
                    
                    <v-row>
                      <v-col cols="12" md="6">
                        <v-text-field 
                          v-model.number="newRtdSensor.csPin" 
                          type="number"
                          label="SPI CS Pin"
                          hint="GPIO pin number for SPI Chip Select"
                          persistent-hint />
                      </v-col>
                      <v-col cols="12" md="6">
                        <v-alert type="info" variant="tonal" class="mb-0">
                          <div class="text-body-2">
                            <strong>Wiring:</strong><br>
                            • Connect MAX31865 CS to GPIO {{ newRtdSensor.csPin }}<br>
                            • MOSI, MISO, CLK pins configured in System Settings
                          </div>
                        </v-alert>
                      </v-col>
                    </v-row>
                    
                    <v-row>
                      <v-col cols="12" md="6">
                        <v-switch 
                          v-model="newRtdSensor.show" 
                          label="Show on Chart"
                          color="green"
                          hint="Display sensor readings on temperature chart"
                          persistent-hint />
                      </v-col>
                      <v-col cols="12" md="6">
                        <v-switch 
                          v-model="newRtdSensor.useForControl" 
                          label="Use for Heating Control"
                          color="red"
                          hint="Use this sensor for PID heating control"
                          persistent-hint />
                      </v-col>
                    </v-row>
                  </v-container>
                </v-card-text>
                
                <v-card-actions>
                  <v-spacer />
                  <v-btn color="grey" variant="text" @click="closeAddRtdDialog">
                    Cancel
                  </v-btn>
                  <v-btn color="primary" variant="text" @click="addRtdSensor">
                    Add RTD Sensor
                  </v-btn>
                </v-card-actions>
              </v-card>
            </v-dialog>
          </v-toolbar>
        </template>
        <template v-slot:[`item.actions`]="{ item }">
          <v-icon size="small" class="me-2" @click="editItem(item)" :icon="mdiPencil" />
          <v-icon size="small" @click="openDeleteDialog(item)" :icon="mdiDelete" />
        </template>
        <template v-slot:[`item.sensorType`]="{ item }">
          <v-chip :color="item.sensorType === 0 ? 'blue' : item.sensorType === 1 ? 'green' : 'orange'" size="small">
            {{ item.sensorType === 0 ? 'DS18B20' : item.sensorType === 1 ? 'PT100' : 'PT1000' }}
          </v-chip>
        </template>
        <template v-slot:[`item.csPin`]="{ item }">
          <span v-if="item.sensorType === 1 || item.sensorType === 2">
            {{ item.csPin || 'N/A' }}
          </span>
          <span v-else class="text-grey">-</span>
        </template>
        <template v-slot:[`item.useForControl`]="{ item }">
          <v-tooltip text="Check to use this sensor for heating control">
            <template v-slot:activator="{ props }">
              <v-checkbox-btn class="align-right justify-center" v-model="item.useForControl" 
                @change="save()" 
                :color="item.useForControl ? 'red' : 'grey'" 
                v-bind="props" />
            </template>
          </v-tooltip>
        </template>
        <template v-slot:[`item.show`]="{ item }">
          <v-checkbox-btn class="align-right justify-center" v-model="item.show" 
            @change="save()" 
            :color="item.show ? 'green' : 'grey'" />
        </template>
        <template v-slot:[`item.connected`]="{ item }">
          <v-chip 
            :color="item.lastTemp === -999 ? 'error' : (item.connected ? 'success' : 'warning')" 
            size="small"
            variant="flat">
            {{ item.lastTemp === -999 ? 'Disconnected' : (item.connected ? 'Connected' : 'Offline') }}
          </v-chip>
        </template>
        <template v-slot:[`item.color`]="{ item }">
          <v-icon size="small" class="me-2" :icon="mdiPalette" :color="item.color" />
        </template>
        <template v-slot:[`item.lastTemp`]="{ item }">
          <span v-if="item.lastTemp === -999" class="text-error">
            Disconnected
          </span>
          <span v-else>
            {{ item.lastTemp.toFixed(1) }}°C
          </span>
        </template>
      </v-data-table>

      <v-btn color="success" class="mt-4 mr-2" @click="save"> {{ t('general.save') }} </v-btn>

    </v-form>
  </v-container>
</template>
