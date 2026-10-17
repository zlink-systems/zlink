import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { appendFileSync } from 'node:fs';
import {
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec,
  ZlinkStreamDispatchMode,
  type ZlinkStreamConnector
} from '@zlink-systems/stream-connector';
import { Limits, Names, PacketNames, Http } from '../server/shared/constants.js';
import {
  JoinRoomRequest,
  MoveRequest,
  ShootRequest,
  RoomViewRequest,
  SessionInfoRequest,
  MatchRequest,
  type MatchResponse,
  type SessionInfo,
  type RoomView,
  type ShotRejected,
  type MatchEnded
} from '../server/shared/messages.js';
import { Game, Rejection } from '../server/shared/constants.js';
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
    for (const name of [
      PacketNames.roomView,
      PacketNames.shotRejected,
      PacketNames.matchEnded,
      PacketNames.actionRejected
    ])
      this.connector.on(name, (message) => {
        this.messages.push({ name, payload: message.payload });
        appendFileSync(
          log,
          JSON.stringify({ name, payload: message.payload }, (_, v) =>
            typeof v === 'bigint' ? v.toString() : v
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
        (m, index) => index >= from && m.name === name && predicate(m.payload as T)
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
  async join(roomId: string): Promise<void> {
    await this.connector.connect();
    await this.connector.send(new JoinRoomRequest(this.playerId, roomId)).submit();
  }
  async sessionInfo(): Promise<SessionInfo> {
    return this.connector.request(new SessionInfoRequest()).submit<SessionInfo>();
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
const Positions = { nearX: 10, targetX: 20, behindWallX: 60, farX: 200, laneY: 10 } as const;
function sameState(before: RoomView, after: RoomView): void {
  assert.deepEqual(after, { ...before, nodeId: after.nodeId }, 'Room state changed');
}
function sameActor(before: SessionInfo, after: SessionInfo): void {
  assert.equal(after.actorId, before.actorId);
  assert.equal(after.objectGeneration, before.objectGeneration);
}
async function move(
  a: PlayerClient,
  b: PlayerClient,
  mover: PlayerClient,
  x: number,
  sequence: number
): Promise<void> {
  const ma = a.messages.length,
    mb = b.messages.length;
  await mover.connector.send(new MoveRequest(x, Positions.laneY, sequence)).submit();
  const moved = (v: RoomView): boolean =>
    v.players.some((p) => p.playerId === mover.playerId && p.x === x && p.y === Positions.laneY);
  const views = await Promise.all([
    a.wait<RoomView>(PacketNames.roomView, ma, moved),
    b.wait<RoomView>(PacketNames.roomView, mb, moved)
  ]);
  assert.deepEqual(views[0], views[1]);
}
async function hit(
  a: PlayerClient,
  b: PlayerClient,
  sequence: number,
  hp: number
): Promise<RoomView> {
  const ma = a.messages.length,
    mb = b.messages.length;
  await a.connector.send(new ShootRequest(b.playerId, sequence)).submit();
  const damaged = (v: RoomView): boolean =>
    v.players.some((p) => p.playerId === b.playerId && p.hp === hp);
  const views = await Promise.all([
    a.wait<RoomView>(PacketNames.roomView, ma, damaged),
    b.wait<RoomView>(PacketNames.roomView, mb, damaged)
  ]);
  assert.deepEqual(views[0], views[1]);
  return views[0];
}
async function rejectShot(
  a: PlayerClient,
  b: PlayerClient,
  sequence: number,
  reason: string,
  target = b.playerId
): Promise<void> {
  const before = await b.view(),
    mark = a.messages.length;
  await a.connector.send(new ShootRequest(target, sequence)).submit();
  assert.equal((await a.wait<ShotRejected>(PacketNames.shotRejected, mark)).reason, reason);
  sameState(before, await b.view());
}
async function matchPair(endpoint: string, first: string, second: string): Promise<MatchResponse> {
  const match = async (playerId: string): Promise<MatchResponse> => {
    const response = await fetch(endpoint + Names.matchPath, {
      method: Http.post,
      headers: { 'content-type': Http.contentType },
      body: JSON.stringify(new MatchRequest(playerId)),
      signal: AbortSignal.timeout(Limits.messageMs)
    });
    assert(response.ok, `HTTP match status ${response.status}: ${await response.clone().text()}`);
    return (await response.json()) as MatchResponse;
  };
  const [a, b] = await Promise.all([match(first), match(second)]);
  assert.deepEqual(a, b, 'HTTP assignments differ');
  return a;
}
export async function scenario(
  name: string,
  endpoint: string,
  lobbyEndpoint: string,
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
    const assignment = await matchPair(lobbyEndpoint, a.playerId, b.playerId);
    assert.equal(assignment.sessionEndpoint, endpoint);
    await a.join(assignment.roomId);
    await b.join(assignment.roomId);
    const ready = (v: RoomView): boolean => v.players.length === Game.playersPerRoom;
    const views = await Promise.all([
      a.wait<RoomView>(PacketNames.roomView, 0, ready),
      b.wait<RoomView>(PacketNames.roomView, 0, ready)
    ]);
    assert.deepEqual(views[0], views[1]);
    assert.equal(views[0].roomId, assignment.roomId);
    assert(views[0].players.every((p) => p.hp === Game.initialHp));
    return views[0];
  };
  const a = create('p1'),
    b = create('p2');
  const failures: unknown[] = [];
  try {
    const initial = await pair(a, b);
    if (name === 'lobby') {
      assert.equal(a.disconnects, 0);
      assert.equal(b.disconnects, 0);
      assert(a.connector.isConnected && b.connector.isConnected);
    } else {
      const sa = await a.sessionInfo(),
        sb = await b.sessionInfo();
      assert.equal(sa.actorId, a.playerId);
      assert.equal(sb.actorId, b.playerId);
      await move(a, b, a, Positions.nearX, 1);
      await move(a, b, b, Positions.targetX, 1);
      const before = await hit(a, b, 2, Game.initialHp - Game.shotDamage);
      if (name === 'hit') {
        await move(a, b, b, Positions.behindWallX, 2);
        await rejectShot(a, b, 3, Rejection.blockedByWall);
        await move(a, b, b, Positions.farX, 3);
        await rejectShot(a, b, 4, Rejection.outOfRange);
        await rejectShot(a, b, 5, Rejection.noTarget, 'missing-player');
        await move(a, b, b, Positions.targetX, 4);
        const markA = a.messages.length,
          markB = b.messages.length;
        for (let sequence = 6; sequence <= 8; sequence++)
          await hit(a, b, sequence, Game.initialHp - Game.shotDamage * (sequence - 4));
        for (const ended of await Promise.all([
          a.wait<MatchEnded>(PacketNames.matchEnded, markA),
          b.wait<MatchEnded>(PacketNames.matchEnded, markB)
        ]))
          assert.equal(ended.winner, a.playerId);
      } else if (name === 'reconnect') {
        await b.close();
        const again = create('reconnected', b.playerId);
        await again.join(initial.roomId);
        const restored = await again.wait<RoomView>(PacketNames.roomView, 0);
        const identity = await again.sessionInfo();
        sameActor(sb, identity);
        assert.notEqual(identity.sessionId, sb.sessionId);
        sameState(before, restored);
        await hit(a, again, 3, Game.initialHp - Game.shotDamage * 2);
        assert.equal(again.disconnects, 0);
      } else if (name === 'update') {
        await control.drain(before.nodeId);
        assert.deepEqual(await a.sessionInfo(), sa);
        assert.deepEqual(await b.sessionInfo(), sb);
        const afterA = await a.view(),
          afterB = await b.view();
        assert.notEqual(afterA.nodeId, before.nodeId);
        assert.notEqual(afterB.nodeId, before.nodeId);
        sameState(before, afterA);
        sameState(before, afterB);
        await control.replace(before.nodeId);
        const fresh = await pair(create('new-a'), create('new-b'));
        assert.equal(fresh.nodeId, before.nodeId, 'New Room is not on the replacement match node');
        await hit(a, b, 3, Game.initialHp - Game.shotDamage * 2);
        assert.deepEqual(await a.sessionInfo(), sa);
        assert.deepEqual(await b.sessionInfo(), sb);
      } else throw new Error(`Unknown scenario ${name}`);
      assert.equal(a.disconnects, 0);
      if (name !== 'reconnect') assert.equal(b.disconnects, 0);
    }
  } catch (error) {
    failures.push(error);
  } finally {
    const results = await Promise.allSettled(clients.map((c) => c.close()));
    for (const result of results) if (result.status === 'rejected') failures.push(result.reason);
  }
  if (failures.length)
    throw new AggregateError(failures, `Scenario ${name} or client cleanup failed`);
}
