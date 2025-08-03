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
import TemperatureGauge from "@/components/TemperatureGauge.vue";
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

// Simplified schedule state
const isHeatingPhase = ref<boolean>(true);
const currentHoldingDuration = ref<number>(0);
const holdingTimeRemaining = ref<number>(0);

// Session management
const hardwareId = ref<string>("");
const currentTime = ref<number>(Date.now() / 1000); // Reactive current time for timeline updates
const currentBrewSession = ref<string>("");
const sessionActive = ref<boolean>(false);
const showSessionDialog = ref<boolean>(false);
const sessionName = ref<string>("");
const currentSessionName = ref<string>("");
const showSessionHistoryDialog = ref<boolean>(false);
const sessionStartTime = ref<number>(0);

// Brewing step tracking from backend
const currentMashStep = ref<number>(0);
const currentExecutionStep = ref<number>(0);
const statusFilter = ref<string[]>([]);
const sessionHistory = ref<Array<{ 
  id: string; 
  name: string; 
  created: string; 
  lastModified: string;
  brewingState?: string;
  currentStep?: string;
  schedule?: string;
}>>([]);

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

  // Use simplified notification system
  if (sessionActive.value && notifications.value.length > 0) {
    // When session is active, use notifications from the API
    currentNotifications = [...notifications.value];
  } else if (selectedMashSchedule.value !== null && selectedMashSchedule.value.steps !== null && startDateTime.value != null) {
    // when we have no active session, show schedule notifications
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

  // Use simplified mash schedule for chart display
  if (selectedMashSchedule.value !== null && selectedMashSchedule.value.steps !== null && startDateTime.value != null) {
    // Show the selected mash schedule (simplified system - always show schedule)
    if (status.value === "Idle" || status.value === "Running" || status.value === "Paused") {
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
  // Update current time for timeline reactivity
  currentTime.value = Date.now() / 1000;
  
  const requestData = {
    command: "Data",
    data: {
      LastDate: lastGoodDataDate.value,
    },
  };

  const apiResult = await webConn?.doPostRequest(requestData);

  if (apiResult === undefined || apiResult.success === false) {
    console.warn("Failed to get data, preserving existing chart data");
    // Don't clear the chart data when API fails - just skip this update
    // This prevents the graph from disappearing in Access Point mode
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
  
  // Handle session management data
  if (apiResult.data.hardwareId) {
    hardwareId.value = apiResult.data.hardwareId;
  }
  if (apiResult.data.currentBrewSession) {
    currentBrewSession.value = apiResult.data.currentBrewSession;
    sessionActive.value = true;
    
    // Get session name if available
    if (apiResult.data.sessionName) {
      currentSessionName.value = apiResult.data.sessionName;
    }
    
    // Set session start time if not already set
    if (!sessionStartTime.value && apiResult.data.sessionStartTime) {
      sessionStartTime.value = apiResult.data.sessionStartTime;
    }
  } else {
    currentBrewSession.value = "";
    currentSessionName.value = "";
    sessionActive.value = false;
    sessionStartTime.value = 0;
  }
  
  // Handle restored session brewing state
  if (apiResult.data.selectedMashScheduleName) {
    // Find and set the selected mash schedule
    const scheduleToRestore = apiResult.data.selectedMashScheduleName;
    const mashSchedule = appStore.mashSchedules.find((schedule: IMashSchedule) => 
      schedule.name === scheduleToRestore
    );
    if (mashSchedule) {
      selectedMashSchedule.value = mashSchedule;
      console.log('Restored selected mash schedule:', mashSchedule.name);
    } else {
      console.warn('Schedule not found in mashSchedules:', scheduleToRestore);
      console.log('Available schedules:', appStore.mashSchedules.map(s => s.name));
      // If schedules aren't loaded yet, try again after a delay
      setTimeout(() => {
        const retrySchedule = appStore.mashSchedules.find((schedule: IMashSchedule) => 
          schedule.name === scheduleToRestore
        );
        if (retrySchedule) {
          selectedMashSchedule.value = retrySchedule;
          console.log('Restored selected mash schedule on retry:', retrySchedule.name);
        } else {
          console.error('Schedule still not found after retry:', scheduleToRestore);
        }
      }, 2000);
    }
  }
  
  // Handle restored execution steps
  if (apiResult.data.executionSteps && Array.isArray(apiResult.data.executionSteps)) {
    // Convert execution steps from server format to frontend format
    const restoredSteps = apiResult.data.executionSteps.map((step: any) => ({
      time: step.time,
      temperature: step.temperature,
      extendIfNeeded: step.extendIfNeeded || false,
      allowBoost: step.allowBoost || false
    }));
    
    executionSteps.value = restoredSteps;
    console.log('Restored execution steps:', restoredSteps.length, 'steps (legacy - using simplified schedule system)');
    console.log('Timeline visibility conditions: sessionActive=', sessionActive.value, 'schedule.steps.length=', selectedMashSchedule.value?.steps?.length || 0);
  } else {
    console.log('No execution steps in apiResult.data:', !!apiResult.data.executionSteps, Array.isArray(apiResult.data.executionSteps));
  }
  lastGoodDataDate.value = apiResult.data.lastLogDateTime;
  inOverTime.value = apiResult.data.inOverTime;
  boostStatus.value = apiResult.data.boostStatus;
  
  // Update simplified schedule state
  isHeatingPhase.value = apiResult.data.isHeatingPhase ?? true;
  currentHoldingDuration.value = apiResult.data.currentHoldingDuration ?? 0;
  holdingTimeRemaining.value = apiResult.data.holdingTimeRemaining ?? 0;
  
  // Update current step tracking from backend
  if (apiResult.data.currentMashStep !== undefined) {
    currentMashStep.value = apiResult.data.currentMashStep;
    console.log('Updated currentMashStep from backend:', currentMashStep.value);
  }
  if (apiResult.data.currentExecutionStep !== undefined) {
    currentExecutionStep.value = apiResult.data.currentExecutionStep;
    console.log('Updated currentExecutionStep from backend:', currentExecutionStep.value);
  }
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

// Session management functions
const startBrewSession = async () => {
  if (!sessionName.value.trim()) {
    alert("Please enter a session name");
    return;
  }

  // First, start the session
  const sessionRequestData = {
    command: "StartBrewSession",
    data: {
      sessionData: {
        sessionName: sessionName.value.trim(),
        selectedMashSchedule: selectedMashSchedule.value?.name || null,
      },
    },
  };

  const sessionResult = await webConn?.doPostRequest(sessionRequestData);
  if (sessionResult?.success) {
    console.log("Session created successfully:", sessionResult.data);
    
    // Close dialog and reset form
    showSessionDialog.value = false;
    sessionName.value = "";
    
    // Reset data for new session
    currentTemps.value = [];
    executionSteps.value = [];
    rawData.value = [];
    notificationsShown.value = [];
    setStartDateNow();
    
    // Now start the brewing process
    const brewRequestData = {
      command: "Start",
      data: {
        selectedMashSchedule: selectedMashSchedule.value?.name || null,
      },
    };

    const brewResult = await webConn?.doPostRequest(brewRequestData);
    if (brewResult?.success) {
      console.log("Brewing process started successfully");
      // Reset running version so we can get the new schedule
      lastRunningVersion.value = 0;
    } else {
      console.error("Failed to start brewing process:", brewResult);
      alert(`Session "${sessionResult.data?.sessionId || 'created'}" was created successfully, but failed to start the brewing process. You can start brewing manually.`);
    }
  } else {
    console.error("Failed to create session:", sessionResult);
    alert(`Failed to create brew session: ${sessionResult?.message || 'Unknown error'}`);
  }
};

const stopBrewSession = async () => {
  const requestData = {
    command: "StopBrewSession",
    data: null,
  };

  await webConn?.doPostRequest(requestData);
  // todo capture error
};

const pauseBrewSession = async () => {
  // Pause the brewing process but keep session active
  const requestData = {
    command: "Pause",
    data: null,
  };

  await webConn?.doPostRequest(requestData);
  console.log('Brewing process paused - session remains active');
};

const resumeBrewSession = async () => {
  // Resume the brewing process without resetting data
  const requestData = {
    command: "Start",
    data: {
      selectedMashSchedule: selectedMashSchedule.value?.name || null,
    },
  };

  // DON'T reset data when resuming - keep existing execution steps and progress
  console.log('Resuming brewing session with existing data...');
  
  await webConn?.doPostRequest(requestData);
  
  // Force data refresh to get updated status
  setTimeout(async () => {
    await getData();
    console.log('Resume data refresh completed');
  }, 1000);
};

const continueBrewSession = async (sessionId: string) => {
  const requestData = {
    command: "ContinueBrewSession",
    data: {
      sessionId: sessionId,
    },
  };

  const result = await webConn?.doPostRequest(requestData);
  if (result?.success) {
    // DON'T reset data - we want to keep the restored session state
    // Only clear chart-related data that needs fresh loading
    currentTemps.value = [];
    rawData.value = [];
    notificationsShown.value = [];
    lastRunningVersion.value = 0;
    showSessionHistoryDialog.value = false;
    
    console.log('Session loaded successfully - controller in paused state');
    
    // Force immediate data refresh to get the restored session state
    await getData();
    
    // Multiple refreshes to ensure we get the complete restored state
    setTimeout(async () => {
      await getData();
      console.log('Session state restoration completed - ready for manual resume');
    }, 1000);
    
    setTimeout(async () => {
      await getData();
      console.log('Final session state refresh completed');
    }, 3000);
  } else {
    console.error('Failed to resume session:', result?.message);
    alert(`Failed to resume session: ${result?.message || 'Unknown error'}`);
  }
};

const loadSessionHistory = async () => {
  const requestData = {
    command: "GetSessionHistory",
    data: null,
  };

  console.log("Loading session history...");
  const result = await webConn?.doPostRequest(requestData);
  console.log("Session history result:", result);
  
  if (result?.success) {
    if (result.data?.sessions) {
      sessionHistory.value = result.data.sessions;
      console.log("Loaded sessions:", sessionHistory.value);
    } else {
      console.log("No sessions in response data:", result.data);
      sessionHistory.value = [];
    }
  } else {
    console.error("Failed to load session history:", result);
    sessionHistory.value = [];
  }
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

  // Load mash schedules first so they're available for session restoration
  appStore.getMashSchedules();

  // Use faster polling interval (1s) for responsive temperature updates
  // This provides near real-time temperature monitoring
  intervalId.value = setInterval(() => {
    getData();
  }, 1000);

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

// Enhanced UI computed properties
const temperatureColor = computed(() => {
  if (!temperature.value || !targetTemperature.value) return '';
  const diff = Math.abs(temperature.value - targetTemperature.value);
  if (diff <= 0.5) return 'text-success';
  if (diff <= 2.0) return 'text-warning';
  return 'text-error';
});

const brewingProgress = computed(() => {
  if (!selectedMashSchedule.value?.steps?.length) return 0;
  // Use mash schedule steps instead of execution steps
  const totalSteps = selectedMashSchedule.value.steps.length;
  const currentStep = Math.max(0, currentMashStep.value - 1); // Convert to 0-based
  
  return totalSteps > 0 ? ((currentStep + 1) / totalSteps) * 100 : 0;
});

const currentStepDisplay = computed(() => {
  if (!selectedMashSchedule.value?.steps?.length) return '0/0';
  const totalSteps = selectedMashSchedule.value.steps.length;
  const currentStep = Math.max(0, currentMashStep.value - 1); // Convert to 0-based
  return `${currentStep + 1}/${totalSteps}`;
});

const sessionDuration = computed(() => {
  if (!sessionStartTime.value) return '--:--';
  const now = Date.now();
  const duration = Math.floor((now - sessionStartTime.value * 1000) / 1000);
  const hours = Math.floor(duration / 3600);
  const minutes = Math.floor((duration % 3600) / 60);
  return `${hours.toString().padStart(2, '0')}:${minutes.toString().padStart(2, '0')}`;
});

const filteredSessionHistory = computed(() => {
  if (!statusFilter.value.length) return sessionHistory.value;
  return sessionHistory.value.filter(session => 
    statusFilter.value.includes(session.brewingState || 'Unknown')
  );
});

const timelineSteps = computed(() => {
  // Simplified timeline using new heating/holding state
  if (!selectedMashSchedule.value?.steps?.length) return [];
  
  return selectedMashSchedule.value.steps.map((mashStep, index) => {
    const stepNumber = index + 1; // 1-based step numbering
    const isCompleted = stepNumber < currentMashStep.value;
    const isCurrent = stepNumber === currentMashStep.value;
    const isFuture = stepNumber > currentMashStep.value;
    
    let stepProgress = 0;
    let timeRemaining = '';
    let statusText = '';
    
    if (isCurrent) {
      if (isHeatingPhase.value) {
        // Currently heating to target temperature - no timer runs
        statusText = 'Heating...';
        timeRemaining = 'Heating';
        stepProgress = 0; // No progress during heating
      } else {
        // Currently holding at temperature - timer is active
        const totalHoldTime = currentHoldingDuration.value;
        const remainingTime = holdingTimeRemaining.value;
        
        if (totalHoldTime > 0) {
          stepProgress = Math.max(0, Math.min(100, ((totalHoldTime - remainingTime) / totalHoldTime) * 100));
        }
        
        if (status.value === 'Paused') {
          timeRemaining = 'Paused';
          statusText = 'Paused';
        } else if (remainingTime <= 0) {
          timeRemaining = 'Complete';
          statusText = 'Ready to continue';
          stepProgress = 100;
        } else {
          timeRemaining = `${remainingTime}min`;
          statusText = `Holding (${remainingTime}min left)`;
        }
      }
    } else if (isCompleted) {
      stepProgress = 100;
      timeRemaining = 'Complete';
      statusText = 'Completed';
    } else {
      stepProgress = 0;
      timeRemaining = `${mashStep.stepTime}min`;
      statusText = 'Waiting';
    }
    
    return {
      name: `Step ${index + 1}`,
      temperature: Math.round(mashStep.temperature),
      duration: `${mashStep.stepTime}min`, // Only show hold time - no heating time added
      progress: stepProgress,
      timeRemaining: timeRemaining,
      statusText: statusText,
      completed: isCompleted,
      current: isCurrent,
      future: isFuture,
      isStepCompleted: isCurrent && !isHeatingPhase.value && holdingTimeRemaining.value <= 0,
      stepIndex: index,
      extendIfNeeded: mashStep.extendStepTimeIfNeeded || false,
      allowBoost: mashStep.allowBoost || false
    };
  });
});

// Step control functions
const continueToNextStep = async () => {
  try {
    console.log('Before continue - Target temp:', targetTemperature.value);
    const requestData = {
      command: "ContinueToNextStep",
      data: {}
    };
    const result = await webConn?.doPostRequest(requestData);
    if (result?.success) {
      console.log('Continued to next step successfully');
      // Small delay to ensure backend has processed the step change
      setTimeout(async () => {
        await getData(); // Refresh data
        console.log('After continue - Target temp:', targetTemperature.value);
      }, 100);
    } else {
      console.error('Failed to continue to next step:', result?.message);
    }
  } catch (error) {
    console.error('Error continuing to next step:', error);
  }
};

const skipToNextStep = async () => {
  try {
    console.log('Before skip - Target temp:', targetTemperature.value);
    const requestData = {
      command: "SkipToNextStep", 
      data: {}
    };
    const result = await webConn?.doPostRequest(requestData);
    if (result?.success) {
      console.log('Skipped to next step successfully');
      // Small delay to ensure backend has processed the step change
      setTimeout(async () => {
        await getData(); // Refresh data
        console.log('After skip - Target temp:', targetTemperature.value);
      }, 100);
    } else {
      console.error('Failed to skip to next step:', result?.message);
    }
  } catch (error) {
    console.error('Error skipping to next step:', error);
  }
};

// Helper functions
const getStepProgress = (stepString: string) => {
  if (!stepString || stepString === '-') return 0;
  const [current, total] = stepString.split('/').map(Number);
  return total > 0 ? (current / total) * 100 : 0;
};

const formatDate = (dateString: string) => {
  if (!dateString || dateString === 'Unknown') return 'Unknown';
  try {
    // If it's already formatted, return as is
    if (dateString.includes('-') && dateString.includes(':')) {
      return dateString;
    }
    // Otherwise try to parse and format
    const date = new Date(dateString);
    return date.toLocaleString();
  } catch {
    return dateString;
  }
};
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

  <!-- Session Dialog -->
  <v-dialog v-model="showSessionDialog" max-width="400px">
    <v-card>
      <v-toolbar density="compact" color="dialog-header">
        <v-toolbar-title>Start New Brew Session</v-toolbar-title>
      </v-toolbar>

      <v-card-text>
        <v-text-field
          v-model="sessionName"
          label="Session Name"
          placeholder="Enter session name"
          variant="outlined"
          density="compact"
          autofocus
          @keyup.enter="startBrewSession"
        />
      </v-card-text>

      <v-card-actions>
        <v-spacer />
        <v-btn color="blue-darken-1" variant="text" @click="showSessionDialog = false">
          Cancel
        </v-btn>
        <v-btn color="success" variant="text" @click="startBrewSession">
          Start Session
        </v-btn>
      </v-card-actions>
    </v-card>
  </v-dialog>

  <!-- Enhanced Session History Dialog -->
  <v-dialog v-model="showSessionHistoryDialog" max-width="800px">
    <v-card>
      <v-toolbar density="compact" color="primary">
        <v-icon class="mr-2">mdi-history</v-icon>
        <v-toolbar-title>Brewing Session History</v-toolbar-title>
        <v-spacer />
        <v-btn icon @click="loadSessionHistory(); console.log('Refreshing session history')">
          <v-icon>mdi-refresh</v-icon>
        </v-btn>
        <v-btn icon @click="showSessionHistoryDialog = false">
          <v-icon>mdi-close</v-icon>
        </v-btn>
      </v-toolbar>

      <v-card-text class="pa-4">
        <!-- Status Filter Chips -->
        <div class="mb-4">
          <div class="text-subtitle-2 mb-2">Filter by Status:</div>
          <v-chip-group
            v-model="statusFilter"
            column
            multiple
          >
            <v-chip filter variant="outlined" value="Running" color="success">
              <v-icon start>mdi-play</v-icon>
              Running
            </v-chip>
            <v-chip filter variant="outlined" value="Boiling" color="warning">
              <v-icon start>mdi-fire</v-icon>
              Boiling
            </v-chip>
            <v-chip filter variant="outlined" value="Idle" color="grey">
              <v-icon start>mdi-pause</v-icon>
              Completed
            </v-chip>
          </v-chip-group>
        </div>

        <v-data-table
          :headers="[
            { title: 'Session Name', key: 'name', width: '200px' },
            { title: 'Schedule', key: 'schedule', width: '150px' },
            { title: 'Status', key: 'brewingState', width: '100px' },
            { title: 'Progress', key: 'currentStep', width: '80px' },
            { title: 'Last Activity', key: 'lastModified', width: '150px' },
            { title: 'Actions', key: 'actions', sortable: false, width: '120px' }
          ]"
          :items="filteredSessionHistory"
          item-value="id"
          density="compact"
          :items-per-page="10"
          no-data-text="No sessions found matching your filters"
          class="elevation-1"
        >
          <template v-slot:item.name="{ item }">
            <div class="d-flex align-center">
              <v-icon class="mr-2" color="primary">mdi-flask-outline</v-icon>
              <div>
                <div class="font-weight-medium">{{ item.name }}</div>
                <div class="text-caption text-medium-emphasis">{{ item.id.substring(0, 16) }}...</div>
              </div>
            </div>
          </template>
          
          <template v-slot:item.brewingState="{ item }">
            <v-chip
              :color="item.brewingState === 'Running' ? 'success' : 
                     item.brewingState === 'Boiling' ? 'warning' :
                     item.brewingState === 'Idle' ? 'grey' : 'info'"
              size="small"
              variant="tonal"
            >
              <v-icon start size="x-small">
                {{ item.brewingState === 'Running' ? 'mdi-play' :
                   item.brewingState === 'Boiling' ? 'mdi-fire' :
                   item.brewingState === 'Idle' ? 'mdi-check' : 'mdi-help' }}
              </v-icon>
              {{ item.brewingState || 'Unknown' }}
            </v-chip>
          </template>
          
          <template v-slot:item.schedule="{ item }">
            <div class="text-body-2">
              <div>{{ item.schedule || '-' }}</div>
            </div>
          </template>
          
          <template v-slot:item.currentStep="{ item }">
            <div v-if="item.currentStep" class="text-center">
              <div class="text-body-2 font-weight-medium">{{ item.currentStep }}</div>
              <v-progress-linear
                :model-value="getStepProgress(item.currentStep)"
                height="4"
                rounded
                color="primary"
                class="mt-1"
              />
            </div>
            <span v-else class="text-body-2">-</span>
          </template>
          
          <template v-slot:item.lastModified="{ item }">
            <div class="text-body-2">
              {{ formatDate(item.lastModified) }}
            </div>
          </template>
          
          <template v-slot:item.actions="{ item }">
            <div class="d-flex gap-1">
              <v-btn
                color="primary"
                variant="elevated"
                size="small"
                @click="continueBrewSession(item.id)"
                :disabled="item.brewingState === 'Running'"
              >
                <v-icon start size="small">mdi-play</v-icon>
                Resume
              </v-btn>
            </div>
          </template>
        </v-data-table>
      </v-card-text>

      <v-card-actions class="px-4 pb-4">
        <div class="text-caption text-medium-emphasis">
          {{ filteredSessionHistory.length }} session(s) found
        </div>
        <v-spacer />
        <v-btn color="grey" variant="text" @click="showSessionHistoryDialog = false">
          Close
        </v-btn>
      </v-card-actions>
    </v-card>
  </v-dialog>

  <v-container class="spacing-playground pa-6" fluid>
    <v-form fast-fail @submit.prevent>
      <!-- Enhanced Session Status Section -->
      <v-row v-if="hardwareId" class="mb-4">
        <v-col cols="12">
          <v-card variant="outlined" :color="sessionActive ? 'primary' : 'grey'">
            <v-card-title class="d-flex align-center">
              <v-icon class="mr-2">mdi-flask</v-icon>
              Brewing Session Status
              <v-spacer />
              <v-chip
                :color="sessionActive ? 'success' : 'grey'"
                :variant="sessionActive ? 'elevated' : 'outlined'"
                size="small"
              >
                {{ sessionActive ? 'Active' : 'No Session' }}
              </v-chip>
            </v-card-title>
            <v-card-text>
              <v-row>
                <!-- Hardware ID -->
                <v-col cols="12" md="3">
                  <div class="text-caption text-medium-emphasis">Hardware ID</div>
                  <div class="text-body-1 font-weight-medium">{{ hardwareId }}</div>
                </v-col>
                
                <!-- Session Name -->
                <v-col cols="12" md="3">
                  <div class="text-caption text-medium-emphasis">Session Name</div>
                  <div class="text-body-1 font-weight-medium">
                    {{ sessionActive ? (currentSessionName || currentBrewSession) : 'No active session' }}
                  </div>
                </v-col>
                
                <!-- Brewing Status -->
                <v-col cols="12" md="3">
                  <div class="text-caption text-medium-emphasis">Brewing Status</div>
                  <v-chip
                    :color="status === 'Running' ? 'success' : 
                           status === 'Boiling' ? 'warning' :
                           status === 'Idle' ? 'grey' : 'info'"
                    size="small"
                    variant="tonal"
                    class="mt-1"
                  >
                    {{ displayStatus }}
                  </v-chip>
                </v-col>
                
                <!-- Current Schedule -->
                <v-col cols="12" md="3">
                  <div class="text-caption text-medium-emphasis">Active Schedule</div>
                  <div class="text-body-1">
                    {{ selectedMashSchedule?.name || 'None selected' }}
                  </div>
                </v-col>
              </v-row>
              
              <!-- Brewing Progress Bar (when active) -->
              <v-row v-if="sessionActive && selectedMashSchedule" class="mt-2">
                <v-col cols="12">
                  <div class="text-caption text-medium-emphasis mb-2">Brewing Progress</div>
                  <v-progress-linear
                    :model-value="brewingProgress"
                    height="8"
                    rounded
                    :color="status === 'Running' ? 'success' : 'primary'"
                    class="mb-2"
                  />
                  <div class="d-flex justify-space-between text-caption">
                    <span>Step {{ currentStepDisplay }}</span>
                    <span>{{ Math.round(brewingProgress) }}% Complete</span>
                  </div>
                </v-col>
              </v-row>
              
              <!-- Temperature Status -->
              <v-row class="mt-2">
                <v-col cols="6" md="3">
                  <div class="text-caption text-medium-emphasis">Current Temp</div>
                  <div class="text-h6 font-weight-bold" :class="temperatureColor">
                    {{ temperature?.toFixed(1) || '--' }}°{{ appStore.tempUnit }}
                  </div>
                </v-col>
                <v-col cols="6" md="3">
                  <div class="text-caption text-medium-emphasis">Target Temp</div>
                  <div class="text-h6">
                    {{ targetTemperature?.toFixed(1) || '--' }}°{{ appStore.tempUnit }}
                  </div>
                </v-col>
                <v-col cols="6" md="3">
                  <div class="text-caption text-medium-emphasis">Output</div>
                  <div class="text-h6">
                    {{ outputPercent || 0 }}%
                  </div>
                </v-col>
                <v-col cols="6" md="3">
                  <div class="text-caption text-medium-emphasis">Session Time</div>
                  <div class="text-body-1 font-weight-medium">
                    {{ sessionDuration }}
                  </div>
                </v-col>
              </v-row>
            </v-card-text>
          </v-card>
        </v-col>
      </v-row>

      <!-- Brewing Timeline (when session is active) -->
      <v-row v-if="sessionActive && (selectedMashSchedule?.steps?.length || 0) > 0" class="mb-4">
        <v-col cols="12">
          <v-card variant="outlined">
            <v-card-title class="d-flex align-center">
              <v-icon class="mr-2">mdi-timeline</v-icon>
              Brewing Timeline
              <v-spacer />
              <v-chip size="small" color="info">
                {{ currentStepDisplay }} Steps
              </v-chip>
            </v-card-title>
            <v-card-text>
              <div class="timeline-container">
                <div class="d-flex align-center mb-4">
                  <v-progress-linear
                    :model-value="brewingProgress"
                    height="12"
                    rounded
                    :color="status === 'Running' ? 'success' : 'primary'"
                    class="flex-grow-1"
                  />
                  <div class="ml-4 text-h6 font-weight-bold">
                    {{ Math.round(brewingProgress) }}%
                  </div>
                </div>
                
                <!-- Timeline Steps -->
                <div class="timeline-steps">
                  <div 
                    v-for="(step, index) in timelineSteps" 
                    :key="index"
                    class="timeline-step"
                    :class="{ 
                      'step-completed': step.completed, 
                      'step-current': step.current,
                      'step-future': step.future 
                    }"
                  >
                    <div class="step-marker">
                      <v-icon v-if="step.completed" color="success" size="small">mdi-check</v-icon>
                      <v-icon v-else-if="step.current" color="primary" size="small">mdi-clock</v-icon>
                      <div v-else class="step-number">{{ index + 1 }}</div>
                    </div>
                    <div class="step-content">
                      <div class="step-name font-weight-medium">{{ step.name }}</div>
                      <div class="step-details text-caption">
                        {{ step.temperature }}°{{ appStore.tempUnit }}
                        <span v-if="step.duration && step.duration !== 'Final'"> for {{ step.duration }}</span>
                        <v-chip v-if="step.extendIfNeeded" size="x-small" color="info" variant="outlined" class="ml-1">
                          <v-icon start size="x-small">mdi-clock-plus-outline</v-icon>
                          Extend if needed
                        </v-chip>
                      </div>
                      <div v-if="step.current && step.timeRemaining" class="step-timer text-caption text-primary font-weight-medium">
                        {{ step.timeRemaining }}
                      </div>
                      <div v-if="step.current && step.statusText" class="step-status text-caption text-medium-emphasis">
                        {{ step.statusText }}
                      </div>
                      <div v-if="step.current" class="step-progress">
                        <v-progress-linear
                          :model-value="step.progress"
                          height="4"
                          color="primary"
                          class="mt-1"
                        />
                      </div>
                      <!-- Step completion actions -->
                      <div v-if="step.current && step.isStepCompleted" class="step-actions mt-2">
                        <div class="text-caption text-success font-weight-medium mb-2">
                          ✓ Step completed! What would you like to do?
                        </div>
                        <div class="d-flex gap-2">
                          <v-btn
                            size="small"
                            color="success"
                            variant="elevated"
                            @click="continueToNextStep"
                          >
                            <v-icon start size="small">mdi-play</v-icon>
                            Continue
                          </v-btn>
                          <v-btn
                            size="small"
                            color="warning"
                            variant="outlined"
                            @click="skipToNextStep"
                          >
                            <v-icon start size="small">mdi-skip-next</v-icon>
                            Skip
                          </v-btn>
                        </div>
                      </div>
                    </div>
                    <!-- Skip action for all steps -->
                    <div v-if="step.current || step.future" class="step-skip">
                      <v-btn
                        size="small"
                        color="orange"
                        variant="outlined"
                        @click="skipToNextStep"
                        title="Skip to next step"
                      >
                        <v-icon size="small">mdi-skip-next</v-icon>
                        Skip
                      </v-btn>
                    </div>
                  </div>
                </div>
              </div>
            </v-card-text>
          </v-card>
        </v-col>
      </v-row>

      <v-row style="height: 50vh">
        <Line v-if="chartInitDone && chartData" :options="chartOptions" :data="chartData" />
      </v-row>
      
      <!-- Temperature Gauges Display -->
      <v-row v-if="currentSensorStatus.length > 0" class="mb-4">
        <v-col cols="12">
          <v-card variant="outlined">
            <v-card-title class="text-h6 pb-2">
              <v-icon class="mr-2">{{ mdiThermometer }}</v-icon>
              Temperature Sensors
            </v-card-title>
            <v-card-text>
              <v-row justify="center">
                <v-col 
                  v-for="sensor in currentSensorStatus" 
                  :key="sensor.id"
                  cols="12" 
                  sm="6" 
                  md="4" 
                  lg="3"
                  class="d-flex justify-center"
                >
                  <div v-if="sensor.isConnected" class="sensor-gauge-wrapper">
                    <TemperatureGauge
                      :current-temperature="sensor.currentTemp"
                      :min-temp="appStore.temperatureScale === TemperatureScale.Fahrenheit ? 32 : 0"
                      :max-temp="appStore.temperatureScale === TemperatureScale.Fahrenheit ? 212 : 100"
                      :unit="appStore.temperatureScale === TemperatureScale.Fahrenheit ? 'F' : 'C'"
                      :sensor-id="sensor.id"
                      :sensor-name="sensor.name"
                      :last-update="sensor.timeSinceUpdate"
                    />
                  </div>
                  <v-card 
                    v-else
                    color="error" 
                    variant="tonal"
                    class="pa-3 disconnected-sensor"
                    style="min-height: 300px; display: flex; align-items: center; justify-content: center; flex-direction: column;"
                  >
                    <v-icon size="48" class="mb-2">{{ mdiThermometer }}</v-icon>
                    <div class="text-h6 mb-2">{{ sensor.name }}</div>
                    <div class="text-error">Disconnected</div>
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
          <!-- Session Management Buttons -->
          <div v-if="sessionActive && (status === 'Paused' || status === 'Idle')" class="d-flex flex-column gap-2">
            <!-- Session active but paused - can resume or stop -->
            <v-btn color="success" class="mt-4" block @click="resumeBrewSession"> 
              Resume
            </v-btn>
            <v-btn color="error" variant="outlined" block @click="stopBrewSession"> 
              Stop Session
            </v-btn>
          </div>
          <div v-else-if="status === 'Idle'" class="d-flex flex-column gap-2">
            <!-- Default idle state - can start new or resume -->
            <v-btn color="success" class="mt-4" block @click="showSessionDialog = true"> 
              Start New Session 
            </v-btn>
            <v-btn color="info" variant="outlined" block @click="loadSessionHistory(); showSessionHistoryDialog = true"> 
              Resume Previous Session
            </v-btn>
          </div>
          <div v-else class="d-flex flex-column gap-2">
            <!-- Running state - can pause or stop -->
            <v-btn color="warning" class="mt-4" block @click="pauseBrewSession"> 
              Pause
            </v-btn>
            <v-btn color="error" variant="outlined" block @click="stopBrewSession"> 
              Stop Session
            </v-btn>
          </div>
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

<style scoped>
.sensor-gauge-wrapper {
  position: relative;
  display: flex;
  flex-direction: column;
  align-items: center;
}

.disconnected-sensor {
  max-width: 200px;
}

.gap-2 {
  gap: 0.5rem;
}

/* Timeline Styles */
.timeline-container {
  padding: 8px 0;
}

.timeline-steps {
  display: flex;
  flex-direction: column;
  gap: 16px;
  max-height: 300px;
  overflow-y: auto;
}

.timeline-step {
  display: flex;
  align-items: flex-start;
  gap: 12px;
  padding: 8px;
  border-radius: 8px;
  transition: all 0.2s ease;
  position: relative;
}

.timeline-step.step-completed {
  background: rgba(76, 175, 80, 0.1);
  border-left: 3px solid #4caf50;
}

.timeline-step.step-current {
  background: rgba(33, 150, 243, 0.1);
  border-left: 3px solid #2196f3;
  box-shadow: 0 2px 8px rgba(33, 150, 243, 0.2);
}

.timeline-step.step-future {
  background: rgba(158, 158, 158, 0.05);
  border-left: 3px solid #e0e0e0;
}

.step-marker {
  flex-shrink: 0;
  width: 24px;
  height: 24px;
  border-radius: 50%;
  display: flex;
  align-items: center;
  justify-content: center;
  background: #f5f5f5;
  border: 2px solid #e0e0e0;
  margin-top: 2px;
}

.step-completed .step-marker {
  background: #4caf50;
  border-color: #4caf50;
  color: white;
}

.step-current .step-marker {
  background: #2196f3;
  border-color: #2196f3;
  color: white;
}

.step-number {
  font-size: 10px;
  font-weight: 600;
  color: #666;
}

.step-content {
  flex: 1;
  min-width: 0;
}

.step-timer {
  margin-top: 4px;
  font-weight: 600;
}

.step-actions {
  background: rgba(76, 175, 80, 0.05);
  border: 1px solid rgba(76, 175, 80, 0.2);
  border-radius: 6px;
  padding: 8px;
}

.step-skip {
  position: absolute;
  top: 8px;
  right: 8px;
}

.step-name {
  font-size: 14px;
  margin-bottom: 2px;
}

.step-details {
  color: #666;
  margin-bottom: 4px;
}

.step-progress {
  margin-top: 6px;
}

/* Responsive adjustments for gauges */
@media (max-width: 960px) {
  .sensor-gauge-wrapper {
    margin-bottom: 20px;
  }
  
  .timeline-steps {
    max-height: 250px;
  }
  
  .timeline-step {
    gap: 8px;
    padding: 6px;
  }
  
  .step-marker {
    width: 20px;
    height: 20px;
  }
}

</style>
