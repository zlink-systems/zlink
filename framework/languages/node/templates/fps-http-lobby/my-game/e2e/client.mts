import '../.zlink-framework-json-schemas.cjs';
import assert from 'node:assert/strict';
import { inspect, isDeepStrictEqual } from 'node:util';
import { EventEmitter } from 'node:events';
import { appendFileSync } from 'node:fs';
import { setTimeout as delay } from 'node:timers/promises';
import {
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec,
  ZlinkStreamDispatchMode,
  type ZlinkStreamConnector
} from '@zlink-systems/stream-connector';
import { Game, Limits, MatchPhase, Names, PacketNames, Rejection, ShotOutcome } from '../server/shared/constants.js';
import { addVec, subtractVec, Vec3 } from '../server/shared/geometry.js';
import { Http, LobbyNames, LobbyScenarios } from '../server/shared/lobby-constants.js';
import {
  ActionRejected,
  MatchEnded,
  MatchFound,
  MoveRequest,
  MoveResult,
  RoomViewRequest,
  SessionInfoRequest,
  ShootRequest,
  ShotRejected,
  ShotResult,
  RoomView,
  SessionInfo
} from '../server/shared/messages.js';
import { JoinRoomRequest, MatchRequest, PrepareRoom } from '../server/shared/lobby-messages.js';
import type { MatchResponse } from '../server/shared/lobby-messages.js';

interface Received {
  name: string;
  payload: unknown;
}

export interface NodeControl {
  drain(id: string): Promise<void>;
  replace(id: string): Promise<void>;
  restartLobby(): Promise<string>;
}

class PlayerClient {
  readonly connector: ZlinkStreamConnector;
  readonly messages: Received[] = [];
  private readonly changed = new EventEmitter();
  private failure?: Error;
  disconnects = 0;

  constructor(
    readonly endpoint: string,
    private readonly log: string,
    readonly playerId: string
  ) {
    this.connector = zlinkStreamConnectorFactory.create({
      endpoint,
      codec: zlinkStreamJsonCodec,
      dispatchMode: ZlinkStreamDispatchMode.Immediate,
      waitTimeoutMs: Limits.messageMs,
      requestTimeoutMs: Limits.messageMs,
      reconnect: { enabled: false }
    });
    for (const packetType of [
      RoomView,
      MatchFound,
      MoveResult,
      ShotResult,
      ShotRejected,
      MatchEnded,
      ActionRejected
    ])
      this.connector.on(packetType, (message) => {
        const name = packetType.name;
        this.messages.push({ name, payload: message.payload });
        appendFileSync(
          log,
          JSON.stringify({ name, payload: message.payload }, (_, value) =>
            typeof value === 'bigint' ? value.toString() : value
          ) + '\n'
        );
        this.changed.emit('change');
      });
    this.connector.onDisconnected(() => {
      this.disconnects++;
      this.changed.emit('change');
    });
    this.connector.onErrorReceived((error) => {
      this.failure = new Error(`Connector error: ${error.code}: ${error.message}`);
      this.changed.emit('change');
    });
  }

  async wait<T>(
    name: string,
    from: number,
    predicate: (payload: T) => boolean = () => true
  ): Promise<T> {
    const find = (): T | undefined =>
      this.messages.find(
        (message, index) =>
          index >= from && message.name === name && predicate(message.payload as T)
      )?.payload as T | undefined;
    const result = find();
    if (result !== undefined) return result;
    return new Promise<T>((resolve, reject) => {
      const cleanup = (): void => {
        clearTimeout(timer);
        this.changed.off('change', check);
      };
      const check = (): void => {
        const value = find();
        if (value !== undefined) {
          cleanup();
          resolve(value);
        } else if (this.failure) {
          cleanup();
          reject(this.failure);
        } else if (!this.connector.isConnected) {
          cleanup();
          reject(new Error(`Disconnected waiting for ${name}`));
        }
      };
      const timer = setTimeout(() => {
        cleanup();
        reject(new Error(`Timeout waiting for ${name}; log=${this.log}`));
      }, Limits.messageMs);
      this.changed.on('change', check);
      check();
    });
  }

