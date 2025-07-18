<script lang="ts" setup>
import TemperatureScale from "@/enums/TemperatureScale";
import WebConn from "@/helpers/webConn";
import { IDataPacket } from "@/interfaces/IDataPacket";
import { IExecutionStep } from "@/interfaces/IExecutionStep";
import { IMashSchedule } from "@/interfaces/IMashSchedule";
import { ITempLog } from "@/interfaces/ITempLog";
import { ITempSensor } from "@/interfaces/ITempSensor";
import { useAppStore } from "@/store/app";
import { CategoryScale, Chart as ChartJS, Filler, Legend, LineElement, LinearScale, PointElement, TimeScale, Title, Tooltip } from "chart.js";
import "chartjs-adapter-dayjs-4";
import { INotification } from "@/interfaces/INotification";
import { useClientStore } from "@/store/client";
import annotationPlugin from "chartjs-plugin-annotation";
import debounce from "lodash.debounce";
import { computed, inject, onBeforeUnmount, onMounted, ref, watch } from "vue";
import { Line } from "vue-chartjs";
import { useI18n } from "vue-i18n";
import BoostStatus from "@/enums/BoostStatus";
import { mdiThermometer } from "@mdi/js";
const { t } = useI18n({ useScope: "global" });

const webConn = inject<WebConn>("webConn");

const appStore = useAppStore();
const clientStore = useClientStore();

const status = ref<string>();
const stirStatus = ref<string>();
const temperature = ref<number>();
const outputPercent = ref<number>();
const targetTemperature = ref<number>();
const manualOverrideTemperature = ref<number>();
const manualOverrideOutput = ref<number | null>(null);
const inOverTime = ref<boolean>(false);
const boostStatus = ref<BoostStatus>(BoostStatus.Off);

const intervalId = ref<any>();

const notificationDialog = ref<boolean>(false);
const notificationDialogTitle = ref<string>("");
const notificationDialogText = ref<string>("");

const notificationTimeouts = ref<Array<number>>([]);
const notificationsShown = ref<Array<number>>([]);

const chartInitDone = ref(false);

const lastGoodDataDate = ref<number | null>(null);
const lastRunningVersion = ref<number>(0);

const rawData = ref<Array<IDataPacket>>([]);

const tempSensors = ref<Array<ITempSensor>>([]);
const executionSteps = ref<Array<IExecutionStep>>([]);
const notifications = ref<Array<INotification>>([]);

const selectedMashSchedule = ref<IMashSchedule | null>(null);

const currentTemps = ref<Array<ITempLog>>([]);

const startDateTime = ref<number>();
const speechVoice = ref<SpeechSynthesisVoice | null>(null);

const stirInterval = ref<Array<number>>([0, 3]);
const stirMax = ref<number>(6);

const focussedField = ref<string>("");

const dynamicColor = () => {
  const r = Math.floor(Math.random() * 255);
  const g = Math.floor(Math.random() * 255);
  const b = Math.floor(Math.random() * 255);
  return `rgb(${r},${g},${b})`;
};

const beep = async () => {
  // create web audio api context
  const audioCtx = new AudioContext();

  // create Oscillator node
  const oscillator = audioCtx.createOscillator();

  const gainNode = audioCtx.createGain();
  gainNode.gain.value = clientStore.clientSettings.beepVolume; // volume
  gainNode.connect(audioCtx.destination);

  oscillator.type = "square";
  oscillator.frequency.setValueAtTime(2000, audioCtx.currentTime); // value in hertz

  oscillator.connect(gainNode);

  oscillator.start(audioCtx.currentTime);
  oscillator.stop(audioCtx.currentTime + 0.1); // 100ms beep
};

const speakMessage = async (message: string) => {
  if (clientStore.clientSettings.voiceUri == null) {
    return;
  }

  const synth = window.speechSynthesis;
  // the first time we get the voice and store it in a ref
  if (speechVoice.value == null) {
    const foundVoice = synth.getVoices().find((v) => v.voiceURI === clientStore.clientSettings.voiceUri);
    if (foundVoice !== undefined) {
      speechVoice.value = foundVoice;
    }
  }

  // unable to get a valid voice
  if (speechVoice.value == null) {
    return;
  }

  const ssu = new SpeechSynthesisUtterance(message);
  ssu.voice = speechVoice.value;
  ssu.pitch = 1;
  ssu.rate = clientStore.clientSettings.speechRate;
  ssu.volume = clientStore.clientSettings.speechVolume;
  synth.speak(ssu);
};

