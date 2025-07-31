export interface ITempSensor {
  id: string; // js doesn't support uint64_t, so we convert to string
  color: string;
  name: string;
  show: boolean;
  useForControl: boolean;
  connected: boolean;
  compensateAbsolute: number;
  compensateRelative: number;
  lastTemp: number;
  sensorType: number; // 0 = DS18B20, 1 = PT100, 2 = PT1000
  csPin?: number; // CS pin for RTD sensors (optional, only for RTD sensors)
}