  async waitAny(names: readonly string[], from: number): Promise<Received> {
    const find = (): Received | undefined =>
      this.messages.find((message, index) => index >= from && names.includes(message.name));
    const result = find();
    if (result) return result;
    return new Promise<Received>((resolve, reject) => {
      const cleanup = (): void => {
        clearTimeout(timer);
        this.changed.off('change', check);
      };
      const check = (): void => {
        const value = find();
        if (value) {
          cleanup();
          resolve(value);
        } else if (this.failure) {
          cleanup();
          reject(this.failure);
        } else if (!this.connector.isConnected) {
          cleanup();
          reject(new Error(`Disconnected waiting for ${names.join(' or ')}`));
        }
      };
      const timer = setTimeout(() => {
        cleanup();
        reject(new Error(`Timeout waiting for ${names.join(' or ')}; log=${this.log}`));
      }, Limits.messageMs);
      this.changed.on('change', check);
      check();
    });
  }

  async connect(): Promise<void> {
    if (!this.connector.isConnected) await this.connector.connect();
  }

  async join(roomId: string): Promise<void> {
    await this.connect();
    await this.connector.send(new JoinRoomRequest(this.playerId, roomId)).submit();
  }

  async sessionInfo(): Promise<SessionInfo> {
    return this.connector.request(new SessionInfoRequest()).submit(SessionInfo);
  }

  async view(): Promise<RoomView> {
    const index = this.messages.length;
    await this.connector.send(new RoomViewRequest()).submit();
    return this.wait<RoomView>(PacketNames.roomView, index);
  }

  async action(request: MoveRequest | ShootRequest): Promise<Received> {
    const names =
      request instanceof MoveRequest
        ? [PacketNames.moveResult, PacketNames.actionRejected]
        : [PacketNames.shotResult, PacketNames.shotRejected];
    const index = this.messages.length;
    await this.connector.send(request).submit();
    return this.waitAny(names, index);
  }

  async sendClientPacket(packet: object): Promise<Received> {
    await this.connect();
    const index = this.messages.length;
    await this.connector.send(packet).submit();
    return this.waitAny([PacketNames.actionRejected], index);
  }

  async close(): Promise<void> {
    await this.connector.close();
  }
}

function equal(a: unknown, b: unknown): boolean {
  return isDeepStrictEqual(structuredClone(a), structuredClone(b));
}

function sameState(before: RoomView, after: RoomView): void {
  assert(equal(after, { ...before, nodeId: after.nodeId }), 'Room state changed.');
}

function player(view: RoomView, client: PlayerClient): RoomView['players'][number] {
  const found = view.players.find((candidate) => candidate.playerId === client.playerId);
  assert(found, `Player ${client.playerId} is missing from RoomView.`);
  return found;
}

function next(view: RoomView, client: PlayerClient): bigint {
  return player(view, client).lastSequence + 1n;
}

async function viewAsync(client: PlayerClient): Promise<RoomView> {
  return client.view();
}

async function moveAsync(client: PlayerClient, delta: Vec3): Promise<void> {
  const before = await viewAsync(client),
    request = new MoveRequest(delta, next(before, client));
  while (true) {
    const result = await client.action(request);
    if (result.name === PacketNames.actionRejected) {
      const rejected = result.payload as ActionRejected;
      if (rejected.reason === Rejection.moveBudget && rejected.retryAfterMs > 0) {
        sameState(before, await viewAsync(client));
        await delay(rejected.retryAfterMs);
        continue;
      }
    }
    assert.equal(result.name, PacketNames.moveResult, `Movement was not accepted: ${inspect(result.payload)}.`);
    const moved = result.payload as MoveResult,
      expected = addVec(player(before, client).position, delta);
    assert.equal(moved.sequence, request.sequence);
    assert(equal(moved.position, expected), 'Movement result position differs.');
    const after = await viewAsync(client);
    assert.equal(player(after, client).lastSequence, request.sequence, 'Movement lost sequence.');
    assert(after.players.every((candidate) => candidate.position.y === 0), 'Movement changed feet height.');
    return;
  }
}