const showNotificaton = async (notification: INotification, alert: boolean) => {
  // Currently in overtime don't need to show, updated notifications will come after overtime
  if (inOverTime.value) {
    return;
  }

  // Overtime status comes to slow so we get incorrect messages when notifications are right on the step time, we skip them id temp not reached
  if (targetTemperature.value != null && temperature.value != null) {
    if (targetTemperature.value - temperature.value > 0.5) {
      return;
    }
  }

  notificationsShown.value.push(notification.timePoint);

  notificationDialogTitle.value = notification.name;
  notificationDialogText.value = notification.message.replaceAll("\n", "<br/>");
  notificationDialog.value = true;

  if (alert && clientStore.clientSettings.beepEnabled) {
    beep();
  }

  if (alert && clientStore.clientSettings.speechEnabled) {
    // when beep and speech add a small pauze
    if (clientStore.clientSettings.beepEnabled) {
      setTimeout(() => {
        speakMessage(notification.message);
      }, 1500);
    } else {
      speakMessage(notification.message);
    }
  }
};

const chartAnnotations = computed(() => {
  // wait for chartjs init
  if (!chartInitDone.value) {
    return null;
  }

  let currentNotifications: Array<INotification> = [];

  // when we are running notifications that come from schedule api call
  if (executionSteps.value != null && executionSteps.value.length > 0) {
    currentNotifications = [...notifications.value];
  } else if (selectedMashSchedule.value !== null && selectedMashSchedule.value.steps !== null && startDateTime.value != null) {
    // when we have no steps and we are idle we can compute notifications from the schedule settings
    if (status.value === "Idle") {
      const scheduleNotifications = [...selectedMashSchedule.value.notifications];

      currentNotifications = scheduleNotifications.map((notification) => {
        let notificationTime = startDateTime.value!;
        notificationTime += notification.timeFromStart * 60;

        const newNotification = { ...notification };
        newNotification.timePoint = notificationTime;
        return newNotification;
      });
    }
  }

  const annotationData: Array<any> = [];

  currentNotifications.forEach((notification) => {
    const notificationTime = notification.timePoint * 1000;
    const notificationPoint = {
      type: "line",
      xMin: notificationTime,
      xMax: notificationTime,
      borderColor: "rgb(255, 99, 132)",
      borderWidth: 2,
      label: {
        content: notification.name,
        drawTime: "afterDatasetsDraw",
        display: true,
        yAdjust: -110,
        position: "top",
      },
      click(context: any, event: any) {
        showNotificaton(notification, false);
      },
    };
    annotationData.push(notificationPoint);
  });

  return annotationData;
});

