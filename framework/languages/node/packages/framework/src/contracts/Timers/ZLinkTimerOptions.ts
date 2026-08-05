export interface ZLinkTimerOptions {
  overrunPolicy?: ZLinkTimerOverrunPolicy;
  maxCatchUpTicks?: number;
  stopOnUnhandledException?: boolean;
}

export enum ZLinkTimerOverrunPolicy {
  SkipLateTicks = 'skipLateTicks',
  CatchUpBounded = 'catchUpBounded',
  DelayNextTick = 'delayNextTick'
}