async function walkAsync(client: PlayerClient, destination: Vec3): Promise<void> {
  let position = player(await viewAsync(client), client).position;
  while (!equal(position, destination)) {
    const remaining = subtractVec(destination, position),
      delta =
        remaining.x !== 0
          ? new Vec3(Math.max(-Game.maxStep, Math.min(Game.maxStep, remaining.x)), 0, 0)
          : remaining.y !== 0
            ? new Vec3(0, Math.max(-Game.maxStep, Math.min(Game.maxStep, remaining.y)), 0)
            : new Vec3(0, 0, Math.max(-Game.maxStep, Math.min(Game.maxStep, remaining.z)));
    await moveAsync(client, delta);
    position = player(await viewAsync(client), client).position;
  }
}

async function aimAsync(shooter: PlayerClient, target: PlayerClient): Promise<Vec3> {
  const current = await viewAsync(shooter);
  return subtractVec(player(current, target).position, player(current, shooter).position);
}

async function shootAsync(shooter: PlayerClient, aimDirection: Vec3, outcome: string): Promise<void> {
  const before = await viewAsync(shooter),
    request = new ShootRequest(aimDirection, next(before, shooter));
  while (true) {
    const result = await shooter.action(request);
    if (result.name === PacketNames.shotRejected) {
      const rejected = result.payload as ShotRejected;
      if (rejected.reason === Rejection.fireTooSoon && rejected.retryAfterMs > 0) {
        sameState(before, await viewAsync(shooter));
        await delay(rejected.retryAfterMs);
        continue;
      }
    }
    assert.equal(result.name, PacketNames.shotResult, `Expected ${outcome}, received ${inspect(result.payload)}.`);
    const shot = result.payload as ShotResult;
    assert.equal(shot.outcome, outcome);
    assert.equal(shot.sequence, request.sequence);
    return;
  }
}

async function hitAsync(shooter: PlayerClient, target: PlayerClient, hp: number): Promise<void> {
  await shootAsync(shooter, await aimAsync(shooter, target), ShotOutcome.hit);
  const shooterView = await viewAsync(shooter),
    targetView = await viewAsync(target);
  assert(equal(shooterView, targetView) && player(shooterView, target).hp === hp, 'Hit views or damage differ.');
}

async function endedAsync(a: PlayerClient, b: PlayerClient): Promise<void> {
  assert.equal((await a.wait<MatchEnded>(PacketNames.matchEnded, 0)).winner, a.playerId, 'Wrong winner for first client.');
  assert.equal((await b.wait<MatchEnded>(PacketNames.matchEnded, 0)).winner, a.playerId, 'Wrong winner for second client.');
  const view = await viewAsync(a);
  assert(view.phase === MatchPhase.Finished && view.winner === a.playerId, 'Finished state differs.');
}

async function rejectedAsync(
  client: PlayerClient,
  request: MoveRequest | ShootRequest,
  reason: string
): Promise<void> {
  const before = await viewAsync(client),
    result = await client.action(request);
  assert(
    (result.name === PacketNames.actionRejected &&
      (result.payload as ActionRejected).retryAfterMs === 0 &&
      (result.payload as ActionRejected).reason === reason) ||
      (result.name === PacketNames.shotRejected &&
        (result.payload as ShotRejected).retryAfterMs === 0 &&
        (result.payload as ShotRejected).reason === reason),
    `Expected rejection ${reason}, received ${inspect(result.payload)}.`
  );
  sameState(before, await viewAsync(client));
  console.log(`input rejected: ${reason}; state unchanged`);
}