const chartData = computed(() => {
  // wait for chartjs init
  if (!chartInitDone.value) {
    return null;
  }
  let scheduleData: Array<any> = [];

  if (executionSteps.value != null && executionSteps.value.length > 0) {
    scheduleData = executionSteps.value.map((step: any) => ({
      x: step.time * 1000,
      y: step.temperature,
    }));
  } else if (selectedMashSchedule.value !== null && selectedMashSchedule.value.steps !== null && startDateTime.value != null) {
    // when we have no steps and we are idle we can show the slected mash schedule
    // if the status is idle we can just project the selectes mash shedule
    if (status.value === "Idle") {
      // sort data on index
      const steps = [...selectedMashSchedule.value.steps];
      steps.sort((a, b) => a.index - b.index);

      let lastTimePoint = startDateTime.value;

      const startPoint = {
        x: lastTimePoint * 1000,
        y: temperature.value,
      };
      scheduleData.push(startPoint);

      steps.forEach((step) => {
        lastTimePoint += step.stepTime * 60;
        const startStepPoint = {
          x: lastTimePoint * 1000,
          y: step.temperature,
        };
        scheduleData.push(startStepPoint);

        lastTimePoint += step.time * 60;
        const endStepPoint = {
          x: lastTimePoint * 1000,
          y: step.temperature,
        };
        scheduleData.push(endStepPoint);
      });
    }
  }

  // Find Control/Average data from sensorTempLogs (special sensor ID: 18446744073709551615)
  const avgSensorId = "18446744073709551615"; // 0xFFFFFFFFFFFFFFFF as string
  const avgSensorData = currentTemps.value.find((ct) => ct.sensor === avgSensorId);
  
  let realData: Array<{x: number, y: number}> = [];
  if (avgSensorData && avgSensorData.temps) {
    realData = avgSensorData.temps
      .filter((temp) => temp.temp !== -999) // Filter out invalid readings
      .map((temp) => ({
        x: temp.time * 1000,
        y: temp.temp,
      }));
  }

  // also add the current value, out controller doesn't send identical temp point for performance reasons
  if (lastGoodDataDate.value != null && temperature.value != null && realData.length > 0) {
    const lastDataTime = realData[realData.length - 1]?.x || 0;
    const currentTime = Date.now();
    
    // Only add current value if more than 1 second has passed since last data point
    if (currentTime - lastDataTime > 1000) {
      realData.push({
        x: currentTime,
        y: temperature.value,
      });
    }
  }

  let datasets = [
    {
      label: `${t("control.avg")} ${appStore.tempUnit}`,
      backgroundColor: "rgba(255, 255, 255, 0.7)",
      borderColor: "rgba(255, 255, 255, 0.9)",
      lineTension: 0,
      fill: false,
      pointRadius: 0,
      xAxisID: "xAxis",
      yAxisID: "yAxis",
      data: realData,
    },
    {
      label: `${t("control.target")} ${appStore.tempUnit}`,
      backgroundColor: "rgba(0, 158, 85, 0.3)",
      borderColor: "rgba(0, 158, 85, 0.9)",
      lineTension: 0,
      xAxisID: "xAxis",
      yAxisID: "yAxis",
      fill: true,
      data: scheduleData,
    },
  ];

  const extraDataSets = currentTemps.value
    .filter((extraSet) => extraSet.sensor !== avgSensorId) // Exclude Control/Average sensor
    .map((extraSet) => {
      // Filter out disconnected sensor readings (temp === -999)
      const setData = extraSet.temps
        .filter((temp) => temp.temp !== -999)
        .map((temp) => ({
          x: temp.time * 1000,
          y: temp.temp,
        }));

      let label = extraSet.sensor;
      let { color } = extraSet;
      const sensor = tempSensors.value.find((s) => s.id === extraSet.sensor);

      if (sensor !== undefined) {
        label = sensor.name;
        color = sensor.color;
      }

      const dataset = {
        label,
        backgroundColor: color,
        borderColor: color,
        lineWidth: 0.2,
        lineTension: 0,
        xAxisID: "xAxis",
        yAxisID: "yAxis",
        fill: false,
        pointRadius: 0,
        data: setData,
      };

      return dataset;
    }).filter(dataset => dataset.data.length > 0); // Remove datasets with no valid data points

  datasets = [...datasets, ...extraDataSets];

  return {
    labels: [],
    datasets,
  };
});

const currentSensorStatus = computed(() => {
  const sensorStatus: Array<{
    id: string;
    name: string;
    color: string;
    currentTemp: number;
    lastUpdateTime: number;
    timeSinceUpdate: string;
    isConnected: boolean;
  }> = [];

  // Get all sensors that are configured to show on the chart
  const displaySensors = tempSensors.value.filter(sensor => sensor.show);

  displaySensors.forEach(sensor => {
    // Find the current temperature data for this sensor
    const sensorData = currentTemps.value.find(ct => ct.sensor === sensor.id);
    
    let currentTemp = sensor.lastTemp || 0;
    let lastUpdateTime = 0;
    let isConnected = currentTemp !== -999;

    if (sensorData && sensorData.temps.length > 0) {
      // Get the most recent temperature reading
      const latestReading = sensorData.temps[sensorData.temps.length - 1];
      currentTemp = latestReading.temp;
      lastUpdateTime = latestReading.time;
      isConnected = currentTemp !== -999;
    }

    // Calculate time since last update
    const now = Math.floor(Date.now() / 1000);
    const secondsSinceUpdate = lastUpdateTime > 0 ? now - lastUpdateTime : 0;
    
    let timeSinceUpdate = "";
    if (secondsSinceUpdate < 60) {
      timeSinceUpdate = `${secondsSinceUpdate}s ago`;
    } else if (secondsSinceUpdate < 3600) {
      timeSinceUpdate = `${Math.floor(secondsSinceUpdate / 60)}m ago`;
    } else {
      timeSinceUpdate = `${Math.floor(secondsSinceUpdate / 3600)}h ago`;
    }

    sensorStatus.push({
      id: sensor.id,
      name: sensor.name,
      color: sensor.color,
      currentTemp: isConnected ? currentTemp : -999,
      lastUpdateTime,
      timeSinceUpdate: lastUpdateTime > 0 ? timeSinceUpdate : "No data",
      isConnected
    });
  });

  return sensorStatus;
});

