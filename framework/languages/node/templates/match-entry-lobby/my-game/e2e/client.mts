import '../.zlink-framework-json-schemas.cjs';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { appendFileSync } from 'node:fs';
import {
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec,
  ZlinkStreamDispatchMode,
  type ZlinkStreamConnector
} from '@zlink-systems/stream-connector';
import { Game, Limits, PacketNames, Rejection } from '../server/shared/constants.js';
import {
  ActionRejected,
  QueueRequest,
  MoveRequest,
  MoveResult,
  RoomAssignment,
  ShootRequest,
  ShotResult,
  RoomView,
  RoomViewRequest,
  SessionInfoRequest,
  SessionInfo,
  MatchFound,
  MatchEnded
} from '../server/shared/messages.js';

interface Received {
  name: string;
  payload: unknown;
}

export interface NodeControl {
  drain(id: string): Promise<void>;
  replace(id: string): Promise<void>;
}

class PlayerClient {
  readonly connector: ZlinkStreamConnector;
  readonly messages: Received[] = [];
  private readonly changed = new EventEmitter();
  private failure?: Error;
  disconnects = 0;

  constructor(
    endpoint: string,
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
    for (const packetType of [RoomView, MatchFound, MoveResult, ShotResult, MatchEnded, ActionRejected])
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

  async waitForActionResult(
    from: number,
    updated: (view: RoomView) => boolean
  ): Promise<ActionRejected | RoomView> {
    const find = (): ActionRejected | RoomView | undefined => {
      for (let index = from; index < this.messages.length; index++) {
        const message = this.messages[index];
        if (message.name === PacketNames.actionRejected) return message.payload as ActionRejected;
        if (message.name === PacketNames.roomView && updated(message.payload as RoomView))
          return message.payload as RoomView;
      }
      return undefined;
    };
    const result = find();
    if (result !== undefined) return result;
    return new Promise<ActionRejected | RoomView>((resolve, reject) => {
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
          reject(new Error('Disconnected waiting for an action result'));
        }
      };
      const timer = setTimeout(() => {
        cleanup();
        reject(new Error(`Timeout waiting for an action result; log=${this.log}`));
      }, Limits.messageMs);
      this.changed.on('change', check);
      check();
    });
  }

  async queue(): Promise<void> {
    await this.connector.connect();
    await this.connector.send(new QueueRequest(this.playerId)).submit();
  }

  async sessionInfo(): Promise<SessionInfo> {
    return this.connector.request(new SessionInfoRequest()).submit(SessionInfo);
  }

  async view(): Promise<RoomView> {
    const index = this.messages.length;
    await this.connector.send(new RoomViewRequest()).submit();
    return this.wait<RoomView>(PacketNames.roomView, index);
  }

  async close(): Promise<void> {
    await this.connector.close();
  }
}

function sameState(before: RoomView, after: RoomView): void {
  assert.deepEqual(after, { ...before, nodeId: after.nodeId }, 'Room state changed');
}

function sameActor(before: SessionInfo, after: SessionInfo): void {
  assert.equal(after.actorId, before.actorId);
  assert.equal(after.objectGeneration, before.objectGeneration);
}