async function inputAsync(a: PlayerClient, b: PlayerClient): Promise<void> {
  const beforeBudget = await viewAsync(a),
    moveSequence = next(beforeBudget, a),
    acceptedMoveRequest = new MoveRequest(new Vec3(0, 0, -2), moveSequence),
    move = new MoveRequest(new Vec3(0, 0, -2), moveSequence + 1n),
    moveStart = a.messages.length;
  await a.connector.send(acceptedMoveRequest).submit();
  await a.connector.send(move).submit();
  const acceptedMove = await a.wait<MoveResult>(PacketNames.moveResult, moveStart);
  assert.equal(acceptedMove.sequence, moveSequence, 'Expected accepted movement before movement-budget rejection.');
  const budget = await a.wait<ActionRejected>(PacketNames.actionRejected, moveStart);
  assert(budget.reason === Rejection.moveBudget && budget.retryAfterMs > 0, 'Expected movement-budget rejection.');
  const afterBudget = await viewAsync(a),
    beforeBudgetPlayer = player(beforeBudget, a),
    afterBudgetPlayer = player(afterBudget, a);
  assert(equal(afterBudgetPlayer.position, acceptedMove.position), 'Movement-budget rejection changed the accepted position.');
  assert.equal(afterBudgetPlayer.lastSequence, moveSequence, 'Movement-budget rejection changed the accepted sequence.');
  assert.equal(afterBudgetPlayer.hp, beforeBudgetPlayer.hp, 'Movement-budget rejection changed player HP.');
  assert.equal(player(afterBudget, b).hp, player(beforeBudget, b).hp, 'Movement-budget rejection changed target HP.');
  await delay(budget.retryAfterMs);
  const retriedMove = await a.action(move);
  assert(
    retriedMove.name === PacketNames.moveResult &&
      (retriedMove.payload as MoveResult).sequence === move.sequence,
    'Server movement retry delay was insufficient.'
  );

  let view = await viewAsync(a),
    sequence = player(view, a).lastSequence;
  await rejectedAsync(a, new MoveRequest(new Vec3(0, 0, 1), sequence), Rejection.staleSequence);
  await rejectedAsync(a, new ShootRequest(new Vec3(1, 0, 0), sequence - 1n), Rejection.staleSequence);
  await rejectedAsync(a, new MoveRequest(new Vec3(Game.maxStep + 1, 0, 0), sequence + 1n), Rejection.stepTooLarge);
  await rejectedAsync(a, new MoveRequest(new Vec3(0, 1, 0), sequence + 1n), Rejection.verticalMove);
  await rejectedAsync(a, new MoveRequest(new Vec3(0, -1, 0), sequence + 1n), Rejection.verticalMove);
  await walkAsync(a, new Vec3(player(view, a).position.x, 0, 0));
  sequence = next(await viewAsync(a), a);
  await rejectedAsync(a, new MoveRequest(new Vec3(0, 0, -1), sequence), Rejection.outsideMap);
  await rejectedAsync(a, new ShootRequest(new Vec3(0, 0, 0), sequence), Rejection.invalidAim);
  await walkAsync(a, new Vec3(48, 0, 22));
  sequence = next(await viewAsync(a), a);
  await rejectedAsync(a, new MoveRequest(new Vec3(2, 0, 0), sequence), Rejection.moveBlockedByWall);
  const beforeFire = await viewAsync(a),
    shotSequence = next(beforeFire, a),
    acceptedShotRequest = new ShootRequest(new Vec3(0, 1, 0), shotSequence),
    shot = new ShootRequest(new Vec3(0, 1, 0), shotSequence + 1n),
    shotStart = a.messages.length;
  await a.connector.send(acceptedShotRequest).submit();
  await a.connector.send(shot).submit();
  const acceptedShot = await a.wait<ShotResult>(PacketNames.shotResult, shotStart);
  assert.equal(acceptedShot.sequence, shotSequence, 'Expected accepted shot before fire-too-soon rejection.');
  assert.equal(acceptedShot.outcome, ShotOutcome.miss, 'Expected accepted miss before fire-too-soon rejection.');
  const fire = await a.wait<ShotRejected>(PacketNames.shotRejected, shotStart);
  assert(fire.reason === Rejection.fireTooSoon && fire.retryAfterMs > 0, 'Expected fire-interval rejection.');
  const afterFire = await viewAsync(a),
    beforeFirePlayer = player(beforeFire, a),
    afterFirePlayer = player(afterFire, a);
  assert(equal(afterFirePlayer.position, beforeFirePlayer.position), 'Fire-too-soon rejection changed player position.');
  assert.equal(afterFirePlayer.lastSequence, shotSequence, 'Fire-too-soon rejection changed the accepted sequence.');
  assert.equal(afterFirePlayer.hp, beforeFirePlayer.hp, 'Fire-too-soon rejection changed player HP.');
  assert.equal(player(afterFire, b).hp, player(beforeFire, b).hp, 'Fire-too-soon rejection changed target HP.');
  await delay(fire.retryAfterMs);
  const retriedShot = await a.action(shot);
  assert(
    retriedShot.name === PacketNames.shotResult &&
      (retriedShot.payload as ShotResult).sequence === shot.sequence &&
      (retriedShot.payload as ShotResult).outcome === ShotOutcome.miss,
    'Server fire retry delay was insufficient.'
  );
  assert.equal(player(await viewAsync(b), b).hp, Game.initialHp, 'Input checks damaged target.');
  console.log('input: movement-budget and fire-too-soon server retry delays accepted unchanged requests');
}