const clearAllNotificationTimeouts = () => {
  notificationTimeouts.value.forEach((timeOutId) => {
    window.clearTimeout(timeOutId);
  });

  notificationTimeouts.value = [];
};

const setNotifications = (newNotifications: Array<INotification>) => {
  clearAllNotificationTimeouts();

  const timeoutIds: Array<number> = [];

  // Notifications we have not show yet
  newNotifications
    .filter((n) => notificationsShown.value.includes(n.timePoint) === false)
    .forEach((notification) => {
      const timeTill = notification.timePoint * 1000 - Date.now();
      // We do want past notification due to overtime, but these are verry short in the past! max 10 seconds
      if (timeTill > -10000) {
        const timeoutId = window.setTimeout(() => {
          showNotificaton(notification, true);
        }, timeTill);
        timeoutIds.push(timeoutId);
      }
    });

  notificationTimeouts.value = timeoutIds;

  notifications.value = newNotifications;
};

const getRunningSchedule = async () => {
  const requestData = {
    command: "GetRunningSchedule",
    data: null,
  };

  const apiResult = await webConn?.doPostRequest(requestData);

  if (apiResult === undefined || apiResult.success === false) {
    return;
  }

  executionSteps.value = apiResult.data.steps;
  setNotifications(apiResult.data.notifications as Array<INotification>);

  lastRunningVersion.value = apiResult.data.version;
};

const getData = async () => {
  const requestData = {
    command: "Data",
    data: {
      LastDate: lastGoodDataDate.value,
    },
  };

  const apiResult = await webConn?.doPostRequest(requestData);

  if (apiResult === undefined || apiResult.success === false) {
    return;
  }

  status.value = apiResult.data.status;
  stirStatus.value = apiResult.data.stirStatus;
  temperature.value = apiResult.data.temp;
  outputPercent.value = apiResult.data.output;
  manualOverrideOutput.value = apiResult.data.manualOverrideOutput;

  if (focussedField.value !== "manualOverrideTemperature") {
    manualOverrideTemperature.value = apiResult.data.manualOverrideTargetTemp;
  }

  targetTemperature.value = apiResult.data.targetTemp;
  lastGoodDataDate.value = apiResult.data.lastLogDateTime;
  inOverTime.value = apiResult.data.inOverTime;
  boostStatus.value = apiResult.data.boostStatus;
  const serverRunningVersion = apiResult.data.runningVersion;

  // notifications move with overtime and will be re-added when it is done
  if (inOverTime.value) {
    clearAllNotificationTimeouts();
  }

  if (status.value === "Running" && lastRunningVersion.value !== serverRunningVersion) {
    // the schedule has changed, we need to update
    getRunningSchedule();
  }

  const tempData = [...rawData.value, ...apiResult.data.tempLog];

  // sort data, chartjs seems todo weird things otherwise
  tempData.sort((a, b) => a.time - b.time);

  rawData.value = tempData;

  // Handle individual sensor temperature logs with persistent storage
  if (apiResult.data.sensorTempLogs !== null && apiResult.data.sensorTempLogs.length > 0) {
    apiResult.data.sensorTempLogs.forEach((sensorLog: any) => {
      // Find existing record for this sensor
      const foundRecord = currentTemps.value.find((ct: any) => ct.sensor === sensorLog.sensor);
      
      if (foundRecord === undefined) {
        // Create new record with all historical data
        const newRecord: ITempLog = {
          sensor: sensorLog.sensor,
          color: dynamicColor(),
          temps: sensorLog.temps.map((temp: any) => ({
            time: temp.time,
            temp: temp.temp,
          })),
        };
        currentTemps.value.push(newRecord);
      } else {
        // Merge new temperature data with existing data
        const existingTemps = foundRecord.temps || [];
        const newTemps = sensorLog.temps.map((temp: any) => ({
          time: temp.time,
          temp: temp.temp,
        }));
        
        // Combine and sort by time to ensure proper ordering
        const combinedTemps = [...existingTemps, ...newTemps];
        combinedTemps.sort((a, b) => a.time - b.time);
        
        // Remove duplicates based on time
        const uniqueTemps = combinedTemps.filter((temp, index, arr) => 
          index === 0 || temp.time !== arr[index - 1].time
        );
        
        foundRecord.temps = uniqueTemps;
      }
    });
  } else if (apiResult.data.temps !== null) {
    // Fallback to old behavior for current temperature readings only
    const timestampSeconds = Math.floor(Date.now() / 1000);
    
    apiResult.data.temps.forEach((te: any) => {
      // find record in templog and add
      const foundRecord = currentTemps.value.find((ct: any) => ct.sensor === te.sensor);
      if (foundRecord === undefined) {
        const newRecord: ITempLog = {
          sensor: te.sensor,
          color: dynamicColor(),
          temps: [
            {
              time: timestampSeconds,
              temp: te.temp,
            },
          ],
        };

        currentTemps.value.push(newRecord);
      } else {
        foundRecord.temps.push({
          time: timestampSeconds,
          temp: te.temp,
        });
      }
    });
  }

  // we only need to get the tempsensort once
  if (tempSensors.value == null || tempSensors.value.length === 0) {
    const requestData3 = {
      command: "GetTempSettings",
      data: null,
    };
    const apiResult3 = await webConn?.doPostRequest(requestData3);

    if (apiResult3 === undefined || apiResult3.success === false) {
      return;
    }

    tempSensors.value = apiResult3.data;
  }
};

