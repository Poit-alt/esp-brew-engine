<script lang="ts" setup>
import WebConn from "@/helpers/webConn";
import { IBrewSession, ISessionData, IStatisticsResponse } from "@/interfaces/IBrewSession";
import { useAppStore } from "@/store/app";
import { CategoryScale, Chart as ChartJS, Filler, Legend, LineElement, LinearScale, PointElement, TimeScale, Title, Tooltip } from "chart.js";
import "chartjs-adapter-dayjs-4";
import { computed, inject, onMounted, ref } from "vue";
import { Line } from "vue-chartjs";
import { useI18n } from "vue-i18n";
import { useClientStore } from "@/store/client";
import TemperatureScale from "@/enums/TemperatureScale";

const { t } = useI18n({ useScope: "global" });

const webConn = inject<WebConn>("webConn");
const appStore = useAppStore();
const clientStore = useClientStore();

// Register Chart.js components
ChartJS.register(CategoryScale, LinearScale, TimeScale, PointElement, LineElement, Title, Tooltip, Legend, Filler);

// Reactive data
const loading = ref(false);
const statisticsData = ref<IStatisticsResponse | null>(null);
const selectedSession = ref<IBrewSession | null>(null);
const sessionData = ref<ISessionData | null>(null);
const chartLoading = ref(false);
const showSessionDialog = ref(false);
const exportFormat = ref<'json' | 'csv'>('json');
const exportLoading = ref(false);

// Computed properties
const sessions = computed(() => statisticsData.value?.sessions || []);
const stats = computed(() => statisticsData.value?.stats);
const config = computed(() => statisticsData.value?.config);

