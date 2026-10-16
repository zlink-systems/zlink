import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { appendFileSync } from 'node:fs';
import {
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec,
  ZlinkStreamDispatchMode,
  type ZlinkStreamConnector
} from '@zlink-systems/stream-connector';
import { Control, Limits, PacketNames } from '../server/shared/constants.js';
import {
  JoinWorldRequest,
  MoveRequest,
  OpenDungeonRequest,
  SessionInfoRequest,
  type SessionInfo,
  type WorldState,
  type BorderVisible
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
      PacketNames.borderVisible,
      PacketNames.dungeonOpened
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
  async join(playerId: string, x: number): Promise<WorldState> {
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
  async move(x: number, sequence: number): Promise<WorldState> {
    const index = this.messages.length;
    await this.connector.send(new MoveRequest(x, sequence)).submit();
    const state = await this.wait<WorldState>(
      PacketNames.worldState,
      index,
      (state) => state.sequence === sequence
    );
    assert.equal(state.x, x);
    assert.equal(state.sequence, sequence);
    return state;
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
  try {
    if (name === 'border') {
      const observer = create('observer');
      const observed = await observer.join(`observer-${runId}`, 300);
      const origin = await client.join(id, 299);
      assert.equal(observed.nodeId, 'zoneB');
      assert.equal(origin.nodeId, 'zoneA');
      const mark = observer.messages.length;
      await client.move(298, 1);
      const visible = await observer.wait<BorderVisible>(
        PacketNames.borderVisible,
        mark,
        (v) => v.actorId === id && v.sequence === 1
      );
      assert.equal(visible.observerZoneId, 'zone-4');
      assert.equal(visible.sourceZoneId, 'zone-3');
      assert.equal(visible.x, 298);
      assert.equal(observer.disconnects, 0);
    } else {
      const before = await client.join(id, 299);
      const session = await client.sessionInfo();
      assert.equal(before.zoneId, 'zone-3');
      assert.equal(before.nodeId, 'zoneA');
      if (name === 'cross' || name === 'reconnect') {
        const after = await client.move(300, 1);
        sameActor(session, await client.sessionInfo());
        assert.equal(after.zoneId, 'zone-4');
        assert.equal(after.nodeId, 'zoneB');
        if (name === 'cross') {
          assert.deepEqual(await client.sessionInfo(), session);
          const continued = await client.move(301, 2);
          assert.equal(continued.nodeId, 'zoneB');
          sameActor(session, await client.sessionInfo());
          assert.equal((await client.dungeon(`dungeon-${runId}`)).nodeId, 'instance');
        } else {
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
          await reconnect.move(301, 2);
          sameActor(session, await reconnect.sessionInfo());
          assert.equal(reconnect.disconnects, 0);
        }
      } else if (name === 'update') {
        await client.move(298, 1);
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
        let sequence = 1;
        let state: WorldState;
        try {
          do {
            if (drainFailure) throw drainFailure;
            sequence++;
            state = await client.move(sequence % 2 === 0 ? 299 : 298, sequence);
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
        const after = await client.move(299, sequence + 1);
        assert.equal(after.nodeId, 'zoneB');
        assert.deepEqual(await client.sessionInfo(), session);
        assert.equal((await client.dungeon(`after-update-${runId}`)).nodeId, 'instance');
        // Exercise fresh Actor creation and movement after the replacement is ready.
        const replacement = create('replacement');
        const fresh = await replacement.join(`replacement-${runId}`, Control.replacementX);
        assert.equal(fresh.nodeId, before.nodeId, 'Replacement did not accept the new Zone');
        const freshSession = await replacement.sessionInfo();
        const moved = await replacement.move(Control.replacementX + 1, 1);
        assert.equal(moved.nodeId, before.nodeId);
        sameActor(freshSession, await replacement.sessionInfo());
        assert.equal(replacement.disconnects, 0);
      } else throw new Error(`Unknown scenario ${name}`);
      if (name !== 'reconnect') assert.equal(client.disconnects, 0);
    }
  } finally {
    const closed = await Promise.allSettled(clients.map((c) => c.close()));
    for (const result of closed)
      if (result.status === 'rejected') console.error('Client cleanup failed', result.reason);
    assert(
      closed.every((r) => r.status === 'fulfilled'),
      'Client cleanup failed'
    );
  }
}
