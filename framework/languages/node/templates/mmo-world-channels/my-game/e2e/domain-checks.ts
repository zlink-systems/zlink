import assert from 'node:assert/strict';
import { GameRules, Rejection } from '../server/shared/messages';
import { GoldBalance } from '../server/channel/domain';

export function runDomainChecks(): number {
  let checks = 0;
  let now = 0;
  const balance = new GoldBalance(0n, 0, () => now);
  const accepted = balance.previewEarn();
  assert(accepted.accepted);
  assert.equal(accepted.gold, GameRules.earnAmount);
  checks++;
  assert.equal(balance.value, 0n);
  checks++;
  balance.commitEarn(accepted);
  assert.equal(balance.value, GameRules.earnAmount);
  checks++;

  const early = balance.previewEarn();
  assert(!early.accepted);
  assert.equal(early.rejection.reason, Rejection.earnTooSoon);
  assert.equal(early.rejection.retryAfterMs, GameRules.earnIntervalMs);
  checks++;
  assert.equal(balance.value, GameRules.earnAmount);
  assert.equal(balance.capture().cooldownRemainingMs, GameRules.earnIntervalMs);
  checks++;

  now = 250;
  const snapshot = balance.capture();
  assert.equal(snapshot.cooldownRemainingMs, 750);
  checks++;
  now = 10_000;
  const relocated = new GoldBalance(snapshot.gold, snapshot.cooldownRemainingMs, () => now);
  const restoredCooldown = relocated.previewEarn();
  assert(!restoredCooldown.accepted);
  assert.equal(restoredCooldown.rejection.retryAfterMs, 750);
  checks++;
  now = 10_750;
  const afterCooldown = relocated.previewEarn();
  assert(afterCooldown.accepted);
  assert.equal(afterCooldown.gold, GameRules.earnAmount * 2n);
  checks++;

  const atLimit = new GoldBalance(GameRules.maxInteger - GameRules.earnAmount, 0, () => now);
  const boundary = atLimit.previewEarn();
  assert(boundary.accepted);
  assert.equal(boundary.gold, GameRules.maxInteger);
  checks++;
  atLimit.commitEarn(boundary);
  now += GameRules.earnIntervalMs;
  const atCap = atLimit.previewEarn();
  assert(!atCap.accepted);
  assert.equal(atCap.rejection.reason, Rejection.goldLimit);
  assert.equal(atCap.rejection.retryAfterMs, 0);
  checks++;
  assert.equal(atLimit.value, GameRules.maxInteger);
  checks++;

  const overLimit = new GoldBalance(GameRules.maxInteger - GameRules.earnAmount + 1n, 0, () => now);
  const rejected = overLimit.previewEarn();
  assert(!rejected.accepted);
  assert.equal(rejected.rejection.reason, Rejection.goldLimit);
  assert.equal(rejected.rejection.retryAfterMs, 0);
  checks++;
  assert.equal(overLimit.value, GameRules.maxInteger - GameRules.earnAmount + 1n);
  checks++;
  return checks;
}