const formatDuration = (seconds: number): string => {
  const hours = Math.floor(seconds / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  if (hours > 0) {
    return `${hours}h ${minutes}m`;
  }
  return `${minutes}m`;
};

const formatDateTime = (timestamp: number): string => {
  return new Date(timestamp * 1000).toLocaleString();
};

const formatTemperature = (temp: number): string => {
  if (appStore.temperatureScale === TemperatureScale.Fahrenheit) {
    return `${Math.round(temp * 9/5 + 32)}°F`;
  }
  return `${Math.round(temp)}°C`;
};

// Chart data for session details
const chartData = computed(() => {
  if (!sessionData.value?.data) return { datasets: [] };

  const tempData = sessionData.value.data.map(point => ({
    x: point.timestamp * 1000, // Convert to milliseconds
    y: point.avgTemp
  }));

  const targetData = sessionData.value.data.map(point => ({
    x: point.timestamp * 1000,
    y: point.targetTemp
  }));

  return {
    datasets: [
      {
        label: t('temperature'),
        data: tempData,
        borderColor: '#ffffff',
        backgroundColor: 'rgba(255, 255, 255, 0.1)',
        borderWidth: 1,
        fill: false,
        pointRadius: 0,
        tension: 0.1
      },
      {
        label: t('targetTemperature'),
        data: targetData,
        borderColor: 'rgba(0, 158, 85, 1)',
        backgroundColor: 'rgba(0, 158, 85, 0.2)',
        borderWidth: 1,
        fill: true,
        pointRadius: 0,
        tension: 0.1
      }
    ]
  };
});

// Chart options
const chartOptions = computed(() => ({
  responsive: true,
  maintainAspectRatio: false,
  animation: {
    duration: 0
  },
  scales: {
    x: {
      type: 'time' as const,
      time: {
        unit: 'minute' as const,
        displayFormats: {
          minute: 'HH:mm',
          hour: 'HH:mm'
        }
      },
      ticks: {
        color: '#ffffff',
        maxTicksLimit: 10
      },
      grid: {
        color: '#bdbdbc'
      }
    },
    y: {
      type: 'linear' as const,
      suggestedMin: appStore.temperatureScale === TemperatureScale.Celsius ? 60 : 140,
      suggestedMax: appStore.temperatureScale === TemperatureScale.Celsius ? 105 : 220,
      ticks: {
        color: '#ffffff',
        callback: function(value: any) {
          return `${value}°${appStore.temperatureScale === TemperatureScale.Celsius ? 'C' : 'F'}`;
        }
      },
      grid: {
        color: '#bdbdbc'
      }
    }
  },
  plugins: {
    legend: {
      labels: {
        color: '#ffffff'
      }
    },
    tooltip: {
      mode: 'index' as const,
      intersect: false,
      callbacks: {
        title: function(context: any) {
          return new Date(context[0].parsed.x).toLocaleTimeString();
        },
        label: function(context: any) {
          const temp = appStore.temperatureScale === TemperatureScale.Fahrenheit 
            ? Math.round(context.parsed.y * 9/5 + 32) 
            : Math.round(context.parsed.y);
          const unit = appStore.temperatureScale === TemperatureScale.Celsius ? '°C' : '°F';
          return `${context.dataset.label}: ${temp}${unit}`;
        }
      }
    }
  }
}));

// Methods
const loadStatistics = async () => {
  if (!webConn) return;
  
  loading.value = true;
  try {
    const requestData = {
      command: "GetStatistics",
      data: null,
    };
    const apiResult = await webConn.doPostRequest(requestData);
    if (apiResult && apiResult.success) {
      statisticsData.value = apiResult.data as IStatisticsResponse;
    }
  } catch (error) {
    console.error("Failed to load statistics:", error);
  } finally {
    loading.value = false;
  }
};

const loadSessionData = async (session: IBrewSession) => {
  if (!webConn) return;
  
  selectedSession.value = session;
  chartLoading.value = true;
  showSessionDialog.value = true;
  
  try {
    const requestData = {
      command: "GetSessionData",
      data: { sessionId: session.sessionId },
    };
    const apiResult = await webConn.doPostRequest(requestData);
    if (apiResult && apiResult.success) {
      sessionData.value = apiResult.data as ISessionData;
    }
  } catch (error) {
    console.error("Failed to load session data:", error);
  } finally {
    chartLoading.value = false;
  }
};

const exportSession = async (session: IBrewSession) => {
  if (!webConn) return;
  
  exportLoading.value = true;
  try {
    const requestData = {
      command: "ExportSession",
      data: { 
        sessionId: session.sessionId, 
        format: exportFormat.value 
      },
    };
    const apiResult = await webConn.doPostRequest(requestData);
    
    if (apiResult && apiResult.success) {
      const data = apiResult.data.exportData;
      const blob = new Blob([data], { 
        type: exportFormat.value === 'json' ? 'application/json' : 'text/csv' 
      });
      
      const url = window.URL.createObjectURL(blob);
      const link = document.createElement('a');
      link.href = url;
      link.download = `session_${session.sessionId}_${session.scheduleName || 'unnamed'}.${exportFormat.value}`;
      document.body.appendChild(link);
      link.click();
      document.body.removeChild(link);
      window.URL.revokeObjectURL(url);
    }
  } catch (error) {
    console.error("Failed to export session:", error);
  } finally {
    exportLoading.value = false;
  }
};

const closeSessionDialog = () => {
  showSessionDialog.value = false;
  selectedSession.value = null;
  sessionData.value = null;
};

// Lifecycle
onMounted(() => {
  loadStatistics();
});
</script>

<template>
  <v-container fluid>
    <v-row>
      <v-col cols="12">
        <h1 class="text-h4 mb-4">{{ t('statistics') }}</h1>
      </v-col>
    </v-row>

    <!-- Statistics Overview -->
    <v-row v-if="stats">
      <v-col cols="12" md="4">
        <v-card>
          <v-card-title>{{ t('totalSessions') }}</v-card-title>
          <v-card-text class="text-h3">{{ stats.totalSessions }}</v-card-text>
        </v-card>
      </v-col>
      <v-col cols="12" md="4">
        <v-card>
          <v-card-title>{{ t('totalBrewTime') }}</v-card-title>
          <v-card-text class="text-h3">{{ formatDuration(stats.totalBrewTime) }}</v-card-text>
        </v-card>
      </v-col>
      <v-col cols="12" md="4">
        <v-card>
          <v-card-title>{{ t('avgSessionDuration') }}</v-card-title>
          <v-card-text class="text-h3">{{ formatDuration(stats.avgSessionDuration) }}</v-card-text>
        </v-card>
      </v-col>
    </v-row>

    <!-- Current Session Status -->
    <v-row v-if="config?.currentSessionActive">
      <v-col cols="12">
        <v-alert type="info" prominent>
          <v-row align="center">
            <v-col>
              <div class="text-h6">Active Brewing Session</div>
              <div>Session ID: {{ config.currentSessionId }}</div>
              <div>Data Points: {{ config.currentDataPoints }}</div>
            </v-col>
          </v-row>
        </v-alert>
      </v-col>
    </v-row>

    <!-- Sessions Table -->
    <v-row>
      <v-col cols="12">
        <v-card>
          <v-card-title>
            <span>{{ t('brewingSessions') }}</span>
            <v-spacer></v-spacer>
            <v-btn @click="loadStatistics" :loading="loading" variant="outlined" size="small">
              <v-icon start>mdi-refresh</v-icon>
              {{ t('refresh') }}
            </v-btn>
          </v-card-title>
          <v-card-text>
            <v-data-table
              :headers="[
                { title: t('sessionId'), key: 'sessionId', width: '100px' },
                { title: t('scheduleName'), key: 'scheduleName' },
                { title: t('startTime'), key: 'startTime', width: '180px' },
                { title: t('duration'), key: 'duration', width: '100px' },
                { title: t('avgTemp'), key: 'avgTemperature', width: '100px' },
                { title: t('dataPoints'), key: 'dataPoints', width: '120px' },
                { title: t('status'), key: 'completed', width: '100px' },
                { title: t('actions'), key: 'actions', width: '150px', sortable: false }
              ]"
              :items="sessions"
              :loading="loading"
              item-value="sessionId"
              no-data-text="No brewing sessions found"
            >
              <template v-slot:item.scheduleName="{ item }">
                {{ item.scheduleName || t('unnamed') }}
              </template>
              <template v-slot:item.startTime="{ item }">
                {{ formatDateTime(item.startTime) }}
              </template>
              <template v-slot:item.duration="{ item }">
                {{ formatDuration(item.duration) }}
              </template>
              <template v-slot:item.avgTemperature="{ item }">
                {{ formatTemperature(item.avgTemperature) }}
              </template>
              <template v-slot:item.completed="{ item }">
                <v-chip :color="item.completed ? 'success' : 'warning'" size="small">
                  {{ item.completed ? t('completed') : t('incomplete') }}
                </v-chip>
              </template>
              <template v-slot:item.actions="{ item }">
                <v-btn-group density="compact">
                  <v-btn @click="loadSessionData(item)" size="small" variant="outlined">
                    <v-icon>mdi-chart-line</v-icon>
                  </v-btn>
                  <v-menu>
                    <template v-slot:activator="{ props }">
                      <v-btn v-bind="props" size="small" variant="outlined">
                        <v-icon>mdi-download</v-icon>
                      </v-btn>
                    </template>
                    <v-list>
                      <v-list-item @click="exportFormat = 'json'; exportSession(item)">
                        <v-list-item-title>Export JSON</v-list-item-title>
                      </v-list-item>
                      <v-list-item @click="exportFormat = 'csv'; exportSession(item)">
                        <v-list-item-title>Export CSV</v-list-item-title>
                      </v-list-item>
                    </v-list>
                  </v-menu>
                </v-btn-group>
              </template>
            </v-data-table>
          </v-card-text>
        </v-card>
      </v-col>
    </v-row>

    <!-- Session Detail Dialog -->
    <v-dialog v-model="showSessionDialog" max-width="1200px" persistent>
      <v-card v-if="selectedSession">
        <v-card-title>
          Session {{ selectedSession.sessionId }} - {{ selectedSession.scheduleName || t('unnamed') }}
          <v-spacer></v-spacer>
          <v-btn @click="closeSessionDialog" variant="text" icon="mdi-close"></v-btn>
        </v-card-title>
        <v-card-text>
          <v-row>
            <v-col cols="12" md="6">
              <v-list>
                <v-list-item>
                  <v-list-item-title>{{ t('startTime') }}</v-list-item-title>
                  <v-list-item-subtitle>{{ formatDateTime(selectedSession.startTime) }}</v-list-item-subtitle>
                </v-list-item>
                <v-list-item>
                  <v-list-item-title>{{ t('endTime') }}</v-list-item-title>
                  <v-list-item-subtitle>{{ formatDateTime(selectedSession.endTime) }}</v-list-item-subtitle>
                </v-list-item>
                <v-list-item>
                  <v-list-item-title>{{ t('duration') }}</v-list-item-title>
                  <v-list-item-subtitle>{{ formatDuration(selectedSession.duration) }}</v-list-item-subtitle>
                </v-list-item>
              </v-list>
            </v-col>
            <v-col cols="12" md="6">
              <v-list>
                <v-list-item>
                  <v-list-item-title>{{ t('avgTemp') }}</v-list-item-title>
                  <v-list-item-subtitle>{{ formatTemperature(selectedSession.avgTemperature) }}</v-list-item-subtitle>
                </v-list-item>
                <v-list-item>
                  <v-list-item-title>{{ t('tempRange') }}</v-list-item-title>
                  <v-list-item-subtitle>
                    {{ formatTemperature(selectedSession.minTemperature) }} - 
                    {{ formatTemperature(selectedSession.maxTemperature) }}
                  </v-list-item-subtitle>
                </v-list-item>
                <v-list-item>
                  <v-list-item-title>{{ t('dataPoints') }}</v-list-item-title>
                  <v-list-item-subtitle>{{ selectedSession.dataPoints }}</v-list-item-subtitle>
                </v-list-item>
              </v-list>
            </v-col>
          </v-row>

          <!-- Temperature Chart -->
          <v-row v-if="sessionData">
            <v-col cols="12">
              <div style="height: 400px; position: relative;">
                <Line 
                  :data="chartData" 
                  :options="chartOptions"
                  v-if="!chartLoading"
                />
                <v-progress-circular 
                  v-else
                  indeterminate 
                  color="primary"
                  style="position: absolute; top: 50%; left: 50%; transform: translate(-50%, -50%);"
                ></v-progress-circular>
              </div>
            </v-col>
          </v-row>
        </v-card-text>
      </v-card>
    </v-dialog>
  </v-container>
</template>

<style scoped>
.v-data-table {
  background-color: transparent;
}
</style>