import assert from 'node:assert/strict';
import { FieldState } from '../server/field/domain';
import { PartyRun, newId, validId } from '../server/field/party-domain';
import { DungeonState } from '../server/instance/domain';
import { ActionRejected, EnterField, MaxSafeInteger, MoveRequest, PrepareDungeon, type RunPhase } from '../server/shared/messages';
import { dungeonId } from '../server/shared/constants';
export function checkDomain(): void {
  let count = 0;
  const check = (value: boolean, message: string): void => { assert(value, message); count++; };
  let now = 1000;
  const domain = new FieldState(() => now);
  domain.admit('p', new EnterField(50, 0n), 'field-1'); domain.arrive('p');
  for (const [request, reason] of [
    [new MoveRequest(NaN, 1n), 'non-finite'], [new MoveRequest(Infinity, 1n), 'non-finite'],
    [new MoveRequest(-Infinity, 1n), 'non-finite'], [new MoveRequest(10.01, 1n), 'step-too-large'],
    [new MoveRequest(0, MaxSafeInteger + 1n), 'integer-out-of-range'],
    [new MoveRequest(0, -1n), 'stale-sequence'], [new MoveRequest(0, 0n), 'stale-sequence']
  ] as const) {
    const before = JSON.stringify(domain.capture());
    const result = domain.move('p', request);
    check(result instanceof ActionRejected && result.reason === reason && result.retryAfterMs === 0, reason);
    check(JSON.stringify(domain.capture()) === before, 'immutable rejection');
  }
  check(domain.move('p', new MoveRequest(10, 1n)) instanceof EnterField, 'first budget');
  const before = JSON.stringify(domain.capture()), budget = domain.move('p', new MoveRequest(10, 2n));
  check(budget instanceof ActionRejected && budget.reason === 'move-budget' && budget.retryAfterMs === 200, 'budget');
  check(JSON.stringify(domain.capture()) === before, 'budget unchanged');
  now += (budget as ActionRejected).retryAfterMs;
  check(domain.move('p', new MoveRequest(10, 2n)) instanceof EnterField, 'server delay');
  now += 25;
  const restored = new FieldState(() => now + 10_000);
  restored.restore(JSON.parse(JSON.stringify(domain.capture())));
  check(JSON.stringify(restored.capture()) === JSON.stringify(domain.capture()), 'relative snapshot');
  check(restored.move('p', new MoveRequest(0, MaxSafeInteger)) instanceof EnterField, 'maximum');
  check((restored.move('p', new MoveRequest(0, MaxSafeInteger + 1n)) as ActionRejected).reason === 'integer-out-of-range', 'range first');
  check((restored.move('p', new MoveRequest(0, MaxSafeInteger)) as ActionRejected).reason === 'stale-sequence', 'maximum stale');
  domain.admit('edge', new EnterField(0), 'field-1'); domain.arrive('edge');
  check((domain.move('edge', new MoveRequest(-1, 1n)) as ActionRejected).reason === 'outside-world', 'minimum');
  const last = new FieldState(() => now);
  last.admit('edge', new EnterField(399), 'field-4'); last.arrive('edge');
  check((last.move('edge', new MoveRequest(1, 1n)) as ActionRejected).reason === 'outside-world', 'maximum world');
  last.admit('fraction', new EnterField(399.5), 'field-4'); last.arrive('fraction');
  check(last.move('fraction', new MoveRequest(0, 1n)) instanceof EnterField, 'fractional world edge');
  check(!domain.admit('x', new EnterField(100), 'field-1'), 'arrival field');
  check(!domain.admit('x', new EnterField(1, MaxSafeInteger + 1n), 'field-1'), 'arrival sequence');
  check(!domain.admit('x', new EnterField(1, 0n, 201), 'field-1'), 'arrival remaining');
  const phases: RunPhase[] = ['Forming', 'Preparing', 'Entering', 'Active', 'Completed'];
  for (const phase of phases) {
    const party = Object.assign(new PartyRun(), { members: ['leader', 'member'], phase,
      runId: phase === 'Forming' ? null : 'run-' + 'a'.repeat(32) });
    const initial = JSON.stringify(party);
    check(party.enter('member')?.reason === 'not-leader' && JSON.stringify(party) === initial, 'nonleader ' + phase);
    const old = party.runId;
    check(party.enter('leader') === null, 'entry ' + phase);
    check(validId(party.runId!, 'run-'), 'run ID');
    check(['Forming', 'Completed'].includes(phase) ? party.runId !== old && party.phase === 'Preparing'
      : party.runId === old && party.phase === phase, 'entry transition');
    const leaving = Object.assign(new PartyRun(), { members: ['leader', 'member'], phase });
    leaving.leave('member'); check(leaving.members.length === (phase === 'Forming' ? 1 : 2), 'leave');
    const transition = Object.assign(new PartyRun(), { members: ['leader'], phase, runId: 'run' });
    check((transition.prepared('run') === null) === ['Preparing', 'Entering'].includes(phase), 'prepared');
    transition.phase = phase; transition.entered = ['leader'];
    check((transition.started('run') === null) === ['Entering', 'Active'].includes(phase), 'started');
    transition.phase = phase;
    check((transition.completed('run') === null) === ['Active', 'Completed'].includes(phase), 'completed');
    transition.phase = phase;
    check(transition.prepared('wrong')?.reason === 'run-mismatch' && transition.started('wrong')?.reason === 'run-mismatch'
      && transition.completed('wrong')?.reason === 'run-mismatch', 'mismatch');
    const dungeon = Object.assign(new DungeonState(), { phase });
    check((dungeon.advance() === null) === (phase === 'Active'), 'advance');
  }
  const run = new PartyRun();
  check(run.join('leader') === null && run.join('member') === null, 'forming membership');
  check(run.join('member')?.reason === 'invalid-party', 'duplicate');
  run.enter('leader'); const id = run.runId!;
  check(run.join('late')?.reason === 'invalid-party', 'frozen');
  run.prepared(id); run.arrived(id, 'leader'); run.arrived(id, 'member');
  check(run.started(id) === null && run.completed(id) === null, 'complete machine');
  check(JSON.stringify(Object.assign(new PartyRun(), JSON.parse(JSON.stringify(run)))) === JSON.stringify(run), 'party snapshot');
  run.enter('leader'); check(run.runId !== id && run.phase === 'Preparing', 'new run');
  const dungeon = new DungeonState(), prepare = new PrepareDungeon(id, newId('party-'), ['leader', 'member']);
  check(validId(prepare.partyId, 'party-'), 'party ID');
  check(dungeon.prepare(prepare, dungeonId(id)) === null && dungeon.prepare(prepare, dungeonId(id)) === null, 'prepare twice');
  const prepared = JSON.stringify(dungeon);
  check(dungeon.prepare(new PrepareDungeon('other', prepare.partyId, prepare.members), dungeonId(id))?.reason === 'run-mismatch'
    && dungeon.prepare(new PrepareDungeon(id, prepare.partyId, ['other']), dungeonId(id))?.reason === 'run-mismatch'
    && JSON.stringify(dungeon) === prepared, 'prepare mismatch');
  dungeon.arrived('leader'); check(dungeon.phase === 'Entering' && dungeon.advance()?.reason === 'run-not-active', 'partial');
  dungeon.arrived('member');
  for (let stage = 1; stage <= 3; stage++) check(dungeon.advance() === null && dungeon.stage === stage, 'stage');
  check(dungeon.phase === 'Completed' && dungeon.advance()?.reason === 'run-not-active', 'final');
  check(JSON.stringify(Object.assign(new DungeonState(), JSON.parse(JSON.stringify(dungeon)))) === JSON.stringify(dungeon), 'dungeon snapshot');
  check(JSON.stringify(run.view(prepare.partyId)).includes('"phase":"Preparing"'), 'enum');
  check(JSON.stringify(new PartyRun().view(prepare.partyId)).includes('"runId":null'), 'nullable');
  console.log(`Domain checks: ${count}/${count} passed`);
}