async function requestMatch(
  lobbyEndpoint: string,
  playerId: string,
  mode?: string
): Promise<MatchResponse> {
  const body = mode === undefined ? { playerId } : new MatchRequest(playerId, mode);
  const response = await fetch(lobbyEndpoint + LobbyNames.matchPath, {
    method: Http.post,
    headers: { 'content-type': Http.contentType },
    body: JSON.stringify(body),
    signal: AbortSignal.timeout(Limits.messageMs)
  });
  assert(response.ok, `HTTP match status ${response.status}: ${await response.clone().text()}`);
  return (await response.json()) as MatchResponse;
}

async function matchPair(
  lobbyEndpoint: string,
  first: string,
  second: string,
  firstMode?: string,
  secondMode?: string
): Promise<MatchResponse> {
  const [a, b] = await Promise.all([
    requestMatch(lobbyEndpoint, first, firstMode),
    requestMatch(lobbyEndpoint, second, secondMode)
  ]);
  assert.deepEqual(a, b, 'HTTP players received different Room or Session endpoint.');
  return a;
}

async function rejectOutsiderAsync(
  endpoint: string,
  roomId: string,
  playerId: string,
  logDir: string
): Promise<void> {
  const outsider = new PlayerClient(endpoint, `${logDir}/outsider-${playerId}.jsonl`, playerId);
  try {
    await outsider.join(roomId);
    const rejected = await outsider.wait<ActionRejected>(PacketNames.actionRejected, 0);
    assert.equal(rejected.reason, Rejection.admissionRejected, 'Room admitted an outsider.');
  } finally {
    await outsider.close();
  }
}