const changeTargetTemp = async () => {
  if (manualOverrideTemperature.value === undefined) {
    return;
  }

  // for some reason value is still a string while ref defined as number, bug in vue?
  const forceInt = Number.parseInt(manualOverrideTemperature.value?.toString(), 10);

  const requestData = {
    command: "SetTemp",
    data: {
      targetTemp: forceInt,
    },
  };

  webConn?.doPostRequest(requestData);
  // todo capture error
};

const changeOverrideOutput = (event: any) => {
  if (event.target.value === undefined) {
    return;
  }

  const forceInt = Number.parseInt(event.target.value.toString(), 10);

  const requestData = {
    command: "SetOverrideOutput",
    data: {
      output: forceInt,
    },
  };

  webConn?.doPostRequest(requestData);
  // todo capture error
};

const setStartDateNow = () => {
  const now = new Date();
  startDateTime.value = Math.floor(now.getTime() / 1000);
};

const start = async () => {
  const requestData = {
    command: "Start",
    data: {
      selectedMashSchedule: null as string | null,
    },
  };

  // reset all our data so we can start over
  currentTemps.value = [];
  executionSteps.value = [];
  rawData.value = [];
  notificationsShown.value = [];
  setStartDateNow();

  if (selectedMashSchedule.value != null) {
    requestData.data.selectedMashSchedule = selectedMashSchedule.value?.name;
  }

  await webConn?.doPostRequest(requestData);
  // todo capture error
  // reset out running version so we can definitly get the new schedule
  lastRunningVersion.value = 0;
};

const stop = async () => {
  const requestData = {
    command: "Stop",
    data: null,
  };

  clearAllNotificationTimeouts();

  webConn?.doPostRequest(requestData);
  // todo capture error
};

const startStir = async () => {
  const requestData = {
    command: "StartStir",
    data: {
      max: stirMax.value,
      intervalStart: stirInterval.value[0],
      intervalStop: stirInterval.value[1],
    },
  };

  await webConn?.doPostRequest(requestData);
  // todo capture error
};

const stopStir = async () => {
  const requestData = {
    command: "StopStir",
    data: null,
  };

  webConn?.doPostRequest(requestData);
  // todo capture error
};

const debounceTargetTemp = debounce(changeTargetTemp, 1000);

