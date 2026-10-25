import assert from 'node:assert/strict';
import { isDeepStrictEqual } from 'node:util';
import { Names } from '../server/shared/constants';
import { MatchRequest, MatchResponse, PrepareRoom } from '../server/shared/lobby-messages';
import { Vec3 } from '../server/shared/geometry';
import { MoveRequest, ShootRequest } from '../server/shared/messages';
import { Matchmaker } from '../server/lobby/matchmaker';
import { RoomState } from '../server/match/domain';
import { RoomRoster, type HttpRoomSnapshot } from '../server/match/roster';

interface ProvisionCall {
  roomId: string;
  members: string[];
}

class RecordingProvisioner {
  static readonly sessionEndpoint = 'ws://127.0.0.1:1';
  readonly calls: ProvisionCall[] = [];

  constructor(private readonly completion?: Promise<MatchResponse>) {}

  prepare = (roomId: string, members: string[]): Promise<MatchResponse> => {
    this.calls.push({ roomId, members: [...members] });
    return (
      this.completion ?? Promise.resolve(new MatchResponse(roomId, RecordingProvisioner.sessionEndpoint))
    );
  };
}

function rejected(action: () => unknown): boolean {
  try {
    action();
    return false;
  } catch (error) {
    return error instanceof Error;
  }
}

function matchmaker(provisioner: RecordingProvisioner): Matchmaker {
  return new Matchmaker(provisioner.prepare);
}

function request(playerId: string, mode?: string): MatchRequest {
  return mode === undefined ? new MatchRequest(playerId) : new MatchRequest(playerId, mode);
}

function signal(): AbortSignal {
  return new AbortController().signal;
}

