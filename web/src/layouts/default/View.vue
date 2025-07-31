<script lang="ts" setup>
import { mdiAltimeter, mdiChartLine, mdiCookieSettingsOutline, mdiFullscreen, mdiFullscreenExit, mdiHeatingCoil, mdiImport, mdiKnob, mdiReceiptTextClock, mdiThermometer, mdiThermometerLines, mdiWifi, mdiWrenchCogOutline, mdiMemory, mdiSpeedometer } from "@mdi/js";
import { ref, onMounted, onBeforeUnmount, inject } from "vue";
import { useI18n } from "vue-i18n";
import WebConn from '@/helpers/webConn';
const { t } = useI18n({ useScope: "global" });

const drawer = ref(true);
const linksBrewing = ref([
  [mdiKnob, t("links.control"), "control"],
  [mdiImport, t("links.import"), "import"],
]);

const linksTools = ref([
  [mdiAltimeter, t("links.refractometer"), "refractometer"],
  [mdiChartLine, t("links.statistics"), "statistics"],
]);

const linksSettings = ref([
  [mdiReceiptTextClock, t("links.schedules"), "mashschedules"],
  [mdiThermometerLines, t("links.pid_settings"), "pidsettings"],
  [mdiThermometer, t("links.temp_settings"), "tempsettings"],
  [mdiHeatingCoil, t("links.heater"), "heaterSettings"],
  [mdiWifi, t("links.wifi"), "wifiSettings"],
  [mdiWrenchCogOutline, t("links.system"), "systemSettings"],
  [mdiCookieSettingsOutline, t("links.client"), "clientSettings"],
]);

const version = ref<string>(import.meta.env.VITE_APP_VERSION);

const fullscreen = ref<boolean>(false);
const webConn = inject<WebConn>("webConn");

const systemInfo = ref<any>({
  memoryUsagePercent: 0,
  cpuUsagePercent: 0
});
let systemInfoInterval: NodeJS.Timeout;

if (import.meta.env.MODE === "development") {
  version.value = `${import.meta.env.VITE_APP_VERSION}_dev`;
}

const enterFullscreen = () => {
  const elem = document.documentElement;

  elem
    .requestFullscreen({ navigationUI: "hide" })
    .then(() => {
      fullscreen.value = true;
    })
    .catch((err) => {
      console.error(err);
    });
};

const exitFullscreen = () => {
  document.exitFullscreen();
  fullscreen.value = false;
};

const getSystemInfo = async () => {
  try {
    const requestData = {
      command: "Data",
      data: {}
    };
    const result = await webConn?.doPostRequest(requestData);
    if (result?.success && result.data.systemInfo) {
      systemInfo.value = result.data.systemInfo;
    }
  } catch (error) {
    console.error('Failed to get system info:', error);
  }
};

onMounted(() => {
  getSystemInfo();
  systemInfoInterval = setInterval(getSystemInfo, 2000); // Update every 2 seconds
});

onBeforeUnmount(() => {
  if (systemInfoInterval) {
    clearInterval(systemInfoInterval);
  }
});
</script>

<template>
  <v-app>
    <v-app-bar app color="primary">
      <v-app-bar-nav-icon @click="drawer = !drawer" />
      <v-toolbar-title>ESP Brew Engine</v-toolbar-title>
      <v-spacer />
      
      <!-- System Info -->
      <div class="d-flex align-center mr-4">
        <v-tooltip bottom>
          <template v-slot:activator="{ props }">
            <div class="d-flex align-center mr-3" v-bind="props">
              <v-icon size="small" class="mr-1">{{ mdiSpeedometer }}</v-icon>
              <span class="text-caption">{{ systemInfo.cpuUsagePercent }}%</span>
            </div>
          </template>
          <span>CPU Usage</span>
        </v-tooltip>
        
        <v-tooltip bottom>
          <template v-slot:activator="{ props }">
            <div class="d-flex align-center" v-bind="props">
              <v-icon size="small" class="mr-1">{{ mdiMemory }}</v-icon>
              <span class="text-caption">{{ systemInfo.memoryUsagePercent }}%</span>
            </div>
          </template>
          <span>Memory Usage</span>
        </v-tooltip>
      </div>
      
      <h5 class="mr-4">Version: {{ version }}</h5>

      <v-btn icon v-if="!fullscreen" @click="enterFullscreen">
        <v-icon>{{ mdiFullscreen }}</v-icon>
      </v-btn>
      <v-btn icon v-if="fullscreen" @click="exitFullscreen">
        <v-icon>{{ mdiFullscreenExit }}</v-icon>
      </v-btn>
    </v-app-bar>

    <v-navigation-drawer v-model="drawer">
      <v-list>

        <v-list-subheader>{{$t("links.brewing")}}</v-list-subheader>
        <v-list-item v-for="[icon, text, route] in linksBrewing" :key="icon" link :to="route">
          <template v-slot:prepend>
            <v-icon>{{ icon }}</v-icon>
          </template>
          <v-list-item-title>{{ text }}</v-list-item-title>
        </v-list-item>

        <v-list-subheader>{{$t("links.settings")}}</v-list-subheader>
        <v-list-item v-for="[icon, text, route] in linksSettings" :key="icon" link :to="route">
          <template v-slot:prepend>
            <v-icon>{{ icon }}</v-icon>
          </template>
          <v-list-item-title>{{ text }}</v-list-item-title>
        </v-list-item>

        <v-list-subheader>{{$t("links.tools")}}</v-list-subheader>
        <v-list-item v-for="[icon, text, route] in linksTools" :key="icon" link :to="route">
          <template v-slot:prepend>
            <v-icon>{{ icon }}</v-icon>
          </template>
          <v-list-item-title>{{ text }}</v-list-item-title>
        </v-list-item>

      </v-list>

    </v-navigation-drawer>

    <v-main style="min-height: 300px;">

      <router-view v-slot="{ Component }">
        <keep-alive include="Control">
          <component :is="Component" />
        </keep-alive>
      </router-view>

    </v-main>
  </v-app>
</template>