watch(() => manualOverrideTemperature.value, debounceTargetTemp);

watch(selectedMashSchedule, () => {
  // reset all our data so we can start over
  currentTemps.value = [];
  executionSteps.value = [];
  rawData.value = [];
  setStartDateNow();
});

const initChart = () => {
  ChartJS.register(Title, Tooltip, Legend, PointElement, LineElement, TimeScale, LinearScale, CategoryScale, Filler, annotationPlugin);
  chartInitDone.value = true;
};

const chartOptions = computed<any>(() => {
  let suggestedMin = 60;
  let suggestedMax = 105;
  // ajust min max when farenheit
  if (appStore.temperatureScale === TemperatureScale.Fahrenheit) {
    suggestedMin = 140;
    suggestedMax = 220;
  }

  const options = {
    responsive: true,
    maintainAspectRatio: false,
    animation: false, // Disable all animations, does weird things when adding data
    scales: {
      xAxis: {
        title: {
          display: true,
          text: t("control.time"),
          color: "#ffffff",
        },
        type: "time",
        time: {
          unit: "minute",
          displayFormats: {
            millisecond: "HH:mm",
            second: "HH:mm",
            minute: "HH:mm",
            hour: "HH:mm",
            day: "HH:mm",
            week: "HH:mm",
            month: "HH:mm",
            quarter: "HH:mm",
            year: "HH:mm",
          },
        },
        ticks: {
          color: "#ffffff",
        },
      },
      yAxis: {
        title: {
          display: true,
          text: appStore.tempUnit,
          color: "#ffffff",
        },
        type: "linear",
        suggestedMin,
        suggestedMax,
        ticks: {
          color: "#ffffff",
        },
        grid: { color: "#bdbdbc" },
      },
    },
    plugins: {
      annotation: {
        annotations: chartAnnotations.value,
      },
    },
  };

  return options;
});

onMounted(() => {
  // atm only used to render te schedule at the current time
  setStartDateNow();

  intervalId.value = setInterval(() => {
    getData();
  }, 3000);

  initChart();
});

onBeforeUnmount(() => {
  clearAllNotificationTimeouts();
  clearInterval(intervalId.value);
});

const displayStatus = computed(() => {
  let ds = status.value;

  if (inOverTime.value) {
    ds += " (Overtime)";
  }

  if (boostStatus.value === BoostStatus.Boost) {
    ds += " (Boost)";
  }

  if (boostStatus.value === BoostStatus.Rest) {
    ds += " (Boost Rest)";
  }

  return ds;
});

const labelTargetTemp = computed(() => {
  if (selectedMashSchedule.value == null) {
    return `${t("control.set_target")} (${appStore.tempUnit})`;
  }

  return `${t("control.set_target_override")} (${appStore.tempUnit})`;
});
</script>

