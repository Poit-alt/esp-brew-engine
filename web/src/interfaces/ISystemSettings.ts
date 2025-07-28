import type TemperatureScale from "@/enums/TemperatureScale";

export interface ISystemSettings {
  onewirePin: number;
  stirPin: number;
  buzzerPin: number;
  buzzerTime: number;
  invertOutputs: boolean;
  mqttUri: string;
  firebaseUrl: string;
  firebaseApiKey: string;
  firebaseAuthToken: string;
  firebaseEmail: string;
  firebasePassword: string;
  firebaseAuthMethod: string; // "email" or "token"
  firebaseSendInterval: number;
  firebaseDatabaseEnabled: boolean;
  temperatureScale: TemperatureScale;
  rtdSensorsEnabled: boolean;
  spiMosiPin: number;
  spiMisoPin: number;
  spiClkPin: number;
  spiCsPin: number;
}
