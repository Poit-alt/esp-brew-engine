import type TemperatureScale from "@/enums/TemperatureScale";

export interface ISystemSettings {
  onewirePin: number;
  stirPin: number;
  buzzerPin: number;
  buzzerTime: number;
  invertOutputs: boolean;
  mqttUri: string;
  temperatureScale: TemperatureScale;
  rtdSensorsEnabled: boolean;
  spiMosiPin: number;
  spiMisoPin: number;
  spiClkPin: number;
  spiCsPin: number;
}
