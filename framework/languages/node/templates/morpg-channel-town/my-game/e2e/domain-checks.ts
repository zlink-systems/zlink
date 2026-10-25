import assert from 'node:assert/strict';
import { Game, Rejection } from '../server/shared/constants';
import { ChannelRules } from '../server/shared/channel-rules';
import { DungeonState, type DungeonSnapshot } from '../server/instance/dungeon-state';
import { PartyRun } from '../server/town/party-run';
import {
  ActionRejected,
  JoinDungeon,
  PrepareDungeon,
  RunPhase
} from '../server/shared/messages';

export function runDomainChecks(): void {
  let checkedCount = 0;
  const check = (condition: boolean, name: string): void => {
    assert(condition, `Domain boundary failed: ${name}`);
    checkedCount++;
  };
  const json = (value: unknown): string => JSON.stringify(value);

  check(
    ChannelRules.validate(Game.maximumInteger + 1n) === Rejection.integerOutOfRange,
    'channel above maximum'
  );
  check(
    ChannelRules.validate(Game.maximumInteger) === Rejection.invalidChannel,
    'channel maximum'
  );
  check(
    ChannelRules.validate(-Game.maximumInteger - 1n) === Rejection.integerOutOfRange,
    'channel below minimum'
  );
  check(ChannelRules.validate(0n) === Rejection.invalidChannel, 'channel zero');
  check(
    ChannelRules.validate(1n) === null && ChannelRules.validate(2n) === null,
    'valid channels'
  );
  check(ChannelRules.validate(3n) === Rejection.invalidChannel, 'channel above count');

  for (const phase of Object.values(RunPhase)) {
    const runId = `run-${'a'.repeat(32)}`;
    for (const action of ['enter', 'prepared', 'started', 'completed'] as const) {
      const sample = new PartyRun(
        `party-${'b'.repeat(32)}`,
        ['leader', 'member'],
        phase,
        phase === RunPhase.Forming ? null : runId
      );
      const snapshot = json(sample.capture());
      const result =
        action === 'enter'
          ? sample.enter('leader')
          : action === 'prepared'
            ? sample.prepared(runId)
            : action === 'started'
              ? sample.started(runId)
              : sample.completed(runId);
      const allowed =
        action === 'enter' ||
        (action === 'prepared' &&
          (phase === RunPhase.Preparing || phase === RunPhase.Entering)) ||
        (action === 'started' && (phase === RunPhase.Entering || phase === RunPhase.Active)) ||
        (action === 'completed' && (phase === RunPhase.Active || phase === RunPhase.Completed));
      check(
        allowed
          ? result === null
          : result === Rejection.runMismatch && json(sample.capture()) === snapshot,
        `transition table ${phase}/${action}`
      );
    }
    const original = new PartyRun(
      `party-${'c'.repeat(32)}`,
      ['leader', 'member'],
      phase,
      phase === RunPhase.Forming ? null : runId
    );
    const encoded = json(original.capture());
    const restored = PartyRun.restore(JSON.parse(encoded) as ReturnType<PartyRun['capture']>);
    check(json(restored.capture()) === encoded, `snapshot ${phase}`);
  }

  const party = new PartyRun(undefined, ['leader', 'member']);
  check(DungeonState.validId(party.partyId, 'party-'), 'party ID');
  check(party.phase === RunPhase.Forming && party.runId === null, 'forming');
  check(party.changeChannel() === null, 'forming channel');
  let before = json(party.capture());
  check(
    party.enter('member') === Rejection.notLeader && json(party.capture()) === before,
    'nonleader unchanged'
  );
  check(party.started('unknown') === Rejection.runMismatch, 'forming start rejected');
  check(party.completed('unknown') === Rejection.runMismatch, 'forming completion rejected');
  check(party.prepared('unknown') === Rejection.runMismatch, 'forming preparation rejected');
  check(party.enter('leader') === null && party.phase === RunPhase.Preparing, 'prepare transition');
  const runId = party.runId!;
  check(DungeonState.validId(runId, 'run-'), 'run ID');
  check(party.enter('leader') === null && party.runId === runId, 'preparing retry');
  check(party.changeChannel() === Rejection.runInProgress, 'preparing channel');
  party.leave('member');
  check(party.members.includes('member'), 'preparing retains member');
  check(party.started(runId) === Rejection.runMismatch, 'preparing start rejected');
  check(party.completed(runId) === Rejection.runMismatch, 'preparing completion rejected');
  check(party.prepared('other') === Rejection.runMismatch, 'wrong preparation');
  check(party.prepared(runId) === null && party.phase === RunPhase.Entering, 'entering transition');
  check(party.prepared(runId) === null, 'prepared retry');
  check(party.enter('leader') === null && party.runId === runId, 'entering retry');
  check(party.completed(runId) === Rejection.runMismatch, 'entering completion rejected');
  check(party.changeChannel() === Rejection.runInProgress, 'entering channel');
  party.leave('member');
  check(party.members.length === 2, 'entering retains member');
  check(party.started(runId) === null && party.phase === RunPhase.Active, 'active transition');
  check(party.started(runId) === null, 'started retry');
  check(party.enter('leader') === null && party.runId === runId, 'active enter unchanged');
  check(party.prepared(runId) === Rejection.runMismatch, 'active prepare rejected');
  check(party.completed('other') === Rejection.runMismatch, 'wrong completion');
  check(party.changeChannel() === Rejection.runInProgress, 'active channel');
  party.leave('member');
  check(party.members.length === 2, 'active retains member');
  check(party.completed(runId) === null && party.phase === RunPhase.Completed, 'completed transition');
  check(party.completed(runId) === null, 'completed retry');
  check(
    party.prepared(runId) === Rejection.runMismatch &&
      party.started(runId) === Rejection.runMismatch,
    'completed rejects old transitions'
  );
  check(party.changeChannel() === Rejection.runInProgress, 'completed channel');
  party.leave('member');
  check(party.members.length === 2, 'completed retains member');
  const partySnapshot = json(party.capture());
  check(
    json(PartyRun.restore(JSON.parse(partySnapshot) as ReturnType<PartyRun['capture']>).capture()) ===
      partySnapshot,
    'party snapshot'
  );
  check(
    party.enter('leader') === null && party.runId !== runId && party.phase === RunPhase.Preparing,
    'new completed run'
  );
  const forming = new PartyRun(undefined, ['leader']);
  forming.leave('leader');
  check(forming.members.length === 0, 'forming leave');

  const dungeon = new DungeonState();
  const target = `dungeon-${runId}`;
  const request = new PrepareDungeon(runId, party.partyId, 'town-2', ['leader', 'member']);
  check(dungeon.advance() === Rejection.runNotActive && dungeon.stage === 0n, 'unprepared advance');
  check(dungeon.prepare(target, request) === null, 'prepare dungeon');
  before = json(dungeon.capture());
  check(dungeon.prepare(target, request) === null && json(dungeon.capture()) === before, 'idempotent prepare');
  check(
    dungeon.prepare(target, new PrepareDungeon(party.runId!, party.partyId, 'town-2', request.members)) ===
      Rejection.runMismatch &&
      json(dungeon.capture()) === before,
    'wrong run unchanged'
  );
  check(
    dungeon.prepare(target, new PrepareDungeon(runId, party.partyId, 'town-2', ['member', 'leader'])) ===
      Rejection.runMismatch &&
      json(dungeon.capture()) === before,
    'wrong members unchanged'
  );
  check(
    dungeon.prepare(target, new PrepareDungeon(runId, party.partyId, 'town-1', request.members)) ===
      Rejection.runMismatch &&
      json(dungeon.capture()) === before,
    'wrong owner unchanged'
  );
  check(
    dungeon.advance() === Rejection.runNotActive && json(dungeon.capture()) === before,
    'entering advance unchanged'
  );
  check(!dungeon.accepts('other', target, new JoinDungeon(target, runId)), 'nonmember admission');
  check(!dungeon.accepts('leader', target, new JoinDungeon(target, 'other')), 'wrong run admission');
  check(dungeon.accepts('leader', target, new JoinDungeon(target, runId)), 'member admission');
  check(!dungeon.join('other') && dungeon.joined.size === 0, 'unknown join unchanged');
  check(!dungeon.join('leader') && dungeon.phase === RunPhase.Entering, 'partial arrival');
  check(!dungeon.join('leader') && dungeon.joined.size === 1, 'duplicate arrival');
  check(dungeon.join('member') && dungeon.phase === RunPhase.Active, 'all arrive');
  check(dungeon.advance() === null && dungeon.stage === 1n, 'advance one');
  check(dungeon.advance() === null && dungeon.stage === 2n, 'advance two');
  check(
    dungeon.advance() === null &&
      dungeon.stage === Game.finalStage &&
      dungeon.phase === RunPhase.Completed,
    'final stage'
  );
  before = json(dungeon.capture());
  check(
    dungeon.advance() === Rejection.runNotActive && json(dungeon.capture()) === before,
    'completed advance unchanged'
  );
  const restoredDungeon = new DungeonState();
  restoredDungeon.restore(JSON.parse(before) as DungeonSnapshot);
  check(json(restoredDungeon.capture()) === before, 'dungeon snapshot');
  dungeon.stage = Game.maximumInteger;
  before = json(dungeon.capture());
  check(
    dungeon.advance() === Rejection.integerOutOfRange && json(dungeon.capture()) === before,
    'integer boundary unchanged'
  );
  dungeon.stage = Game.maximumInteger + 1n;
  check(dungeon.advance() === Rejection.integerOutOfRange, 'integer above bound');
  check(json(RunPhase.Active) === '"Active"', 'enum name');
  check(json(new PartyRun().capture()).includes('"runId":null'), 'nullable run');
  check(new ActionRejected(Rejection.runNotActive).retryAfterMs === 0, 'rejection time');

  assert.equal(checkedCount, 91, 'Domain boundary count differs from .NET');
  console.log(`Domain checks: ${checkedCount}/${checkedCount} passed`);
}
