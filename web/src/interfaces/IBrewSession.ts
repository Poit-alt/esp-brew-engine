export interface IBrewSession {
  sessionId: number;
  scheduleName: string;
  startTime: number;
  endTime: number;
  duration: number;
  dataPoints: number;
  avgTemperature: number;
  minTemperature: number;
  maxTemperature: number;
  completed: boolean;
}

export interface ISessionDataPoint {
  timestamp: number;
  avgTemp: number;
  targetTemp: number;
  pidOutput: number;
}

export interface ISessionData extends IBrewSession {
  data: ISessionDataPoint[];
}

export interface IStatistics {
  totalSessions: number;
  totalBrewTime: number;
  avgSessionDuration: number;
}

export interface IStatisticsConfig {
  maxSessions: number;
  currentSessionActive: boolean;
  currentSessionId?: number;
  currentDataPoints?: number;
}

export interface IStatisticsResponse {
  sessions: IBrewSession[];
  stats: IStatistics;
  config: IStatisticsConfig;
}