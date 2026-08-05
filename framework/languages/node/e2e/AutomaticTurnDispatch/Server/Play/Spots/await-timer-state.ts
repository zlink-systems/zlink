import type { ZLinkTimer } from '@zlink-systems/framework';

export class AwaitTimerState {
  private tickCount = 0;
  timer?: ZLinkTimer;

  constructor(
    readonly requestId: string,
    readonly timerName: string,
    readonly mode: string,
    readonly delayMs: number
  ) {}

  nextTick(): number {
    this.tickCount += 1;
    return this.tickCount;
  }
}
