import assert from 'node:assert/strict';
import { GameRules, Rejection } from '../server/shared/messages';
import { GoldBalance } from '../server/channel/domain';

export function runDomainChecks(): number {
  let checks = 0;
  let now = 0;
  const balance = new GoldBalance(0, 0, () => now);
  const accepted = balance.previewEarn();
  assert(accepted.accepted);
  balance.commitEarn(accepted);
  checks++;
  assert.equal(balance.value, GameRules.earnAmount);
  checks++;

  now = 600;
  const early = balance.previewEarn();
  assert(!early.accepted);
  assert.equal(early.rejection.reason, Rejection.earnTooSoon);
  assert.equal(early.rejection.retryAfterMs, 400);
  assert.equal(balance.value, GameRules.earnAmount);
  checks++;

  now = 999;
  const lastMs = balance.previewEarn();
  assert(!lastMs.accepted);
  assert.equal(lastMs.rejection.retryAfterMs, 1);
  checks++;
  now = 1_000;
  const next = balance.previewEarn();
  assert(next.accepted);
  balance.commitEarn(next);
  assert.equal(balance.value, 2 * GameRules.earnAmount);
  checks++;

  now = 1_500;
  const snapshot = balance.capture();
  assert.equal(snapshot.cooldownRemainingMs, 500);
  checks++;
  now = 10_000;
  const relocated = new GoldBalance(snapshot.gold, snapshot.cooldownRemainingMs, () => now);
  const restoredCooldown = relocated.previewEarn();
  assert(!restoredCooldown.accepted);
  assert.equal(restoredCooldown.rejection.retryAfterMs, 500);
  checks++;
  now = 10_500;
  const afterCooldown = relocated.previewEarn();
  assert(afterCooldown.accepted);
  relocated.commitEarn(afterCooldown);
  checks++;

  const atLimit = new GoldBalance(GameRules.maxInteger - GameRules.earnAmount, 0, () => now);
  const boundary = atLimit.previewEarn();
  assert(boundary.accepted);
  atLimit.commitEarn(boundary);
  assert.equal(atLimit.value, GameRules.maxInteger);
  checks++;

  const overLimit = new GoldBalance(GameRules.maxInteger - GameRules.earnAmount + 1, 0, () => now);
  const rejected = overLimit.previewEarn();
  assert(!rejected.accepted);
  assert.equal(rejected.rejection.reason, Rejection.goldLimit);
  assert.equal(rejected.rejection.retryAfterMs, 0);
  assert.equal(overLimit.value, GameRules.maxInteger - GameRules.earnAmount + 1);
  checks++;
  return checks;
}