function delay(milliseconds: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

function playerView(view: RoomView, playerId: string) {
  const player = view.players.find((candidate) => candidate.playerId === playerId);
  assert(player, `Room view omitted player ${playerId}`);
  return player;
}

async function nextSequence(player: PlayerClient): Promise<bigint> {
  return playerView(await player.view(), player.playerId).lastSequence + 1n;
}

async function moveBy(
  a: PlayerClient,
  b: PlayerClient,
  mover: PlayerClient,
  dx: number,
  dy: number,
  sequence: bigint,
  priorView?: RoomView,
  requireMoveBudget = false
): Promise<RoomView> {
  const before = priorView ?? (await mover.view());
  const position = playerView(before, mover.playerId);
  const targetX = position.x + dx;
  const targetY = position.y + dy;
  const moved = (view: RoomView): boolean => {
    const player = view.players.find((candidate) => candidate.playerId === mover.playerId);
    return (
      player !== undefined &&
      player.x === targetX &&
      player.y === targetY &&
      player.lastSequence === sequence
    );
  };
  let moveBudgetRejected = false;
  while (true) {
    const mark = mover.messages.length;
    const other = mover.playerId === a.playerId ? b : a;
    const peerMark = other.messages.length;
    const resultTask = mover.waitForActionResult(mark, moved);
    await mover.connector.send(new MoveRequest(dx, dy, BigInt(sequence))).submit();
    const result = await resultTask;
    if (result instanceof ActionRejected || 'reason' in result) {
      const rejection = result as ActionRejected;
      assert.equal(moveBudgetRejected, false, 'Move was rejected again after RetryAfterMs');
      moveBudgetRejected = true;
      assert.equal(
        rejection.reason,
        Rejection.moveBudget,
        `Unexpected movement rejection: ${rejection.reason}`
      );
      assert(rejection.retryAfterMs > 0, 'Movement rejection omitted RetryAfterMs');
      sameState(before, await mover.view());
      await delay(rejection.retryAfterMs);
      continue;
    }

    const peerView = await other.wait<RoomView>(PacketNames.roomView, peerMark, moved);
    assert.deepEqual(result, peerView, 'Movement views differ');
    assert(!requireMoveBudget || moveBudgetRejected, 'Move budget rejection was not observed');
    return result;
  }
}

async function walk(
  a: PlayerClient,
  b: PlayerClient,
  mover: PlayerClient,
  destinationX: number,
  destinationY: number
): Promise<RoomView> {
  let view = await mover.view();
  let position = playerView(view, mover.playerId);
  while (position.x !== destinationX || position.y !== destinationY) {
    const remainingX = destinationX - position.x;
    const dx = remainingX === 0 ? 0 : Math.max(-Game.maxStep, Math.min(remainingX, Game.maxStep));
    const remainingY = destinationY - position.y;
    const dy =
      dx !== 0 || remainingY === 0
        ? 0
        : Math.max(-Game.maxStep, Math.min(remainingY, Game.maxStep));
    view = await moveBy(a, b, mover, dx, dy, position.lastSequence + 1n, view);
    position = playerView(view, mover.playerId);
  }
  return view;
}

async function hit(
  a: PlayerClient,
  b: PlayerClient,
  sequence: bigint,
  hp: number,
  allowFireRetry = true
): Promise<RoomView> {
  const damaged = (view: RoomView): boolean =>
    playerView(view, b.playerId).hp === hp &&
    (hp !== 0 || (view.phase === Game.finishedPhase && view.winner === a.playerId));
  let before = await a.view();
  let fireTooSoonRejected = false;
  while (true) {
    const markA = a.messages.length;
    const markB = b.messages.length;
    const resultTask = a.waitForActionResult(markA, damaged);
    await a.connector.send(new ShootRequest(b.playerId, BigInt(sequence))).submit();
    const result = await resultTask;
    if (result instanceof ActionRejected || 'reason' in result) {
      const rejection = result as ActionRejected;
      assert.equal(fireTooSoonRejected, false, 'Shot was rejected again after RetryAfterMs');
      fireTooSoonRejected = true;
      assert.equal(
        rejection.reason,
        Rejection.fireTooSoon,
        `Unexpected shot rejection: ${rejection.reason}`
      );
      assert(rejection.retryAfterMs > 0, 'Fire rejection omitted RetryAfterMs');
      sameState(before, await a.view());
      assert(allowFireRetry, 'Shot was rejected after the explicit RetryAfterMs wait');
      await delay(rejection.retryAfterMs);
      before = await a.view();
      continue;
    }

    const peerView = await b.wait<RoomView>(PacketNames.roomView, markB, damaged);
    assert.deepEqual(result, peerView, 'Hit views differ');
    return result;
  }
}

async function rejectAction<T>(
  player: PlayerClient,
  request: T,
  reason: string,
  retryAfterMs?: number
): Promise<ActionRejected> {
  const before = await player.view();
  const mark = player.messages.length;
  const rejected = player.wait<ActionRejected>(PacketNames.actionRejected, mark);
  await player.connector.send(request).submit();
  const response = await rejected;
  assert.equal(response.reason, reason, `Expected ${reason}; received ${response.reason}`);
  if (retryAfterMs !== undefined) {
    assert.equal(response.retryAfterMs, retryAfterMs, `Unexpected RetryAfterMs for ${reason}`);
  } else {
    const timeLimited = reason === Rejection.moveBudget || reason === Rejection.fireTooSoon;
    assert(
      timeLimited ? response.retryAfterMs > 0 : response.retryAfterMs === 0,
      `Unexpected RetryAfterMs for ${reason}`
    );
  }
  sameState(before, await player.view());
  return response;
}

async function rejectShot(
  a: PlayerClient,
  b: PlayerClient,
  sequence: bigint,
  reason: string,
  target = b.playerId
): Promise<void> {
  await rejectAction(a, new ShootRequest(target, BigInt(sequence)), reason);
}

async function input(a: PlayerClient, b: PlayerClient): Promise<void> {
  const beforeMove = await a.view();
  const firstSpawn = beforeMove.players.find(
    (player) => player.x === Game.firstSpawnX && player.y === Game.firstSpawnY
  );
  assert(firstSpawn, 'Room view omitted the first spawn');
  const mover = firstSpawn.playerId === a.playerId ? a : b;
  const other = mover.playerId === a.playerId ? b : a;
  const source = playerView(beforeMove, mover.playerId);
  const target = playerView(beforeMove, other.playerId);
  const moveDelta = target.x >= source.x ? Game.maxStep : -Game.maxStep;
  const moveSequence = source.lastSequence + 1n;
  const acceptedMove = new MoveRequest(moveDelta, 0, moveSequence);
  const rejectedMove = new MoveRequest(moveDelta, 0, moveSequence + 1n);
  const moveMark = mover.messages.length;
  const acceptedMoveTask = mover.wait<MoveResult>(MoveResult.name, moveMark, (result) =>
    result.sequence === moveSequence
  );
  const rejectedMoveTask = mover.wait<ActionRejected>(PacketNames.actionRejected, moveMark);
  await mover.connector.send(acceptedMove).submit();
  await mover.connector.send(rejectedMove).submit();
  const [moveResult, moveRejection] = await Promise.all([acceptedMoveTask, rejectedMoveTask]);
  assert.equal(moveResult.sequence, moveSequence, 'Accepted move result sequence differs');
  assert.equal(moveRejection.reason, Rejection.moveBudget, 'Second move was not rejected for movement budget');
  assert(moveRejection.retryAfterMs > 0, 'Movement rejection omitted RetryAfterMs');
  const afterMoveA = await mover.view();
  const afterMovePlayer = playerView(afterMoveA, mover.playerId);
  assert.equal(afterMovePlayer.x, moveResult.x);
  assert.equal(afterMovePlayer.y, moveResult.y);
  assert.equal(afterMovePlayer.lastSequence, moveSequence);
  assert.equal(playerView(afterMoveA, other.playerId).hp, target.hp);
  await delay(moveRejection.retryAfterMs);
  const retryMoveMark = mover.messages.length;
  const retriedMoveTask = mover.wait<MoveResult>(MoveResult.name, retryMoveMark, (result) =>
    result.sequence === rejectedMove.sequence
  );
  await mover.connector.send(rejectedMove).submit();
  const retriedMove = await retriedMoveTask;
  assert.equal(retriedMove.sequence, rejectedMove.sequence, 'Retried move result sequence differs');
  const afterMoveB = await mover.view();
  assert.equal(playerView(afterMoveB, mover.playerId).lastSequence, rejectedMove.sequence);
  assert.equal(playerView(afterMoveB, mover.playerId).x, retriedMove.x);
  assert.equal(playerView(afterMoveB, mover.playerId).y, retriedMove.y);

  const beforeShot = await a.view();
  const shotSequence = playerView(beforeShot, a.playerId).lastSequence + 1n;
  const shotTarget = playerView(beforeShot, b.playerId);
  const acceptedShot = new ShootRequest(b.playerId, shotSequence);
  const rejectedShot = new ShootRequest(b.playerId, shotSequence + 1n);
  const shotMark = a.messages.length;
  const acceptedShotTask = a.wait<ShotResult>(ShotResult.name, shotMark, (result) =>
    result.sequence === shotSequence
  );
  const rejectedShotTask = a.wait<ActionRejected>(PacketNames.actionRejected, shotMark);
  await a.connector.send(acceptedShot).submit();
  await a.connector.send(rejectedShot).submit();
  const [shotResult, fireRejection] = await Promise.all([acceptedShotTask, rejectedShotTask]);
  assert.equal(shotResult.sequence, shotSequence, 'Accepted shot result sequence differs');
  assert.equal(fireRejection.reason, Rejection.fireTooSoon, 'Second shot was not rejected for fire interval');
  assert(fireRejection.retryAfterMs > 0, 'Fire rejection omitted RetryAfterMs');
  const afterShotA = await a.view();
  assert.equal(playerView(afterShotA, b.playerId).hp, shotTarget.hp - Game.shotDamage);
  assert.equal(playerView(afterShotA, a.playerId).lastSequence, shotSequence);
  await delay(fireRejection.retryAfterMs);
  const retryShotMark = a.messages.length;
  const retriedShotTask = a.wait<ShotResult>(ShotResult.name, retryShotMark, (result) =>
    result.sequence === rejectedShot.sequence
  );
  await a.connector.send(rejectedShot).submit();
  const retriedShot = await retriedShotTask;
  assert.equal(retriedShot.sequence, rejectedShot.sequence, 'Retried shot result sequence differs');
  const afterShotB = await a.view();
  assert.equal(playerView(afterShotB, b.playerId).hp, shotTarget.hp - 2 * Game.shotDamage);
  assert.equal(playerView(afterShotB, a.playerId).lastSequence, rejectedShot.sequence);

  const staleSequence = playerView(await mover.view(), mover.playerId).lastSequence;
  await rejectAction(mover, new MoveRequest(0, 0, staleSequence), Rejection.staleSequence);
  await rejectAction(mover, new MoveRequest(0, 0, moveSequence), Rejection.staleSequence);
  const stepTooLargeSequence = staleSequence + 1n;
  await rejectAction(
    mover,
    new MoveRequest(Game.maxStep + 0.01, 0, stepTooLargeSequence),
    Rejection.stepTooLarge
  );
  await rejectAction(
    mover,
    new MoveRequest(0, 0, BigInt(Game.maxSafeInteger) + 1n),
    Rejection.integerOutOfRange
  );

  const outsideStart = await walk(a, b, mover, 1, Game.firstSpawnY);
  const outsideSequence = playerView(outsideStart, mover.playerId).lastSequence + 1n;
  await rejectAction(
    mover,
    new MoveRequest(-Game.maxStep, 0, outsideSequence),
    Rejection.outsideMap
  );

  const wallCenterY = (Game.wallMinY + Game.wallMaxY) / 2;
  const beforeWall = await walk(a, b, mover, Game.wallX - Game.maxStep, wallCenterY);
  const wallSequence = playerView(beforeWall, mover.playerId).lastSequence + 1n;
  await rejectAction(
    mover,
    new MoveRequest(Game.maxStep, 0, wallSequence),
    Rejection.moveBlockedByWall
  );

  await moveBy(a, b, mover, -0.1, 0, BigInt(Game.maxSafeInteger));
  const atMaximum = await mover.view();
  assert.equal(
    playerView(atMaximum, mover.playerId).lastSequence,
    BigInt(Game.maxSafeInteger),
    'Maximum safe sequence was not accepted'
  );
  await rejectAction(
    mover,
    new MoveRequest(0, 0, BigInt(Game.maxSafeInteger) + 1n),
    Rejection.integerOutOfRange
  );
}

export async function scenario(
  name: string,
  endpoint: string,
  runId: string,
  logDir: string,
  control: NodeControl
): Promise<void> {
  const clients: PlayerClient[] = [];
  const create = (suffix: string, playerId = `${name}-${runId}-${suffix}`): PlayerClient => {
    const client = new PlayerClient(endpoint, `${logDir}/${name}-${suffix}.jsonl`, playerId);
    clients.push(client);
    return client;
  };
  const pair = async (a: PlayerClient, b: PlayerClient): Promise<RoomView> => {
    await a.queue();
    await b.queue();
    const [foundA, foundB] = await Promise.all([
      a.wait<MatchFound>(PacketNames.matchFound, 0),
      b.wait<MatchFound>(PacketNames.matchFound, 0)
    ]);
    assert.equal(foundA.roomId, foundB.roomId);
    const ready = (view: RoomView): boolean => view.players.length === Game.playersPerRoom;
    const views = await Promise.all([
      a.wait<RoomView>(PacketNames.roomView, 0, ready),
      b.wait<RoomView>(PacketNames.roomView, 0, ready)
    ]);
    assert.deepEqual(views[0], views[1]);
    assert.equal(views[0].roomId, foundA.roomId);
    assert(
      views[0].players.every((player) => player.hp === Game.initialHp && player.lastSequence === 0n)
    );
    assert.equal(views[0].phase, Game.playingPhase);
    assert.equal(views[0].winner, null);
    return views[0];
  };

  const a = create('p1');
  const b = create('p2');
  const failures: unknown[] = [];
  try {
    const initial = await pair(a, b);
    if (name === 'input') {
      await input(a, b);
    } else {
      const sessionA = await a.sessionInfo();
      const sessionB = await b.sessionInfo();
      assert.equal(sessionA.actorId, a.playerId);
      assert.equal(sessionB.actorId, b.playerId);
      await moveBy(a, b, a, 0, 0, 1n, initial);
      await moveBy(a, b, b, 0, 0, 1n, initial);
      const before = await hit(a, b, 2n, Game.initialHp - Game.shotDamage);

      if (name === 'hit') {
        await rejectAction(a, new RoomAssignment(before.roomId), Rejection.packetNotAllowed);
        const wallCenterY = (Game.wallMinY + Game.wallMaxY) / 2;
        await walk(a, b, a, Game.firstSpawnX, Game.firstSpawnY);
        await walk(a, b, b, Game.secondSpawnX, wallCenterY);
        await rejectShot(a, b, await nextSequence(a), Rejection.blockedByWall);
        await walk(a, b, b, Game.mapMaxX, Game.mapMaxY - 10);
        await rejectShot(a, b, await nextSequence(a), Rejection.outOfRange);
        await rejectShot(a, b, await nextSequence(a), Rejection.noTarget, 'missing-player');
        await walk(a, b, b, Game.secondSpawnX, Game.firstSpawnY);
        const markA = a.messages.length;
        const markB = b.messages.length;
        for (let hp = Game.initialHp - 2 * Game.shotDamage; hp >= 0; hp -= Game.shotDamage)
          await hit(a, b, await nextSequence(a), hp);
        for (const ended of await Promise.all([
          a.wait<MatchEnded>(PacketNames.matchEnded, markA),
          b.wait<MatchEnded>(PacketNames.matchEnded, markB)
        ]))
          assert.equal(ended.winner, a.playerId);
      } else if (name === 'wall') {
        const wallCenterY = (Game.wallMinY + Game.wallMaxY) / 2;
        const left = Game.wallX - Game.maxStep;
        const right = Game.wallX + Game.maxStep;
        await walk(a, b, a, left, wallCenterY);
        await walk(a, b, b, right, wallCenterY);
        const beforeWall = await a.view();
        const targetBefore = playerView(beforeWall, b.playerId);
        await rejectAction(
          a,
          new MoveRequest(Game.maxStep, 0, BigInt(await nextSequence(a))),
          Rejection.moveBlockedByWall
        );
        await rejectShot(a, b, await nextSequence(a), Rejection.blockedByWall);
        const afterWall = await a.view();
        sameState(beforeWall, afterWall);
        assert.equal(
          playerView(afterWall, b.playerId).hp,
          targetBefore.hp,
          'Wall-blocked shot changed HP'
        );

        await walk(a, b, a, Game.firstSpawnX, Game.firstSpawnY);
        await walk(a, b, b, Game.secondSpawnX, Game.secondSpawnY);
        const beforeClearShot = await a.view();
        const expectedHp = playerView(beforeClearShot, b.playerId).hp - Game.shotDamage;
        const clearShot = await hit(a, b, await nextSequence(a), expectedHp);
        assert.equal(
          playerView(clearShot, b.playerId).hp,
          expectedHp,
          'Wall behind the clear lane blocked a shot'
        );
      } else if (name === 'reconnect') {
        await b.close();
        const again = create('reconnected', b.playerId);
        await again.queue();
        const restored = await again.wait<RoomView>(PacketNames.roomView, 0);
        const identity = await again.sessionInfo();
        sameActor(sessionB, identity);
        assert.notEqual(identity.sessionId, sessionB.sessionId);
        sameState(before, restored);
        assert.equal(restored.phase, Game.playingPhase);
        assert.equal(restored.winner, null);
        assert.equal(playerView(restored, again.playerId).lastSequence, 1n);
        await moveBy(again, a, again, 0, 0, 2n);
        await hit(a, again, 3n, Game.initialHp - Game.shotDamage * 2);
        await hit(a, again, 4n, Game.initialHp - Game.shotDamage * 3);
        const finished = await hit(a, again, 5n, 0);
        assert.equal(finished.phase, Game.finishedPhase);
        assert.equal(finished.winner, a.playerId);
        const finishedAgain = create('finished-reconnect', again.playerId);
        await finishedAgain.queue();
        const restoredFinished = await finishedAgain.wait<RoomView>(
          PacketNames.roomView,
          0,
          (view) => view.phase === Game.finishedPhase
        );
        const finishedIdentity = await finishedAgain.sessionInfo();
        assert.notEqual(finishedIdentity.sessionId, identity.sessionId);
        sameActor(identity, finishedIdentity);
        sameState(finished, restoredFinished);
        assert.equal(restoredFinished.winner, a.playerId);
        assert.equal(playerView(restoredFinished, again.playerId).lastSequence, 2n);
      } else if (name === 'update') {
        await control.drain(before.nodeId);
        assert.deepEqual(await a.sessionInfo(), sessionA);
        assert.deepEqual(await b.sessionInfo(), sessionB);
        const afterA = await a.view();
        const afterB = await b.view();
        assert.notEqual(afterA.nodeId, before.nodeId);
        assert.notEqual(afterB.nodeId, before.nodeId);
        sameState(before, afterA);
        sameState(before, afterB);
        await control.replace(before.nodeId);
        const fresh = await pair(create('new-a'), create('new-b'));
        assert.equal(fresh.nodeId, before.nodeId, 'Replacement did not accept a new Room');
        await hit(a, b, 3n, Game.initialHp - Game.shotDamage * 2);
        assert.deepEqual(await a.sessionInfo(), sessionA);
        assert.deepEqual(await b.sessionInfo(), sessionB);
      } else {
        throw new Error(`Unknown scenario ${name}`);
      }
    }
    assert.equal(a.disconnects, 0);
    if (name !== 'reconnect') assert.equal(b.disconnects, 0);
  } catch (error) {
    failures.push(error);
  } finally {
    const results = await Promise.allSettled(clients.map((client) => client.close()));
    for (const result of results) if (result.status === 'rejected') failures.push(result.reason);
  }
  if (failures.length)
    throw new AggregateError(failures, `Scenario ${name} or client cleanup failed`);
}
