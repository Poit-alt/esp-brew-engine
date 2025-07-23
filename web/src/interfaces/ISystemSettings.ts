import type TemperatureScale from "@/enums/TemperatureScale";

export interface ISystemSettings {
  onewirePin: number;
  stirPin: number;
  buzzerPin: number;
  buzzerTime: number;
  invertOutputs: boolean;
  mqttUri: string;
  influxdbUrl: string;
  influxdbToken: string;
  influxdbOrg: string;
  influxdbBucket: string;
  influxdbSendInterval: number;
  temperatureScale: TemperatureScale;
  rtdSensorsEnabled: boolean;
  spiMosiPin: number;
  spiMisoPin: number;
  spiClkPin: number;
  spiCsPin: number;
}
