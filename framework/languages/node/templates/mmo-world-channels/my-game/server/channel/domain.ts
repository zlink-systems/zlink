import { GameRules, Rejection, ActionRejected } from '../shared/messages';

export interface GoldSnapshot {
  gold: bigint;
  cooldownRemainingMs: number;
}

export type GoldDecision =
  | { accepted: true; gold: bigint; earnedAt: number }
  | { accepted: false; rejection: ActionRejected };

export class GoldBalance {
  private current = 0n;
  private lastEarnAt?: number;

  constructor(
    value: bigint,
    cooldownRemainingMs = 0,
    private readonly now: () => number = () => performance.now()
  ) {
    this.restore({ gold: value, cooldownRemainingMs });
  }

  get value(): bigint {
    return this.current;
  }

  previewEarn(): GoldDecision {
    const now = this.now();
    if (this.lastEarnAt !== undefined) {
      const remainingMs = GameRules.earnIntervalMs - (now - this.lastEarnAt);
      if (remainingMs > 0)
        return {
          accepted: false,
          rejection: new ActionRejected(Rejection.earnTooSoon, Math.ceil(remainingMs))
        };
    }
    if (this.current > GameRules.maxInteger - GameRules.earnAmount)
      return {
        accepted: false,
        rejection: new ActionRejected(Rejection.goldLimit, 0)
      };
    return { accepted: true, gold: this.current + GameRules.earnAmount, earnedAt: now };
  }

  commitEarn(decision: Extract<GoldDecision, { accepted: true }>): void {
    this.current = decision.gold;
    this.lastEarnAt = decision.earnedAt;
  }

  capture(): GoldSnapshot {
    const remainingMs =
      this.lastEarnAt === undefined
        ? 0
        : Math.max(0, Math.ceil(GameRules.earnIntervalMs - (this.now() - this.lastEarnAt)));
    return { gold: this.current, cooldownRemainingMs: remainingMs };
  }

  restore(snapshot: GoldSnapshot): void {
    if (
      typeof snapshot.gold !== 'bigint' ||
      snapshot.gold < 0n ||
      snapshot.gold > GameRules.maxInteger ||
      !Number.isSafeInteger(snapshot.cooldownRemainingMs) ||
      snapshot.cooldownRemainingMs < 0 ||
      snapshot.cooldownRemainingMs > GameRules.earnIntervalMs
    )
      throw new Error('Invalid gold snapshot');
    this.current = snapshot.gold;
    this.lastEarnAt =
      snapshot.cooldownRemainingMs === 0
        ? undefined
        : this.now() - (GameRules.earnIntervalMs - snapshot.cooldownRemainingMs);
  }
}