<template>
  <v-dialog v-model="notificationDialog" max-width="500px">
    <v-card>
      <v-toolbar density="compact" color="dialog-header">
        <v-toolbar-title>{{ notificationDialogTitle }}</v-toolbar-title>
      </v-toolbar>

      <v-card-text v-html="notificationDialogText" />

      <v-card-actions>
        <v-spacer />
        <v-btn color="blue-darken-1" variant="text" @click="notificationDialog = false">
          Close
        </v-btn>
      </v-card-actions>
    </v-card>
  </v-dialog>

  <v-container class="spacing-playground pa-6" fluid>
    <v-form fast-fail @submit.prevent>
      <v-row style="height: 50vh">
        <Line v-if="chartInitDone && chartData" :options="chartOptions" :data="chartData" />
      </v-row>
      
      <!-- Sensor Status Display -->
      <v-row v-if="currentSensorStatus.length > 0" class="mb-4">
        <v-col cols="12">
          <v-card variant="outlined">
            <v-card-title class="text-h6 pb-2">
              <v-icon class="mr-2">{{ mdiThermometer }}</v-icon>
              Temperature Sensors
            </v-card-title>
            <v-card-text>
              <v-row>
                <v-col 
                  v-for="sensor in currentSensorStatus" 
                  :key="sensor.id"
                  cols="12" 
                  sm="6" 
                  md="4" 
                  lg="3"
                >
                  <v-card 
                    :color="sensor.isConnected ? 'surface-variant' : 'error'" 
                    variant="tonal"
                    class="pa-3"
                  >
                    <div class="d-flex align-center mb-2">
                      <v-chip 
                        :color="sensor.color" 
                        size="small" 
                        class="mr-2"
                      >
                        ●
                      </v-chip>
                      <span class="text-body-2 font-weight-medium">{{ sensor.name }}</span>
                    </div>
                    <div class="text-h6">
                      <span v-if="sensor.isConnected">
                        {{ sensor.currentTemp.toFixed(1) }}°{{ appStore.tempUnit || 'C' }}
                      </span>
                      <span v-else class="text-error">
                        Disconnected
                      </span>
                    </div>
                    <div class="text-caption text-medium-emphasis">
                      {{ sensor.timeSinceUpdate }}
                    </div>
                  </v-card>
                </v-col>
              </v-row>
            </v-card-text>
          </v-card>
        </v-col>
      </v-row>
      
      <v-row>
        <v-col cols="12" md="3">
          <v-text-field v-model="displayStatus" readonly :label="$t('control.status')" />
        </v-col>
        <v-col cols="12" md="3">
          <v-text-field v-model="temperature" readonly :label="`${$t('control.temperature')} (${appStore.tempUnit})`" />
        </v-col>
        <v-col cols="12" md="3">
          <v-text-field v-model="targetTemperature" readonly :label="`${$t('control.target')} (${appStore.tempUnit})`" />
        </v-col>
        <v-col cols="12" md="3">
          <v-text-field v-model="manualOverrideTemperature" @focus="focussedField = 'manualOverrideTemperature'" @blur="focussedField = ''" type="number" :label="labelTargetTemp" />
        </v-col>
      </v-row>
      <v-row>
        <v-col cols="12" md="6">
          <v-select
            :label="$t('control.mashSchedule')"
            :readonly="status !== 'Idle'"
            v-model="selectedMashSchedule"
            :items="appStore.mashSchedules"
            item-title="name"
            :filled="appStore.mashSchedules"
            :clearable="status === 'Idle'"
            return-object />
        </v-col>
        <v-col cols="12" md="3">
          <v-text-field
            v-model.number="outputPercent"
            type="number"
            :label="$t('control.output')"
            readonly />
        </v-col>
        <v-col cols="12" md="3">
          
        </v-col>

      </v-row>
      <v-row>
        <v-col cols="12" md="6">
          <v-btn v-if="status === 'Idle'" color="success" class="mt-4" block @click="start"> {{ $t('control.start') }} </v-btn>
          <v-btn v-else color="error" class="mt-4" block @click="stop"> {{ $t('control.stop') }} </v-btn>
        </v-col>
        <v-col cols="12" md="3">
          <v-text-field
            v-model.number="manualOverrideOutput"
            type="number"
            :label="$t('control.override_output')"
            readonly />
        </v-col>
        <v-col cols="12" md="3">
          <v-text-field type="number" :label="$t('control.set_override_output')" @change="changeOverrideOutput" />
        </v-col>

      </v-row>

      <div class="text-subtitle-2 mt-4 mb-2">{{ $t('control.stir_control') }}</div>
      <v-divider :thickness="7" />

      <v-row>
        <v-col cols="12" md="3">
          <v-text-field v-model="stirStatus" readonly :label="$t('control.status')" />
        </v-col>
      </v-row>

      <v-row>

        <v-col cols="12" md="">

          <v-range-slider
            v-model="stirInterval"
            :label="$t('control.interval')"
            step="1"
            thumb-label="always"
            :max="stirMax">
            <template v-slot:append>
              <v-text-field
                v-model.number="stirMax"
                hide-details
                single-line
                type="number"
                variant="outlined"
                style="width: 70px"
                density="compact"
                :label="$t('control.timespan')" />
            </template>
          </v-range-slider>

        </v-col>
      </v-row>

      <v-row>
        <v-col cols="12" md="6">
          <v-btn v-if="stirStatus === 'Idle'" color="success" class="mt-4" block @click="startStir"> {{ $t('control.start') }} </v-btn>
          <v-btn v-else color="error" class="mt-4" block @click="stopStir"> {{ $t('control.stop') }} </v-btn>
        </v-col>
      </v-row>

    </v-form>
  </v-container>
</template>
