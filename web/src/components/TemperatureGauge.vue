<template>
  <div class="temperature-gauge" :class="{ 'dark-theme': theme.current.value.dark }">
    <!-- Sensor name at top -->
    <div v-if="sensorName" class="sensor-info">
      <div class="sensor-name-display">{{ sensorName }}</div>
      <div v-if="lastUpdate" class="last-update-display">{{ lastUpdate }}</div>
    </div>

    <div class="vertical-gauge-container">
      <div class="gauge-and-prediction-wrapper">
        <div class="scale-container">
          <!-- Moving temperature scale -->
          <div 
            class="temperature-scale"
            :style="{
              transform: `translateY(${getScaleOffset()}px)`,
              transition: 'transform 0.3s ease'
            }"
          >
            <!-- Major scale marks -->
            <div
              v-for="temp in majorMarks"
              :key="`major-${temp}`"
              class="scale-mark major"
              :style="{
                position: 'absolute',
                top: `${(maxTemp - temp) * pixelsPerDegree}px`,
              }"
            >
              <div class="scale-line major-line"></div>
              <div class="scale-text">{{ temp }}°</div>
            </div>
            
            <!-- Minor scale marks -->
            <div
              v-for="temp in minorMarks"
              :key="`minor-${temp}`"
              class="scale-mark minor"
              :style="{
                position: 'absolute',
                top: `${(maxTemp - temp) * pixelsPerDegree}px`,
              }"
            >
              <div class="scale-line minor-line"></div>
            </div>
          </div>
          
          <!-- Center needle (fixed position) -->
          <div class="center-needle">
            <div class="needle-line"></div>
          </div>
        </div>
        
        <!-- Predictive arrow system -->
        <div class="prediction-arrow-system" v-if="predictionDirection !== 'stable'">
          <!-- Prediction line -->
          <div 
            class="prediction-line"
            :style="{
              position: 'absolute',
              left: '45px',
              top: predictionDirection === 'up' ? `${predictedPosition}px` : '120px',
              height: `${Math.abs(predictedPosition - 120)}px`,
              width: '2px',
              background: trendColor,
              zIndex: 20,
              transition: 'all 0.3s ease'
            }"
          />
          
          <!-- Arrow head -->
          <div
            class="prediction-arrowhead"
            :style="{
              position: 'absolute',
              left: '42px',
              top: `${predictedPosition - 2}px`,
              width: '8px',
              height: '4px',
              background: trendColor,
              clipPath: predictionDirection === 'up' 
                ? 'polygon(50% 0%, 0% 100%, 100% 100%)'
                : 'polygon(0% 0%, 100% 0%, 50% 100%)',
              zIndex: 21,
              transition: 'all 0.3s ease'
            }"
          />
          
          <!-- Prediction label -->
          <div
            class="prediction-label-vertical"
            :style="{
              position: 'absolute',
              left: '46px',
              top: predictionDirection === 'up' 
                ? `${predictedPosition - 14}px` 
                : `${predictedPosition + 6}px`,
              fontSize: '6px',
              color: '#495057',
              background: 'rgba(255, 255, 255, 0.95)',
              padding: '1px 2px',
              borderRadius: '2px',
              whiteSpace: 'nowrap',
              zIndex: 22,
              transition: 'all 0.3s ease',
              boxShadow: '0 1px 3px rgba(0, 0, 0, 0.1)',
              transform: 'translateX(-50%)'
            }"
          >
            {{ predictedTemperature.toFixed(1) }}°
          </div>
        </div>
      </div>

      <!-- Digital display -->
      <div class="digital-display">
        <div class="temperature-value">
          {{ currentTemperature.toFixed(1) }}°{{ unit }}
        </div>
        
        
        <!-- Time selector dropdown -->
        <div class="prediction-time-dropdown">
          <select 
            class="time-select"
            v-model="predictionTime"
          >
            <option :value="10">10s</option>
            <option :value="60">1m</option>
            <option :value="300">5m</option>
            <option :value="600">10m</option>
          </select>
        </div>
        
        <div class="trend-display">
          <div class="trend-info">
            <div class="prediction-info">
              <span class="prediction-label">{{ formatTime(predictionTime) }}:</span>
              <span class="prediction-value">
                {{ predictedTemperature.toFixed(1) }}°
              </span>
            </div>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, watch, onMounted, onUnmounted } from 'vue'
import { useTheme } from 'vuetify'

interface TemperatureReading {
  temperature: number
  timestamp: number
}

interface Props {
  currentTemperature: number
  minTemp?: number
  maxTemp?: number
  unit?: 'C' | 'F'
  sensorId?: string
  sensorName?: string
  lastUpdate?: string
}

const props = withDefaults(defineProps<Props>(), {
  minTemp: 0,
  maxTemp: 100,
  unit: 'C',
  sensorId: '',
  sensorName: '',
  lastUpdate: ''
})