async function lobbyAsync(
  endpoint: string,
  lobbyEndpoint: string,
  runId: string,
  logDir: string,
  control: NodeControl
): Promise<void> {
  const suffix = runId;
  const defaultAId = `duel-a-${suffix}`;
  const defaultBId = `duel-b-${suffix}`;
  const rankedAId = `ranked-a-${suffix}`;
  const rankedBId = `ranked-b-${suffix}`;
  const clients: PlayerClient[] = [];
  const create = (label: string, playerId: string): PlayerClient => {
    const client = new PlayerClient(endpoint, `${logDir}/lobby-${label}.jsonl`, playerId);
    clients.push(client);
    return client;
  };
  try {
    const [duel, ranked, duelPartner, rankedPartner] = await Promise.all([
      requestMatch(lobbyEndpoint, defaultAId),
      requestMatch(lobbyEndpoint, rankedAId, 'ranked'),
      requestMatch(lobbyEndpoint, defaultBId, Names.defaultMode),
      requestMatch(lobbyEndpoint, rankedBId, 'ranked')
    ]);
    assert.deepEqual(duel, duelPartner, 'Default duel players received different assignments.');
    assert.deepEqual(ranked, rankedPartner, 'Ranked players received different assignments.');
    assert.notEqual(duel.roomId, ranked.roomId, 'Different modes received the same Room.');
    assert.equal(duel.sessionEndpoint, endpoint, 'Default duel returned a different Session endpoint.');
    assert.equal(ranked.sessionEndpoint, endpoint, 'Ranked returned a different Session endpoint.');
    await rejectOutsiderAsync(endpoint, duel.roomId, `outsider-empty-${suffix}`, logDir);

    const a = create('duel-a', defaultAId);
    const b = create('duel-b', defaultBId);
    const c = create('ranked-a', rankedAId);
    const d = create('ranked-b', rankedBId);
    const forged = new PrepareRoom(duel.roomId, ['forged-a', 'forged-b']);
    await a.connect();
    const beforeAdmission = await a.sendClientPacket(forged);
    assert.equal((beforeAdmission.payload as ActionRejected).reason, Rejection.packetNotAllowed);
    await Promise.all([
      a.join(duel.roomId),
      b.join(duel.roomId),
      c.join(ranked.roomId),
      d.join(ranked.roomId)
    ]);
    const ready = (view: RoomView): boolean => view.players.length === Game.playersPerRoom;
    const [duelView, rankedView] = await Promise.all([
      a.wait<RoomView>(PacketNames.roomView, 0, ready),
      c.wait<RoomView>(PacketNames.roomView, 0, ready)
    ]);
    assert(
      new Set(duelView.players.map((player) => player.playerId)).size === Game.playersPerRoom &&
        duelView.players.some((player) => player.playerId === a.playerId) &&
        duelView.players.some((player) => player.playerId === b.playerId) &&
        new Set(rankedView.players.map((player) => player.playerId)).size === Game.playersPerRoom &&
        rankedView.players.some((player) => player.playerId === c.playerId) &&
        rankedView.players.some((player) => player.playerId === d.playerId),
      'Prepared Room roster differs from HTTP members.'
    );
    const duringMatch = await a.sendClientPacket(forged);
    assert.equal((duringMatch.payload as ActionRejected).reason, Rejection.packetNotAllowed);
    sameState(duelView, await viewAsync(a));
    await moveAsync(a, new Vec3(0, 0, -1));
    await hitAsync(a, b, Game.initialHp - Game.shotDamage);
    const beforeRestart = await viewAsync(a);
    const session = await a.sessionInfo();
    const restartedLobbyEndpoint = await control.restartLobby();
    sameState(beforeRestart, await viewAsync(a));
    assert.deepEqual(await a.sessionInfo(), session, 'Lobby restart changed an existing Session.');

    const freshAId = `restart-a-${suffix}`;
    const freshBId = `restart-b-${suffix}`;
    const fresh = await matchPair(restartedLobbyEndpoint, freshAId, freshBId);
    assert(
      fresh.roomId !== duel.roomId &&
        fresh.roomId !== ranked.roomId &&
        fresh.sessionEndpoint === endpoint,
      'Lobby restart reused a Room or changed the Session endpoint.'
    );
    const x = create('restart-a', freshAId);
    const y = create('restart-b', freshBId);
    await Promise.all([x.join(fresh.roomId), y.join(fresh.roomId)]);
    const freshView = await x.wait<RoomView>(PacketNames.roomView, 0, ready);
    assert.equal(freshView.roomId, fresh.roomId);
    assert(freshView.players.every((player) => player.hp === Game.initialHp));
    sameState(beforeRestart, await viewAsync(a));
    assert(
      clients.every((client) => !client.disconnects && client.connector.isConnected),
      'Lobby flow disconnected a client.'
    );
    console.log('lobby: default duel, mode separation, roster, PrepareRoom rejection and restart verified');
  } finally {
    await Promise.all(clients.map((client) => client.close()));
  }
}

async function newMatchAsync(
  endpoint: string,
  lobbyEndpoint: string,
  runId: string,
  logDir: string,
  replacementId: string
): Promise<void> {
  const a = new PlayerClient(endpoint, `${logDir}/new-a.jsonl`, `update-${runId}-new-a`),
    b = new PlayerClient(endpoint, `${logDir}/new-b.jsonl`, `update-${runId}-new-b`);
  try {
    const room = await pairAsync(a, b, lobbyEndpoint);
    assert.equal(room.nodeId, replacementId, 'Replacement node did not accept the new Room.');
  } finally {
    await Promise.all([a.close(), b.close()]);
  }
}

