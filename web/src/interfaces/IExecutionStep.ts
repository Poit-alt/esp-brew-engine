export interface IExecutionStep {
  temperature: number;
  time: number;
  extendIfNeeded: boolean;
  allowBoost: boolean;
  isHoldingStep: boolean;
}
