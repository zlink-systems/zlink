import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { appendFileSync } from 'node:fs';
import {
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec,
  ZlinkStreamDispatchMode,
  type ZlinkStreamConnector
} from '@zlink-systems/stream-connector';
import { Game, Limits, Names, PacketNames } from '../server/shared/constants.js';
import {
  EnterWorldRequest,
  ChangeChannelRequest,
  EarnRequest,
  ViewRequest,
  SessionInfoRequest,
  type PlayerView,
  type SessionInfo,
  type WorldInfo
} from '../server/shared/messages.js';
interface NodeControl {
  drain(id: string): Promise<void>;
  replace(id: string): Promise<void>;
  accepted(id: string, playerId: string): Promise<void>;
}
class PlayerClient {
  readonly connector: ZlinkStreamConnector;
  private readonly views: PlayerView[] = [];
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
    this.connector.on<PlayerView>(PacketNames.playerView, (message) => {
      this.views.push(message.payload);
      appendFileSync(
        log,
        JSON.stringify({ name: PacketNames.playerView, payload: message.payload }) + '\n'
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
  private async response(message: object): Promise<PlayerView> {
    const from = this.views.length;
    await this.connector.send(message).submit();
    return new Promise<PlayerView>((resolve, reject) => {
      const cleanup = (): void => {
        clearTimeout(timer);
        this.changed.off('change', check);
      };
      const check = (): void => {
        const view = this.views[from];
        if (view) {
          cleanup();
          resolve(view);
        } else if (this.failure || !this.connector.isConnected) {
          cleanup();
          reject(this.failure ?? new Error('Disconnected waiting for PlayerView'));
        }
      };
      const timer = setTimeout(() => {
        cleanup();
        reject(new Error(`Timeout waiting for PlayerView; log=${this.log}`));
      }, Limits.messageMs);
      this.changed.on('change', check);
      check();
    });
  }
  async enter(): Promise<PlayerView> {
    await this.connector.connect();
    return this.response(new EnterWorldRequest(this.playerId));
  }
  async earn(amount: number): Promise<PlayerView> {
    return this.response(new EarnRequest(amount));
  }
  async change(channel: number): Promise<PlayerView> {
    return this.response(new ChangeChannelRequest(channel));
  }
  async view(): Promise<PlayerView> {
    return this.response(new ViewRequest());
  }
  async sessionInfo(): Promise<SessionInfo> {
    return this.connector.request(new SessionInfoRequest()).submit<SessionInfo>();
  }
  async close(): Promise<void> {
    await this.connector.close();
  }
}
const Test = { earnedGold: 10, secondChannel: 2, firstChannel: Game.firstChannel } as const;
export async function scenario(
  name: string,
  endpoints: Record<string, string>,
  accounts: string,
  runId: string,
  logDir: string,
  control: NodeControl
): Promise<void> {
  const clients: PlayerClient[] = [];
  const create = (world: string, suffix: string, playerId = `${name}-${runId}`): PlayerClient => {
    const client = new PlayerClient(
      endpoints[world],
      `${logDir}/${name}-${suffix}.jsonl`,
      playerId
    );
    clients.push(client);
    return client;
  };
  const failures: unknown[] = [];
  try {
    const response = await fetch(accounts + Names.worldsPath, {
      signal: AbortSignal.timeout(Limits.messageMs)
    });
    assert.equal(response.status, 200);
    const worlds = (await response.json()) as WorldInfo[];
    assert.equal(worlds.length, Object.keys(endpoints).length);
    assert.equal(new Set(worlds.map((w) => w.worldId)).size, worlds.length);
    for (const world of worlds) {
      assert(world.name.length);
      assert.equal(world.sessionEndpoint, endpoints[world.worldId]);
    }
    const a = create('A', 'a');
    const initial = await a.enter();
    assert.deepEqual(initial, {
      worldId: 'A',
      channel: Game.firstChannel,
      nodeId: 'channelA1',
      gold: 0
    });
    const before = await a.earn(Test.earnedGold);
    assert.equal(before.gold, Test.earnedGold);
    const binding = await a.sessionInfo();
    assert.equal(binding.actorId, a.playerId);
    if (name === 'worlds') {
      const b = create('B', 'b', a.playerId);
      assert.deepEqual(await b.enter(), {
        worldId: 'B',
        channel: Game.firstChannel,
        nodeId: 'channelB',
        gold: 0
      });
      assert.equal((await a.view()).gold, Test.earnedGold);
      assert.equal((await b.sessionInfo()).actorId, a.playerId);
      assert.equal(b.disconnects, 0);
    } else if (name === 'channel' || name === 'reconnect') {
      const moved = await a.change(Test.secondChannel);
      assert.deepEqual(moved, {
        worldId: 'A',
        channel: Test.secondChannel,
        nodeId: 'channelA2',
        gold: Test.earnedGold
      });
      assert.deepEqual(await a.sessionInfo(), binding);
      assert.equal(a.disconnects, 0);
      if (name === 'reconnect') {
        await a.close();
        const again = create('A', 'again', a.playerId);
        assert.deepEqual(await again.enter(), moved);
        const after = await again.sessionInfo();
        assert.equal(after.actorId, binding.actorId);
        assert.equal(after.objectGeneration, binding.objectGeneration);
        assert.notEqual(after.sessionId, binding.sessionId);
        assert.equal(again.disconnects, 0);
      }
    } else if (name === 'update') {
      const b = create('B', 'b', a.playerId),
        worldB = await b.enter(),
        bindingB = await b.sessionInfo();
      await control.drain(before.nodeId);
      const moved = await a.view();
      assert.deepEqual(moved, { ...before, nodeId: 'channelA2' });
      assert.deepEqual(await a.sessionInfo(), binding);
      assert.deepEqual(await b.view(), worldB);
      assert.deepEqual(await b.sessionInfo(), bindingB);
      await control.replace(before.nodeId);
      assert.deepEqual(await a.view(), moved);
      assert.deepEqual(await a.sessionInfo(), binding);
      const newcomer = create('A', 'new', `${name}-${runId}-new`);
      const admitted = await newcomer.enter();
      await control.accepted(before.nodeId, newcomer.playerId);
      assert.equal(admitted.worldId, 'A');
      assert.equal(admitted.nodeId, 'channelA2');
      assert.equal((await newcomer.earn(Test.earnedGold)).gold, Test.earnedGold);
      assert.equal(newcomer.disconnects, 0);
      assert.equal(b.disconnects, 0);
    } else throw new Error(`Unknown scenario ${name}`);
    if (name !== 'reconnect') assert.equal(a.disconnects, 0);
  } catch (error) {
    failures.push(error);
  } finally {
    const results = await Promise.allSettled(clients.map((c) => c.close()));
    for (const result of results) if (result.status === 'rejected') failures.push(result.reason);
  }
  if (failures.length) throw new AggregateError(failures, `Scenario ${name} or cleanup failed`);
}
