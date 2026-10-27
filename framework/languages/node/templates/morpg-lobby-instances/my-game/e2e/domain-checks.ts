import assert from 'node:assert/strict';
import { PartyState } from '../server/lobby/domain';
import { DungeonState } from '../server/instance/domain';
import { newPartyId, newRunId, RunPhase, MaximumInteger, FinalStage, PrepareDungeon, JoinDungeon } from '../server/shared/messages';
import { Rejection } from '../server/shared/constants';
export function domainChecks(): void {
  let count = 0;
  const check = (value: boolean, name: string): void => { assert(value, name); count++; };
  const snap = (value: unknown): string => JSON.stringify(value, (_, v) => typeof v === 'bigint' ? v.toString() : v);
  const party = (phase = RunPhase.Forming): PartyState => Object.assign(new PartyState(), { partyId: newPartyId(), members: ['leader', 'member'], ready: ['leader', 'member'], phase, runId: phase === RunPhase.Forming ? null : newRunId() });
  check(/^party-[0-9a-f]{32}$/.test(party().partyId), 'party ID');
  check(/^run-[0-9a-f]{32}$/.test(party(RunPhase.Active).runId!), 'run ID');
  check(MaximumInteger === 9007199254740991n && FinalStage === 3n, 'bounds');
  for (const phase of Object.values(RunPhase)) {
    let p = party(phase), before = snap(p);
    check(p.enter('member') === Rejection.notLeader && snap(p) === before, phase + ' non-leader');
    p.ready = ['leader']; before = snap(p);
    check(p.enter('leader') === Rejection.notReady && snap(p) === before, phase + ' not-ready');
    p.ready.push('member'); let old = p.runId;
    check(p.enter('leader') === null, phase + ' leader');
    check([RunPhase.Forming, RunPhase.Completed].includes(phase) ? p.phase === RunPhase.Preparing && p.runId !== old : p.phase === phase && p.runId === old, phase + ' entry');
    old = p.runId; check(p.enter('leader') === null && p.runId === old, phase + ' repeated');
    p = party(phase); before = snap(p);
    check(p.prepared('wrong') === Rejection.runMismatch && snap(p) === before, phase + ' prepared mismatch');
    check((p.prepared(p.runId ?? '') === null) === [RunPhase.Preparing, RunPhase.Entering].includes(phase), phase + ' prepared table');
    p = party(phase); before = snap(p);
    check(p.started('wrong') === Rejection.runMismatch && snap(p) === before, phase + ' started mismatch');
    check((p.started(p.runId ?? '') === null) === [RunPhase.Entering, RunPhase.Active].includes(phase), phase + ' started table');
    p = party(phase); before = snap(p);
    check(p.completed('wrong') === Rejection.runMismatch && snap(p) === before, phase + ' completed mismatch');
    check((p.completed(p.runId ?? '') === null) === [RunPhase.Active, RunPhase.Completed].includes(phase), phase + ' completed table');
    p = party(phase); p.leave('member');
    check(p.members.includes('member') === (phase !== RunPhase.Forming), phase + ' leave');
    check(snap(Object.assign(new PartyState(), JSON.parse(snap(p)))) === snap(p), phase + ' snapshot');
    const d = Object.assign(new DungeonState(), { phase }); before = snap(d);
    check((d.advance() === null) === (phase === RunPhase.Active), phase + ' advance table');
    check(phase === RunPhase.Active || snap(d) === before, phase + ' advance unchanged');
  }
  let d = new DungeonState(); const request = new PrepareDungeon(party(RunPhase.Active).runId!, party().partyId, ['leader', 'member']);
  check(d.prepare(request) === null, 'prepare'); let snapshot = snap(d);
  check(d.prepare(request) === null && snap(d) === snapshot, 'idempotent');
  check(d.prepare({ ...request, runId: 'other' }) === Rejection.runMismatch && snap(d) === snapshot, 'run mismatch');
  check(d.prepare({ ...request, members: ['leader'] }) === Rejection.runMismatch && snap(d) === snapshot, 'roster mismatch');
  check(d.prepare({ ...request, partyId: 'other' }) === Rejection.runMismatch && snap(d) === snapshot, 'party mismatch');
  check(!d.admit('outsider', new JoinDungeon('dungeon-' + request.runId, request.runId), 'dungeon-' + request.runId), 'admit roster');
  check(!d.join('outsider') && snap(d) === snapshot, 'outsider unchanged');
  check(!d.join('leader') && d.phase === RunPhase.Entering, 'partial');
  check(!d.join('leader') && d.phase === RunPhase.Entering, 'duplicate');
  check(d.join('member') && d.phase === RunPhase.Active, 'all arrived');
  check(!d.join('member') && d.phase === RunPhase.Active, 'started idempotence');
  for (let stage = 1n; stage <= FinalStage; stage++) check(d.advance() === null && d.stage === stage && d.phase === (stage === FinalStage ? RunPhase.Completed : RunPhase.Active), 'stage ' + stage);
  snapshot = snap(d); check(d.advance() === Rejection.runNotActive && snap(d) === snapshot, 'completed unchanged');
  check(snap(DungeonState.restore(d.capture())) === snapshot, 'dungeon snapshot');
  for (const stage of [-1n, MaximumInteger, MaximumInteger + 1n, 9223372036854775807n]) {
    d = Object.assign(new DungeonState(), { stage, phase: RunPhase.Active }); snapshot = snap(d);
    check(d.advance() === Rejection.integerOutOfRange && snap(d) === snapshot, 'integer ' + stage);
  }
  const arrivals = party(RunPhase.Entering); let arrivalSnapshot = snap(arrivals);
  check(arrivals.arrived('wrong', 'member') === Rejection.runMismatch && snap(arrivals) === arrivalSnapshot, 'arrival run mismatch');
  check(arrivals.arrived(arrivals.runId!, 'outsider') === Rejection.runMismatch && snap(arrivals) === arrivalSnapshot, 'arrival roster mismatch');
  check(arrivals.arrived(arrivals.runId!, 'member') === null && arrivals.entered.includes('member'), 'arrival recorded');
  arrivalSnapshot = snap(arrivals);
  check(arrivals.arrived(arrivals.runId!, 'member') === null && snap(arrivals) === arrivalSnapshot, 'arrival idempotent');
  check(snap(Object.assign(new PartyState(), JSON.parse(arrivalSnapshot))) === arrivalSnapshot, 'arrival snapshot');
  arrivals.phase = RunPhase.Completed;
  check(arrivals.enter('leader') === null && arrivals.entered.length === 0, 'new run clears arrivals');
  check(snap(party().view()).includes('"runId":null') && snap(party().view()).includes('"phase":"Forming"'), 'enum null JSON');
  console.log(`Domain checks: ${count}/${count} passed`);
}