async function enterPairAsync(
  a: PlayerClient,
  b: PlayerClient,
  assignment: MatchResponse
): Promise<RoomView> {
  assert.equal(assignment.sessionEndpoint, a.endpoint, 'Lobby returned a different Session endpoint.');
  await Promise.all([a.join(assignment.roomId), b.join(assignment.roomId)]);
  const [foundA, foundB] = await Promise.all([
    a.wait<MatchFound>(PacketNames.matchFound, 0),
    b.wait<MatchFound>(PacketNames.matchFound, 0)
  ]);
  assert.equal(foundA.roomId, assignment.roomId, 'First client entered a different Room.');
  assert.equal(foundB.roomId, assignment.roomId, 'Second client entered a different Room.');
  assert.equal(foundA.roomId, foundB.roomId, 'Same-mode players entered different Rooms.');
  const ready = (value: RoomView): boolean => value.players.length === Game.playersPerRoom,
    [viewA, viewB] = await Promise.all([
      a.wait<RoomView>(PacketNames.roomView, 0, ready),
      b.wait<RoomView>(PacketNames.roomView, 0, ready)
    ]);
  assert(equal(viewA, viewB) && viewA.phase === MatchPhase.Playing, 'Initial Room views differ.');
  assert(
    viewA.players.every((candidate) => candidate.hp === Game.initialHp && candidate.lastSequence === 0n),
    'Initial Room state differs.'
  );
  return viewA;
}

async function pairAsync(a: PlayerClient, b: PlayerClient, lobbyEndpoint: string): Promise<RoomView> {
  const assignment = await matchPair(lobbyEndpoint, a.playerId, b.playerId);
  return enterPairAsync(a, b, assignment);
}