const theme = useTheme()

const readings = ref<TemperatureReading[]>([])
const timeInterval = ref<10 | 60>(10) // 10 seconds or 1 minute
const predictionTime = ref<10 | 60 | 300 | 600>(10) // 10s, 1m, 5m, 10m
const trend = ref<'rising' | 'falling' | 'stable'>('stable')
const trendValue = ref<number>(0)

// Constants for scale calculation
const pixelsPerDegree = computed(() => 480 / (props.maxTemp - props.minTemp))

// Generate scale marks
const majorMarks = computed(() => {
  const marks = []
  const step = 5 // Use 5° steps for good granularity
  
  for (let temp = props.minTemp; temp <= props.maxTemp; temp += step) {
    marks.push(temp)
  }
  
  return marks
})

const minorMarks = computed(() => {
  const marks = []
  
  for (let temp = props.minTemp; temp <= props.maxTemp; temp += 1) {
    if (temp % 5 !== 0) { // Don't include major marks
      marks.push(temp)
    }
  }
  
  return marks
})

// Calculate scale offset to center current temperature
const getScaleOffset = () => {
  // Center temperature at 120px (half of 240px height)
  const currentTempPosition = (props.maxTemp - props.currentTemperature) * pixelsPerDegree.value
  // Offset needed to move that position to center (120px = half of 240px)
  return 120 - currentTempPosition
}

const trendColor = computed(() => {
  switch (trend.value) {
    case 'rising': return '#e74c3c'  // Red
    case 'falling': return '#3498db' // Blue
    default: return '#95a5a6'        // Gray
  }
})

// Calculate predicted temperature
const predictedTemperature = computed(() => {
  if (readings.value.length < 2) return props.currentTemperature

  const now = Date.now()
  const relevantReadings = readings.value.filter(
    reading => reading.timestamp > now - (timeInterval.value * 1000)
  )

  if (relevantReadings.length < 2) return props.currentTemperature

  // Calculate rate of change per second
  const oldest = relevantReadings[0]
  const newest = relevantReadings[relevantReadings.length - 1]
  const tempChange = newest.temperature - oldest.temperature
  const timeChange = (newest.timestamp - oldest.timestamp) / 1000 // seconds
  const ratePerSecond = tempChange / timeChange

  // Predict temperature for selected time period
  const predictedTemp = props.currentTemperature + (ratePerSecond * predictionTime.value)
  
  // For longer predictions, add some constraints based on physics
  let constrainedTemp = predictedTemp
  if (predictionTime.value >= 60) {
    // For longer predictions, consider boiling point constraints
    if (predictedTemp > 100 && props.currentTemperature < 100) {
      // Can't exceed boiling point significantly
      constrainedTemp = Math.min(predictedTemp, 102)
    }
  }
  
  // Clamp to reasonable bounds
  return Math.max(props.minTemp - 5, Math.min(props.maxTemp + 5, constrainedTemp))
})

// Calculate predicted position
const predictedPosition = computed(() => {
  const totalRange = props.maxTemp - props.minTemp
  const pixelsPerDegree = 480 / totalRange
  
  // Calculate absolute position on the scale (flipped axis)
  const absolutePosition = (props.maxTemp - predictedTemperature.value) * pixelsPerDegree
  
  // Apply the same offset as the temperature scale to get screen position
  const scaleOffset = getScaleOffset()
  return absolutePosition + scaleOffset
})

// Get prediction direction
const predictionDirection = computed(() => {
  const tempDiff = predictedTemperature.value - props.currentTemperature
  
  if (Math.abs(tempDiff) < 0.5) { // Less than 0.5° difference
    return 'stable'
  } else if (tempDiff > 0) {
    return 'up' // Temperature rising
  } else {
    return 'down' // Temperature falling
  }
})

// Format time helper
const formatTime = (seconds: number) => {
  if (seconds < 60) return `${seconds}s`
  if (seconds < 3600) return `${seconds / 60}m`
  return `${seconds / 3600}h`
}

// Add new temperature reading when currentTemperature changes
watch(() => props.currentTemperature, (newTemp) => {
  const now = Date.now()
  readings.value.push({ temperature: newTemp, timestamp: now })
  
  // Keep only readings within the selected time interval
  const cutoffTime = now - (timeInterval.value * 1000)
  readings.value = readings.value.filter(reading => reading.timestamp > cutoffTime)
})

