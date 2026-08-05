export interface ZLinkTimerTick {
  readonly name: string;
  readonly deliveryIndex: bigint;
  readonly scheduledIndex: bigint;
  readonly periodMs: number;
  readonly scheduledAt: Date;
  readonly startedAt: Date;
  readonly scheduledElapsedMs: number;
  readonly startedElapsedMs: number;
  readonly delayMs: number;
  readonly skippedTicks: bigint;
}
