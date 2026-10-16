import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { appendFileSync } from 'node:fs';
import {
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec,
  ZlinkStreamDispatchMode,
  type ZlinkStreamConnector
} from '@zlink-systems/stream-connector';
import {
  EnterRegionRequest,
  ViewRequest,
  RegionView,
  MoveRequest,
  SessionInfo,
  SessionInfoRequest,
  OpenDungeonRequest,
  DungeonOpened
} from '../server/shared/messages.js';
import { Limits, PacketNames, regionId } from '../server/shared/constants.js';
interface Received {
  name: string;
  payload: unknown;
}
function sameState(before: RegionView, after: RegionView): void {
  assert.equal(after.regionId, before.regionId);
  assert.deepEqual(after.players, before.players);
}
const Test = { region: 'forest', freshRegion: 'desert', movedX: 42, sequence: 1 } as const;
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
  const enter = async (
    client: PlayerClient,
    region = Test.region as string
  ): Promise<RegionView> => {
    await client.enter(region);
    return client.wait<RegionView>(PacketNames.regionView, 0);
  };
  const failures: unknown[] = [];
  let a = create('p1');
  const b = create('p2');
  const c = name === 'region' ? create('p3') : undefined;
  try {
    const initial = await enter(a);
    const together = await enter(b);
    assert.equal(initial.regionId, regionId(Test.region, 1));
    assert.equal(together.regionId, initial.regionId);
    assert.deepEqual(
      together.players.map((p) => p.playerId).sort(),
      [a.playerId, b.playerId].sort()
    );
    sameState(
      together,
      await a.wait<RegionView>(PacketNames.regionView, 0, (v) => v.players.length === 2)
    );
    const separate = c ? await enter(c) : undefined;
    if (separate && c) {
      assert.equal(separate.regionId, regionId(Test.region, 2));
      assert.deepEqual(separate.players, [{ playerId: c.playerId, x: 0 }]);
    }
    const identity = await a.sessionInfo(),
      otherIdentity = await b.sessionInfo();
    assert.equal(identity.actorId, a.playerId);
    assert.equal(otherIdentity.actorId, b.playerId);
    const ma = a.messages.length,
      mb = b.messages.length,
      mc = c?.messages.length ?? 0;
    await a.connector.send(new MoveRequest(Test.movedX, Test.sequence)).submit();
    const moved = (v: RegionView): boolean =>
      v.players.some((p) => p.playerId === a.playerId && p.x === Test.movedX);
    const before = await a.wait<RegionView>(PacketNames.regionView, ma, moved);
    sameState(before, await b.wait<RegionView>(PacketNames.regionView, mb, moved));
    if (separate && c) {
      sameState(separate, await c.view());
      assert(
        c.messages
          .slice(mc)
          .every(
            (m) =>
              m.name !== PacketNames.regionView ||
              (m.payload as RegionView).regionId === separate.regionId
          ),
        'Movement leaked into another region'
      );
    }
    if (name === 'region') {
      assert.equal(before.players.length, 2);
    } else if (name === 'dungeon') {
      const mark = a.messages.length,
        id = `dungeon-${runId}`;
      await a.connector.send(new OpenDungeonRequest(id)).submit();
      const opened = await a.wait<DungeonOpened>(PacketNames.dungeonOpened, mark);
      assert.equal(opened.dungeonId, id);
      assert.equal(opened.nodeId, 'instance');
    } else if (name === 'reconnect') {
      await a.close();
      a = create('reconnected', a.playerId);
      const restored = await enter(a);
      sameState(before, restored);
      assert.equal(restored.nodeId, before.nodeId);
      const after = await a.sessionInfo();
      assert.equal(after.actorId, identity.actorId);
      assert.equal(after.objectGeneration, identity.objectGeneration);
      assert.notEqual(after.sessionId, identity.sessionId);
    } else if (name === 'update') {
      await control.drain(before.nodeId);
      assert.deepEqual(await a.sessionInfo(), identity);
      assert.deepEqual(await b.sessionInfo(), otherIdentity);
      const after = await a.view(),
        other = await b.view();
      sameState(before, after);
      sameState(before, other);
      assert.notEqual(after.nodeId, before.nodeId);
      assert.equal(other.nodeId, after.nodeId);
      await control.replace(before.nodeId);
      const fresh = await enter(create('p4'), Test.freshRegion);
      assert(
        control.isLiveRegion(fresh.nodeId),
        'New region was not accepted by a live Region node'
      );
      assert.deepEqual(await a.sessionInfo(), identity);
      assert.deepEqual(await b.sessionInfo(), otherIdentity);
    } else throw new Error(`Unknown scenario ${name}`);
    a.verifyConnected();
    b.verifyConnected();
    c?.verifyConnected();
  } catch (error) {
    failures.push(error);
  } finally {
    const results = await Promise.allSettled(clients.map((c) => c.close()));
    for (const result of results) if (result.status === 'rejected') failures.push(result.reason);
  }
  if (failures.length)
    throw new AggregateError(failures, `Scenario ${name} or client cleanup failed`);
}
export interface NodeControl {
  isLiveRegion(id: string): boolean;
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
    for (const name of [PacketNames.regionView, PacketNames.dungeonOpened])
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
    if (this.failure) throw this.failure;
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
        if (this.failure) {
          cleanup();
          reject(this.failure);
        } else if (value !== undefined) {
          cleanup();
          resolve(value);
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
  async enter(region: string): Promise<void> {
    await this.connector.connect();
    await this.connector.send(new EnterRegionRequest(this.playerId, region)).submit();
  }
  async sessionInfo(): Promise<SessionInfo> {
    return this.connector.request(new SessionInfoRequest()).submit<SessionInfo>();
  }
  async view(): Promise<RegionView> {
    const index = this.messages.length;
    await this.connector.send(new ViewRequest()).submit();
    return this.wait<RegionView>(PacketNames.regionView, index);
  }
  async close(): Promise<void> {
    await this.connector.close();
  }
  verifyConnected(): void {
    if (this.failure) throw this.failure;
    assert(this.connector.isConnected, 'Client connection was lost');
    assert.equal(this.disconnects, 0);
  }
}