// Calculate trend
watch([readings, timeInterval], () => {
  if (readings.value.length < 2) {
    trend.value = 'stable'
    trendValue.value = 0
    return
  }

  const now = Date.now()
  const relevantReadings = readings.value.filter(
    reading => reading.timestamp > now - (timeInterval.value * 1000)
  )

  if (relevantReadings.length < 2) {
    trend.value = 'stable'
    trendValue.value = 0
    return
  }

  // Calculate average rate of change
  const oldest = relevantReadings[0]
  const newest = relevantReadings[relevantReadings.length - 1]
  const tempChange = newest.temperature - oldest.temperature
  const timeChange = (newest.timestamp - oldest.timestamp) / 1000 // seconds
  
  const ratePerSecond = tempChange / timeChange
  const ratePerMinute = ratePerSecond * 60

  trendValue.value = Math.abs(ratePerMinute)

  if (Math.abs(ratePerSecond) < 0.01) { // Less than 0.01°/second = stable
    trend.value = 'stable'
  } else if (ratePerSecond > 0) {
    trend.value = 'rising'
  } else {
    trend.value = 'falling'
  }
}, { deep: true })

// Initialize with first reading
onMounted(() => {
  readings.value.push({
    temperature: props.currentTemperature,
    timestamp: Date.now()
  })
})
</script>

<style scoped>
.temperature-gauge {
  background: rgba(255, 255, 255, 0.98);
  border: 1px solid rgba(0, 0, 0, 0.08);
  border-radius: 16px;
  padding: 16px;
  box-shadow: 0 4px 20px rgba(0, 0, 0, 0.06);
  color: #2c3e50;
  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
  max-width: 200px;
  margin: 0 auto;
  backdrop-filter: blur(10px);
  overflow: hidden;
}

.gauge-header {
  display: flex;
  flex-direction: column;
  align-items: center;
  margin-bottom: 6px;
  gap: 0;
}

.gauge-header h3 {
  margin: 0;
  color: #495057;
  font-size: 0.7rem;
  font-weight: 600;
  letter-spacing: 1px;
  text-transform: uppercase;
}

.vertical-gauge-container {
  display: flex;
  gap: 16px;
  align-items: center;
  justify-content: center;
  background: transparent;
  padding: 0;
  overflow: hidden;
  flex: 1;
}

.gauge-and-prediction-wrapper {
  position: relative;
  display: flex;
  align-items: center;
  overflow: hidden;
}

.scale-container {
  position: relative;
  width: 70px;
  height: 240px;
  background: rgba(248, 249, 250, 0.5);
  border: 1px solid rgba(0, 0, 0, 0.06);
  border-radius: 6px;
  overflow: hidden;
  margin: 0 auto;
}

.temperature-scale {
  position: absolute;
  left: 0;
  width: 100%;
  height: 960px;
  pointer-events: none;
}

.scale-mark {
  position: absolute;
  width: 100%;
  display: flex;
  align-items: center;
  will-change: transform;
}

.scale-mark.major {
  z-index: 2;
}

.scale-mark.minor {
  z-index: 1;
}

.scale-line {
  background: #ecf0f1;
  height: 2px;
  box-shadow: 0 1px 2px rgba(0, 0, 0, 0.3);
}

.major-line {
  width: 12px;
  margin-left: 0px;
  background: rgba(44, 62, 80, 0.8);
  height: 1px;
}

.minor-line {
  width: 6px;
  margin-left: 3px;
  background: rgba(44, 62, 80, 0.3);
  height: 1px;
}

.scale-text {
  position: absolute;
  left: 18px;
  font-size: 8px;
  font-weight: 500;
  color: #495057;
  width: 40px;
  text-align: left;
  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
}

.center-needle {
  position: absolute;
  top: 50%;
  left: 0;
  right: 0;
  transform: translateY(-50%);
  z-index: 10;
  pointer-events: none;
}

.needle-line {
  position: absolute;
  left: 0;
  width: 30px;
  height: 2px;
  background: #2c3e50;
  border-radius: 0;
  box-shadow: none;
}

.prediction-arrow-system {
  position: absolute;
  top: 0;
  left: 0;
  width: 70px;
  height: 240px;
  pointer-events: none;
  z-index: 20;
  overflow: hidden;
}

.prediction-time-dropdown {
  margin-top: 4px;
}

.time-select {
  background: rgba(255, 255, 255, 0.9);
  border: 1px solid rgba(0, 0, 0, 0.1);
  border-radius: 4px;
  padding: 2px 6px;
  font-size: 0.6rem;
  color: #495057;
  cursor: pointer;
  font-weight: 500;
  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
  outline: none;
  transition: all 0.2s ease;
}

.time-select:hover {
  border-color: rgba(52, 152, 219, 0.3);
  background: rgba(255, 255, 255, 1);
}

.time-select:focus {
  border-color: #3498db;
  box-shadow: 0 0 0 2px rgba(52, 152, 219, 0.1);
}

.digital-display {
  background: transparent;
  border-radius: 8px;
  padding: 6px;
  min-width: 100px;
  text-align: center;
}

.temperature-value {
  font-size: 1.8rem;
  font-weight: 300;
  color: #2c3e50;
  margin-bottom: 6px;
  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
  letter-spacing: -1px;
  white-space: nowrap;
}