export async function runLobbyDomainChecks(): Promise<void> {
  let count = 0;
  const check = (condition: boolean, name: string): void => {
    assert(condition, `HTTP domain boundary failed: ${name}`);
    count++;
  };
  const roster = new RoomRoster();
  check(!roster.canAdmit('a') && !roster.canAdmit('outsider'), 'unprepared Room rejects admission');
  check(
    rejected(() => roster.prepare('room', new PrepareRoom('other', ['a', 'b']))),
    'wrong match identity'
  );
  check(
    rejected(() => roster.prepare('room', new PrepareRoom('room', ['a', 'a']))),
    'duplicate roster member'
  );
  check(
    rejected(() => roster.prepare('room', new PrepareRoom('room', ['a']))),
    'incomplete roster'
  );
  const members = ['a', 'b'];
  check(
    roster.prepare('room', new PrepareRoom('room', members)).matchId === 'room',
    'preparation acknowledges identity'
  );
  members[0] = 'outsider';
  check(
    roster.canAdmit('a') && roster.canAdmit('b') && !roster.canAdmit('outsider'),
    'roster owns member copy'
  );
  const snapshot = roster.capture();
  roster.prepare('room', new PrepareRoom('room', ['b', 'a']));
  check(isDeepStrictEqual(roster.capture(), snapshot), 'same preparation is idempotent');
  check(
    rejected(() => roster.prepare('room', new PrepareRoom('room', ['a', 'outsider']))) &&
      isDeepStrictEqual(roster.capture(), snapshot),
    'prepared roster cannot be replaced'
  );
  const game = new RoomState(() => 1000);
  game.admit('a');
  game.admit('b');
  game.move('a', new MoveRequest(new Vec3(0, 0, -1), 1));
  game.shoot('a', new ShootRequest(new Vec3(4, 0, 1), 2));
  const envelope: HttpRoomSnapshot = { Game: game.capture(), Roster: roster.capture() };
  const restored = JSON.parse(JSON.stringify(envelope)) as HttpRoomSnapshot;
  const restoredRoster = new RoomRoster();
  restoredRoster.restore(restored.Roster);
  const restoredGame = new RoomState(() => 900000);
  restoredGame.restore(restored.Game);
  check(
    restoredRoster.canAdmit('a') &&
      restoredRoster.canAdmit('b') &&
      !restoredRoster.canAdmit('outsider') &&
      isDeepStrictEqual(restoredGame.view(), game.view()),
    'relocation envelope retains roster, HP, position and sequence'
  );
  check(
    rejected(() => restoredRoster.prepare('room', new PrepareRoom('room', ['a', 'outsider']))),
    'relocated roster cannot be replaced'
  );
  check(
    rejected(() => new RoomRoster().restore({ matchId: null, members: ['a'] })),
    'unprepared snapshot with members'
  );
  check(request('default').mode === Names.defaultMode, 'omitted HTTP mode defaults to duel');

  const provisioner = new RecordingProvisioner();
  const firstProcess = matchmaker(provisioner);
  const rankedA = firstProcess.match(request('ra', 'ranked'), signal());
  const casualA = firstProcess.match(request('ca', 'casual'), signal());
  const rankedB = firstProcess.match(request('rb', 'ranked'), signal());
  let casualResolved = false;
  void casualA.then(() => {
    casualResolved = true;
  });
  const [rankedAResult, rankedBResult] = await Promise.all([rankedA, rankedB]);
  await Promise.resolve();
  check(
    !casualResolved && isDeepStrictEqual(rankedAResult, rankedBResult),
    'interleaved modes pair only matching members'
  );
  const casualB = firstProcess.match(request('cb', 'casual'), signal());
  const [casualAResult, casualBResult] = await Promise.all([casualA, casualB]);
  check(
    isDeepStrictEqual(casualAResult, casualBResult) &&
      casualAResult.roomId !== rankedAResult.roomId,
    'different modes have distinct Rooms'
  );
  check(
    provisioner.calls[0]?.members.slice().sort().join(',') === 'ra,rb' &&
      provisioner.calls[1]?.members.slice().sort().join(',') === 'ca,cb',
    'provisioner receives each matched roster'
  );
  const restarted = matchmaker(provisioner);
  const newA = restarted.match(request('new-a'), signal());
  const newB = restarted.match(request('new-b'), signal());
  const [newRoomA, newRoomB] = await Promise.all([newA, newB]);
  check(
    isDeepStrictEqual(newRoomA, newRoomB) &&
      newRoomA.roomId !== rankedAResult.roomId &&
      newRoomA.roomId !== casualAResult.roomId,
    'restart cannot reuse counter-based Room identity'
  );
  check(
    provisioner.calls.every(
      (call) =>
        call.roomId.startsWith(Names.roomPrefix) &&
        /^[0-9a-f]{32}$/u.test(call.roomId.slice(Names.roomPrefix.length))
    ),
    'Room ID contains a generated GUID'
  );

  let release!: (value: MatchResponse) => void;
  const completion = new Promise<MatchResponse>((resolve) => {
    release = resolve;
  });
  const waitingProvisioner = new RecordingProvisioner(completion);
  const held = matchmaker(waitingProvisioner);
  const heldA = held.match(request('held-a'), signal());
  const heldB = held.match(request('held-b'), signal());
  let heldSettled = 0;
  void heldA.then(() => {
    heldSettled++;
  });
  void heldB.then(() => {
    heldSettled++;
  });
  await Promise.resolve();
  check(
    heldSettled === 0 && waitingProvisioner.calls.length === 1,
    'HTTP assignments wait for Room preparation completion'
  );
  const heldCall = waitingProvisioner.calls[0];
  assert(heldCall);
  release(new MatchResponse(heldCall.roomId, RecordingProvisioner.sessionEndpoint));
  const [heldResultA, heldResultB] = await Promise.all([heldA, heldB]);
  check(isDeepStrictEqual(heldResultA, heldResultB), 'prepared assignment releases both HTTP responses');
  assert.equal(count, 19, 'HTTP domain boundary count differs from .NET');
  console.log(`HTTP domain boundaries: ${count}/${count} passed`);
}
