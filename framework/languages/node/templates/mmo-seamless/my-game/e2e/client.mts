import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { appendFileSync } from 'node:fs';
import {
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec,
  ZlinkStreamDispatchMode,
  type ZlinkStreamConnector
} from '@zlink-systems/stream-connector';
import { Control, Game, Limits, PacketNames } from '../server/shared/constants.js';
import {
  ActionRejected,
  EnterZone,
  JoinWorldRequest,
  MoveRequest,
  OpenDungeonRequest,
  Position,
  SessionInfoRequest,
  type SessionInfo,
  type WorldState,
  type PlayerAppeared,
  type PlayerLeft,
  type PlayerMoved
} from '../server/shared/messages.js';
interface Received {
  name: string;
  payload: unknown;
}
function contractPayload(name: string, payload: unknown): unknown {
  if (!payload || typeof payload !== 'object') return payload;
  const value = payload as Record<string, unknown>;
  if (
    (name === PacketNames.worldState || name === PacketNames.playerMoved || name === 'SessionInfo') &&
    typeof value.sequence === 'string'
  )
    return { ...value, sequence: BigInt(value.sequence) };
  if (name === 'SessionInfo' && typeof value.objectGeneration === 'string')
    return { ...value, objectGeneration: BigInt(value.objectGeneration) };
  return payload;
}
export interface NodeControl {
  drain(id: string): Promise<void>;
  replace(id: string): Promise<void>;
}
class PlayerClient {
  readonly connector: ZlinkStreamConnector;
  readonly messages: Received[] = [];
  private playerId = '';
  private readonly changed = new EventEmitter();
  private failure?: Error;
  disconnects = 0;
  constructor(
    endpoint: string,
    private readonly log: string
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
      PacketNames.worldState,
      PacketNames.playerAppeared,
      PacketNames.playerMoved,
      PacketNames.playerLeft,
      PacketNames.sessionInfo,
      PacketNames.actionRejected,
      PacketNames.dungeonOpened
    ])
      this.connector.on(name, (message) => {
        const payload = contractPayload(name, message.payload);
        this.messages.push({ name, payload });
        appendFileSync(
          log,
          JSON.stringify({ name, payload }, (_, v) =>
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
  async join(playerId: string, x: number): Promise<WorldState> {
    this.playerId = playerId;
    await this.connector.connect();
    const index = this.messages.length;
    await this.connector.send(new JoinWorldRequest(playerId, x)).submit();
    const state = await this.wait<WorldState>(PacketNames.worldState, index);
    assert.equal((await this.sessionInfo()).actorId, playerId);
    return state;
  }
  async sessionInfo(): Promise<SessionInfo> {
    return this.connector.request(new SessionInfoRequest()).submit<SessionInfo>();
  }
  async moveResult(delta: number, sequence: bigint): Promise<WorldState | ActionRejected> {
    const index = this.messages.length;
    await this.connector.send(new MoveRequest(delta, sequence)).submit();
    const find = (): WorldState | ActionRejected | undefined => {
      const message = this.messages.find(
        (value, messageIndex) =>
          messageIndex >= index &&
          (value.name === PacketNames.actionRejected ||
            (value.name === PacketNames.worldState &&
              (value.payload as WorldState).sequence === sequence))
      );
      return message?.payload as WorldState | ActionRejected | undefined;
    };
    const result = find();
    if (result !== undefined) return result;
    return new Promise<WorldState | ActionRejected>((resolve, reject) => {
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
          reject(new Error('Disconnected waiting for move result'));
        }
      };
      const timer = setTimeout(() => {
        cleanup();
        reject(new Error(`Timeout waiting for move result; log=${this.log}`));
      }, Limits.messageMs);
      this.changed.on('change', check);
      check();
    });
  }
  async sendMove(delta: number, sequence: bigint): Promise<void> {
    await this.connector.send(new MoveRequest(delta, sequence)).submit();
  }
  async move(delta: number, sequence: bigint): Promise<WorldState> {
    while (true) {
      const result = await this.moveResult(delta, sequence);
      if (!('reason' in result)) return result;
      const rejection = result as ActionRejected;
      if (rejection.reason !== 'move-budget' || rejection.retryAfterMs <= 0)
        throw new Error(`Move was rejected: ${rejection.reason}`);
      await new Promise((resolve) => setTimeout(resolve, rejection.retryAfterMs));
    }
  }
  async moveRejected(delta: number, sequence: bigint): Promise<ActionRejected> {
    const result = await this.moveResult(delta, sequence);
    assert('reason' in result, 'Move unexpectedly succeeded');
    return result as ActionRejected;
  }
  async state(): Promise<WorldState> {
    const index = this.messages.length;
    await this.connector.send(new JoinWorldRequest(this.playerId, 0)).submit();
    return this.wait<WorldState>(PacketNames.worldState, index);
  }
  async dungeon(id: string): Promise<{ dungeonId: string; nodeId: string }> {
    const index = this.messages.length;
    await this.connector.send(new OpenDungeonRequest(id)).submit();
    const opened = await this.wait<{ dungeonId: string; nodeId: string }>(
      PacketNames.dungeonOpened,
      index,
      (opened) => opened.dungeonId === id
    );
    assert.equal(opened.dungeonId, id);
    return opened;
  }
  async packetRejected(packet: EnterZone): Promise<ActionRejected> {
    const index = this.messages.length;
    await this.connector.send(packet).submit();
    return this.wait<ActionRejected>(PacketNames.actionRejected, index, (value) => value.reason === 'packet-not-allowed');
  }
  async close(): Promise<void> {
    await this.connector.close();
  }
}
function sameActor(before: SessionInfo, after: SessionInfo): void {
  assert.equal(after.actorId, before.actorId, 'Actor ID changed');
  assert.equal(after.objectGeneration, before.objectGeneration, 'Actor generation changed');
}
export async function scenario(
  name: string,
  endpoint: string,
  runId: string,
  logDir: string,
  control: NodeControl
): Promise<void> {
  const clients: PlayerClient[] = [];
  const create = (suffix: string): PlayerClient => {
    const client = new PlayerClient(endpoint, `${logDir}/${name}-${suffix}.jsonl`);
    clients.push(client);
    return client;
  };
  const id = `${name}-${runId}`;
  const client = create('player');
  const failures: unknown[] = [];
  try {
    if (name === 'border') {
      const observer = create('observer');
      const observed = await observer.join(`observer-${runId}`, 300);
      const appearedMark = observer.messages.length;
      const origin = await client.join(id, 299);
      assert.equal(observed.nodeId, 'zoneB');
      assert.equal(origin.nodeId, 'zoneA');
      const appeared = await observer.wait<PlayerAppeared>(
        PacketNames.playerAppeared,
        appearedMark,
        (v) => v.playerId === id
      );
      assert.equal(appeared.position.x, 299);
      const mark = observer.messages.length;
      await client.move(-1, 1n);
      const visible = await observer.wait<PlayerMoved>(
        PacketNames.playerMoved,
        mark,
        (v) => v.playerId === id && v.sequence === 1n
      );
      assert.equal(visible.position.x, 298);
      const leftMark = observer.messages.length;
      await client.move(2, 2n);
      const left = await observer.wait<PlayerLeft>(
        PacketNames.playerLeft,
        leftMark,
        (v) => v.playerId === id && v.reason === 'left-zone'
      );
      assert.equal(left.reason, 'left-zone');
      const enteredMark = observer.messages.length;
      const entered = await observer.wait<PlayerAppeared>(
        PacketNames.playerAppeared,
        enteredMark,
        (v) => v.playerId === id && v.position.x === 300
      );
      assert.equal(entered.position.x, 300);
      const movedMark = observer.messages.length;
      await client.move(1, 3n);
      await observer.wait<PlayerMoved>(
        PacketNames.playerMoved,
        movedMark,
        (v) => v.playerId === id && v.position.x === 301 && v.sequence === 3n
      );
      const disconnectedMark = observer.messages.length;
      await client.close();
      const disconnected = await observer.wait<PlayerLeft>(
        PacketNames.playerLeft,
        disconnectedMark,
        (v) => v.playerId === id && v.reason === 'disconnected'
      );
      assert.equal(disconnected.reason, 'disconnected');
      assert.equal(observer.disconnects, 0);
    } else {
      const before = await client.join(id, name === 'cross' ? 294 : 299);
      const session = await client.sessionInfo();
      assert.equal(before.zoneId, 'zone-3');
      assert.equal(before.nodeId, 'zoneA');
      if (name === 'cross' || name === 'reconnect') {
        if (name === 'cross') {
          const tooLarge = await client.moveRejected(11, 1n);
          assert.equal(tooLarge.reason, 'step-too-large');
          assert.deepEqual(await client.state(), before, 'Step rejection changed state');
          const responseIndex = client.messages.length;
          const firstResponse = client.wait<WorldState>(
            PacketNames.worldState,
            responseIndex,
            (state) => state.sequence === 1n
          );
          const budgetResponse = client.wait<ActionRejected>(
            PacketNames.actionRejected,
            responseIndex,
            (rejection) => rejection.reason === 'move-budget'
          );
          await client.sendMove(2, 1n);
          await client.sendMove(10, 2n);
          const first = await firstResponse;
          const budget = await budgetResponse;
          assert.equal(budget.reason, 'move-budget');
          assert(budget.retryAfterMs > 0, 'Move budget rejection omitted retry delay');
          assert.deepEqual(await client.state(), first, 'Budget rejection changed state');
          const after = await client.move(10, 2n);
          assert.equal(after.x, 306);
          sameActor(session, await client.sessionInfo());
          const packet = await client.packetRejected(new EnterZone(new Position(307), 3n, 0));
          assert.equal(packet.retryAfterMs, 0);
          assert.deepEqual(await client.state(), after, 'Session rejection changed state');
          const edge = create('edge');
          const edgeBefore = await edge.join(`edge-${runId}`, Game.zoneWidth * Game.zoneCount - 1);
          const outside = await edge.moveRejected(1, 1n);
          assert.equal(outside.reason, 'outside-world');
          assert.deepEqual(await edge.state(), edgeBefore, 'World-boundary rejection changed state');
          assert.deepEqual(await client.sessionInfo(), session);
          const continued = await client.move(1, 3n);
          assert.equal(continued.nodeId, 'zoneB');
          sameActor(session, await client.sessionInfo());
          assert.equal((await client.dungeon(`dungeon-${runId}`)).nodeId, 'instance');
        } else {
        const after = await client.move(1, 1n);
        sameActor(session, await client.sessionInfo());
        assert.equal(after.zoneId, 'zone-4');
        assert.equal(after.nodeId, 'zoneB');
          await client.close();
          const reconnect = create('reconnected');
          assert.deepEqual(
            await reconnect.join(id, 0),
            after,
            'Reconnect overwrote authoritative state'
          );
          const rebound = await reconnect.sessionInfo();
          sameActor(session, rebound);
          assert.notEqual(rebound.sessionId, session.sessionId);
          await reconnect.move(1, 2n);
          sameActor(session, await reconnect.sessionInfo());
          assert.equal(reconnect.disconnects, 0);
        }
      } else if (name === 'update') {
        let state = await client.move(-1, 1n);
        let safe = false;
        let drainFailure: unknown;
        const drain = control.drain(before.nodeId).then(
          () => {
            safe = true;
          },
          (error) => {
            drainFailure = error;
          }
        );
        let sequence = 1n;
        try {
          do {
            if (drainFailure) throw drainFailure;
            sequence++;
            const target = sequence % 2n === 0n ? 299 : 298;
            state = await client.move(target - state.x, sequence);
            sameActor(session, await client.sessionInfo());
            assert.equal(client.disconnects, 0);
          } while (!safe);
          await drain;
          if (drainFailure) throw drainFailure;
        } catch (error) {
          await Promise.allSettled([drain]);
          throw error;
        }
        assert.equal(state.zoneId, 'zone-3');
        assert.equal(state.nodeId, 'zoneB', 'SafeToShutdown did not move the player to Zone B');
        await control.replace(before.nodeId);
        const after = await client.move(299 - state.x, sequence + 1n);
        assert.equal(after.nodeId, 'zoneB');
        assert.deepEqual(await client.sessionInfo(), session);
        assert.equal((await client.dungeon(`after-update-${runId}`)).nodeId, 'instance');
        // Exercise fresh Actor creation and movement after the replacement is ready.
        const replacement = create('replacement');
        const fresh = await replacement.join(`replacement-${runId}`, Control.replacementX);
        assert.equal(fresh.nodeId, before.nodeId, 'Replacement did not accept the new Zone');
        const freshSession = await replacement.sessionInfo();
        const moved = await replacement.move(1, 1n);
        assert.equal(moved.nodeId, before.nodeId);
        sameActor(freshSession, await replacement.sessionInfo());
        assert.equal(replacement.disconnects, 0);
      } else throw new Error(`Unknown scenario ${name}`);
      if (name !== 'reconnect') assert.equal(client.disconnects, 0);
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