.sensor-info {
  margin-bottom: 6px;
  text-align: center;
}

.sensor-name-display {
  font-size: 0.75rem;
  font-weight: 600;
  color: #495057;
  margin-bottom: 2px;
  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  width: 100%;
  text-align: center;
}

.last-update-display {
  font-size: 0.65rem;
  color: #6c757d;
  font-style: italic;
  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
  text-align: center;
  width: 100%;
}

.trend-display {
  text-align: center;
}

.trend-info {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 3px;
}

.prediction-info {
  margin-top: 6px;
  padding-top: 4px;
  border-top: 1px solid rgba(0, 0, 0, 0.06);
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 1px;
}

.prediction-info .prediction-label {
  font-size: 0.55rem;
  color: #868e96;
  font-weight: 400;
  letter-spacing: 0.3px;
  text-transform: uppercase;
}

.prediction-info .prediction-value {
  font-size: 0.8rem;
  font-weight: 500;
  color: #495057;
  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
}

.prediction-label-vertical {
  font-weight: 600;
  font-size: 8px !important;
  background: rgba(255, 255, 255, 0.95) !important;
  color: #495057 !important;
  border: none !important;
  box-shadow: 0 1px 3px rgba(0, 0, 0, 0.1) !important;
  backdrop-filter: blur(4px);
}

/* Add subtle fade effects to contain overflow */
.scale-container::before {
  content: '';
  position: absolute;
  top: 0;
  left: 0;
  right: 0;
  height: 20px;
  background: linear-gradient(to bottom, rgba(248, 249, 250, 0.8), rgba(248, 249, 250, 0));
  z-index: 5;
  pointer-events: none;
}

.scale-container::after {
  content: '';
  position: absolute;
  bottom: 0;
  left: 0;
  right: 0;
  height: 20px;
  background: linear-gradient(to top, rgba(248, 249, 250, 0.8), rgba(248, 249, 250, 0));
  z-index: 5;
  pointer-events: none;
}

/* Animation for smooth transitions */
.temperature-scale,
.temperature-value {
  transition: all 0.3s ease;
}

/* Dark theme styles */
.temperature-gauge.dark-theme {
  background: rgba(36, 35, 32, 0.98);
  border: 1px solid rgba(255, 255, 255, 0.08);
  color: #ffffff;
}

.temperature-gauge.dark-theme .gauge-header h3 {
  color: #ffffff;
}

.temperature-gauge.dark-theme .scale-container {
  background: rgba(60, 60, 60, 0.5);
  border: 1px solid rgba(255, 255, 255, 0.06);
}

.temperature-gauge.dark-theme .major-line {
  background: rgba(255, 255, 255, 0.8);
}

.temperature-gauge.dark-theme .minor-line {
  background: rgba(255, 255, 255, 0.3);
}

.temperature-gauge.dark-theme .scale-text {
  color: #ffffff;
}

.temperature-gauge.dark-theme .needle-line {
  background: #ffffff;
}

.temperature-gauge.dark-theme .temperature-value {
  color: #ffffff;
}

.temperature-gauge.dark-theme .sensor-name-display {
  color: #ffffff;
}

.temperature-gauge.dark-theme .last-update-display {
  color: #adb5bd;
}

.temperature-gauge.dark-theme .time-select {
  background: rgba(60, 60, 60, 0.9);
  border: 1px solid rgba(255, 255, 255, 0.1);
  color: #ffffff;
}

.temperature-gauge.dark-theme .time-select:hover {
  border-color: rgba(255, 164, 7, 0.3);
  background: rgba(60, 60, 60, 1);
}

.temperature-gauge.dark-theme .time-select:focus {
  border-color: #ffa407;
  box-shadow: 0 0 0 2px rgba(255, 164, 7, 0.1);
}

.temperature-gauge.dark-theme .prediction-info {
  border-top: 1px solid rgba(255, 255, 255, 0.06);
}

.temperature-gauge.dark-theme .prediction-info .prediction-label {
  color: #adb5bd;
}

.temperature-gauge.dark-theme .prediction-info .prediction-value {
  color: #ffffff;
}

.temperature-gauge.dark-theme .prediction-label-vertical {
  background: rgba(60, 60, 60, 0.95) !important;
  color: #ffffff !important;
  box-shadow: 0 1px 3px rgba(0, 0, 0, 0.3) !important;
}

.temperature-gauge.dark-theme .scale-container::before {
  background: linear-gradient(to bottom, rgba(60, 60, 60, 0.8), rgba(60, 60, 60, 0));
}

.temperature-gauge.dark-theme .scale-container::after {
  background: linear-gradient(to top, rgba(60, 60, 60, 0.8), rgba(60, 60, 60, 0));
}
</style>