export async function scenario(
  name: string,
  endpoint: string,
  lobbyEndpoint: string,
  runId: string,
  logDir: string,
  control: NodeControl
): Promise<void> {
  assert(LobbyScenarios.includes(name as (typeof LobbyScenarios)[number]), `Unknown scenario ${name}`);
  if (name === 'lobby') {
    await lobbyAsync(endpoint, lobbyEndpoint, runId, logDir, control);
    return;
  }
  const clients: PlayerClient[] = [],
    create = (suffix: string, playerId = `${name}-${runId}-${suffix}`): PlayerClient => {
      const client = new PlayerClient(endpoint, `${logDir}/${name}-${suffix}.jsonl`, playerId);
      clients.push(client);
      return client;
    },
    p1 = create('p1'),
    p2 = create('p2');
  try {
    const initial = await pairAsync(p1, p2, lobbyEndpoint);
    const session1 = await p1.sessionInfo(),
      session2 = await p2.sessionInfo();
    assert.equal(session1.actorId, p1.playerId);
    assert.equal(session2.actorId, p2.playerId);
    switch (name) {
      case 'hit':
        await hitAsync(p1, p2, Game.initialHp - Game.shotDamage);
        await shootAsync(p1, new Vec3(0, 1, 0), ShotOutcome.miss);
        assert.equal(player(await viewAsync(p2), p2).hp, 75, 'An upward miss caused damage.');
        await walkAsync(p1, new Vec3(5, 0, 18));
        await walkAsync(p1, new Vec3(5, 0, 65));
        await walkAsync(p2, new Vec3(52, 0, 65));
        await walkAsync(p2, new Vec3(95, 0, 65));
        await shootAsync(p1, await aimAsync(p1, p2), ShotOutcome.miss);
        assert.equal(player(await viewAsync(p2), p2).hp, 75, 'An out-of-range ray caused damage.');
        await walkAsync(p1, new Vec3(20, 0, 65));
        for (let hp = 50; hp >= 0; hp -= Game.shotDamage) await hitAsync(p1, p2, hp);
        await endedAsync(p1, p2);
        await rejectedAsync(
          p1,
          new MoveRequest(new Vec3(0, 0, 1), next(await viewAsync(p1), p1)),
          Rejection.matchFinished
        );
        await rejectedAsync(
          p1,
          new ShootRequest(new Vec3(1, 0, 0), next(await viewAsync(p1), p1)),
          Rejection.matchFinished
        );
        break;
      case 'wall': {
        await walkAsync(p1, new Vec3(48, 0, 22));
        await walkAsync(p2, new Vec3(52, 0, 22));
        const beforeWall = await viewAsync(p2);
        await shootAsync(p1, await aimAsync(p1, p2), ShotOutcome.blockedByWall);
        assert.equal(
          player(await viewAsync(p2), p2).hp,
          player(beforeWall, p2).hp,
          'A wall-blocked ray caused damage.'
        );
        await walkAsync(p1, new Vec3(48, 0, 18));
        await walkAsync(p2, new Vec3(52, 0, 18));
        await hitAsync(p1, p2, 75);
        console.log('wall: intervening wall blocks; wall behind target permits hit');
        break;
      }
      case 'input':
        await inputAsync(p1, p2);
        break;
      case 'reconnect': {
        await walkAsync(p2, new Vec3(54, 0, 18));
        await hitAsync(p1, p2, 75);
        const beforeReconnect = await viewAsync(p2),
          oldPlayerId = p2.playerId;
        await p2.close();
        const again = create('reconnected', oldPlayerId);
        await again.join(initial.roomId);
        const restored = await again.wait<RoomView>(PacketNames.roomView, 0),
          identity = await again.sessionInfo();
        sameState(beforeReconnect, restored);
        assert(
          identity.sessionId !== session2.sessionId &&
            identity.actorId === session2.actorId &&
            identity.objectGeneration === session2.objectGeneration,
          'Reconnect changed Actor identity.'
        );
        await moveAsync(again, new Vec3(0, 0, 1));
        for (let hp = 50; hp >= 0; hp -= Game.shotDamage) await hitAsync(p1, again, hp);
        await endedAsync(p1, again);
        const finished = create('finished', oldPlayerId);
        await finished.join(initial.roomId);
        const finishedView = await finished.wait<RoomView>(PacketNames.roomView, 0);
        assert(
          finishedView.phase === MatchPhase.Finished &&
            finishedView.winner === p1.playerId &&
            player(finishedView, finished).hp === 0,
          'Finished reconnect lost winner or HP.'
        );
        break;
      }
      case 'update': {
        await moveAsync(p1, new Vec3(0, 0, -1));
        await hitAsync(p1, p2, 75);
        const before = await viewAsync(p1);
        await control.drain(before.nodeId);
        assert.deepEqual(await p1.sessionInfo(), session1, 'Update changed first session binding.');
        assert.deepEqual(await p2.sessionInfo(), session2, 'Update changed second session binding.');
        const afterA = await viewAsync(p1),
          afterB = await viewAsync(p2);
        assert.notEqual(afterA.nodeId, before.nodeId);
        assert.notEqual(afterB.nodeId, before.nodeId);
        sameState(before, afterA);
        sameState(before, afterB);
        await rejectOutsiderAsync(endpoint, before.roomId, `outsider-update-${runId}`, logDir);
        sameState(before, await viewAsync(p1));
        await control.replace(before.nodeId);
        await newMatchAsync(endpoint, lobbyEndpoint, runId, logDir, before.nodeId);
        await hitAsync(p1, p2, 50);
        assert.deepEqual(await p1.sessionInfo(), session1, 'Replacement changed first session binding.');
        assert.deepEqual(await p2.sessionInfo(), session2, 'Replacement changed second session binding.');
        break;
      }
      default:
        throw new Error(`Unknown scenario ${name}; initial Room ${initial.roomId}`);
    }
    assert(!p1.disconnects && p1.connector.isConnected, 'First player disconnected.');
    if (name !== 'reconnect') assert(!p2.disconnects && p2.connector.isConnected, 'Second player disconnected.');
  } finally {
    await Promise.all(clients.map((client) => client.close()));
  }
}
