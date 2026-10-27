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
import { Game, Limits, Names, PacketNames } from '../server/shared/constants.js';
import {
  EnterWorldRequest,
  ChangeChannelRequest,
  ChannelView,
  ChannelViewRequest,
  EarnRequest,
  PlayerView,
  ActionRejected,
  GameRules,
  Rejection,
  ViewRequest,
  SessionInfoRequest,
  SessionInfo,
  type WorldInfo
} from '../server/shared/messages.js';
interface NodeControl {
  drain(id: string): Promise<void>;
  replace(id: string): Promise<void>;
  createUpdateZone(id: string): Promise<void>;
}
class PlayerClient {
  readonly connector: ZlinkStreamConnector;
  private readonly views: PlayerView[] = [];
  private readonly channelViews: ChannelView[] = [];
  private readonly rejections: ActionRejected[] = [];
  private readonly changed = new EventEmitter();
  private failure?: Error;
  private closed = false;
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
    this.connector.on<PlayerView>(PlayerView, (message) => {
      this.views.push(message.payload);
      appendFileSync(
        log,
        JSON.stringify(
          { name: PacketNames.playerView, payload: message.payload },
          (_, value) => (typeof value === 'bigint' ? value.toString() : value)
        ) + '\n'
      );
      this.changed.emit('change');
    });
    this.connector.on<ChannelView>(ChannelView, (message) => {
      this.channelViews.push(message.payload);
      appendFileSync(
        log,
        JSON.stringify(
          { name: PacketNames.channelView, payload: message.payload },
          (_, value) => (typeof value === 'bigint' ? value.toString() : value)
        ) + '\n'
      );
      this.changed.emit('change');
    });
    this.connector.on<ActionRejected>(ActionRejected, (message) => {
      this.rejections.push(message.payload);
      appendFileSync(
        log,
        JSON.stringify({ name: PacketNames.actionRejected, payload: message.payload }) + '\n'
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
  private async next<T>(
    values: T[],
    from: number,
    label: string,
    predicate: (value: T) => boolean = () => true
  ): Promise<T> {
    return new Promise<T>((resolve, reject) => {
      const cleanup = (): void => {
        clearTimeout(timer);
        this.changed.off('change', check);
      };
      const check = (): void => {
        const value = values.find((candidate, index) => index >= from && predicate(candidate));
        if (value) {
          cleanup();
          resolve(value);
        } else if (this.failure || !this.connector.isConnected) {
          cleanup();
          reject(this.failure ?? new Error(`Disconnected waiting for ${label}`));
        }
      };
      const timer = setTimeout(() => {
        cleanup();
        reject(new Error(`Timeout waiting for ${label}; log=${this.log}`));
      }, Limits.messageMs);
      this.changed.on('change', check);
      check();
    });
  }
  async connect(): Promise<void> {
    if (!this.connector.isConnected) await this.connector.connect();
  }
  async enter(): Promise<PlayerView> {
    await this.connect();
    return this.response(new EnterWorldRequest(this.playerId));
  }
  async earn(): Promise<PlayerView> {
    return this.response(new EarnRequest());
  }
  async rejectedEarn(): Promise<ActionRejected> {
    const from = this.rejections.length;
    await this.connector.send(new EarnRequest()).submit();
    return this.next(this.rejections, from, 'ActionRejected');
  }
  async rejectedChannel(channel: bigint): Promise<ActionRejected> {
    const from = this.rejections.length;
    await this.connector.send(new ChangeChannelRequest(channel)).submit();
    return this.next(this.rejections, from, 'ActionRejected');
  }
  async channelView(): Promise<ChannelView> {
    const from = this.markChannelViews();
    await this.connector.send(new ChannelViewRequest()).submit();
    return this.waitChannelView(from, () => true);
  }
  markChannelViews(): number {
    return this.channelViews.length;
  }
  waitChannelView(from: number, predicate: (view: ChannelView) => boolean): Promise<ChannelView> {
    return this.next(this.channelViews, from, 'ChannelView', predicate);
  }
  async rejectedInternalPacket(): Promise<ActionRejected> {
    const from = this.rejections.length;
    await this.connector.send(new PlayerView()).submit();
    return this.next(this.rejections, from, 'ActionRejected');
  }
  async waitRetryAfter(retryAfterMs: number): Promise<void> {
    assert(Number.isInteger(retryAfterMs) && retryAfterMs > 0 && retryAfterMs <= 2_147_483_647);
    await new Promise<void>((resolve) => setTimeout(resolve, retryAfterMs));
  }
  async change(channel: bigint): Promise<PlayerView> {
    return this.response(new ChangeChannelRequest(channel));
  }
  async view(): Promise<PlayerView> {
    return this.response(new ViewRequest());
  }
  async sessionInfo(): Promise<SessionInfo> {
    return this.connector.request(new SessionInfoRequest()).submit(SessionInfo);
  }
  async close(): Promise<void> {
    if (this.closed) return;
    this.closed = true;
    await this.connector.close();
  }
}
const Test = { earnedGold: 10n, secondChannel: 2n } as const;
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
    const movingPlayerId = `${a.playerId}-moving`;
    const initialChannelMark = a.markChannelViews();
    const initial = await a.enter();
    assert.deepEqual(initial, {
      worldId: 'A',
      channel: Game.firstChannel,
      nodeId: 'channelA1',
      gold: 0n
    });
    let before: PlayerView;
    let repeatedEarnRejection: ActionRejected | undefined;
    if (name === 'worlds') {
      const acceptedEarn = a.earn();
      const rejectedEarn = a.rejectedEarn();
      before = await acceptedEarn;
      repeatedEarnRejection = await rejectedEarn;
    } else {
      before = await a.earn();
    }
    assert.equal(before.gold, Test.earnedGold);
    const binding = await a.sessionInfo();
    assert.equal(binding.actorId, a.playerId);
    if (name === 'worlds') {
      assert(repeatedEarnRejection);
      const rejected = repeatedEarnRejection;
      assert.equal(rejected.reason, Rejection.earnTooSoon);
      assert(rejected.retryAfterMs > 0);
      assert.equal((await a.view()).gold, before.gold);
      await a.waitRetryAfter(rejected.retryAfterMs);
      assert.equal((await a.earn()).gold, before.gold + GameRules.earnAmount);
      const b = create('B', 'b', a.playerId);
      assert.deepEqual(await b.enter(), {
        worldId: 'B',
        channel: Game.firstChannel,
        nodeId: 'channelB',
        gold: 0n
      });
      assert.equal((await a.view()).gold, before.gold + GameRules.earnAmount);
      assert.equal((await b.sessionInfo()).actorId, a.playerId);
      assert.equal(b.disconnects, 0);
    } else if (name === 'channel' || name === 'reconnect') {
      if (name === 'channel') {
        const same = create('A', 'same-channel', `${a.playerId}-same-channel`);
        const moving = create('A', 'moving', movingPlayerId);
        await Promise.all([same.connect(), moving.connect()]);
        const sameAtA = a.waitChannelView(a.markChannelViews(), (view) =>
          view.playerIds.includes(same.playerId)
        );
        const sameAtSelf = same.waitChannelView(same.markChannelViews(), (view) =>
          view.playerIds.includes(a.playerId)
        );
        await same.enter();
        const [sameAView, sameSelfView] = await Promise.all([sameAtA, sameAtSelf]);
        assert.deepEqual(sameAView.playerIds, [a.playerId, same.playerId].sort());
        assert.deepEqual(sameSelfView.playerIds, [a.playerId, same.playerId].sort());

        const expectedAll = [a.playerId, same.playerId, moving.playerId].sort();
        const allAtA = a.waitChannelView(a.markChannelViews(), (view) =>
          expectedAll.every((playerId) => view.playerIds.includes(playerId))
        );
        const allAtSame = same.waitChannelView(same.markChannelViews(), (view) =>
          expectedAll.every((playerId) => view.playerIds.includes(playerId))
        );
        const allAtMoving = moving.waitChannelView(moving.markChannelViews(), (view) =>
          expectedAll.every((playerId) => view.playerIds.includes(playerId))
        );
        await moving.enter();
        const [aAllView, sameAllView, movingAllView] = await Promise.all([
          allAtA,
          allAtSame,
          allAtMoving
        ]);
        assert.deepEqual(aAllView.playerIds, expectedAll);
        assert.deepEqual(sameAllView.playerIds, expectedAll);
        assert.deepEqual(movingAllView.playerIds, expectedAll);

        const invalidChannel = await a.rejectedChannel(GameRules.maxInteger + 1n);
        assert.deepEqual(invalidChannel, { reason: Rejection.integerOutOfRange, retryAfterMs: 0 });
        assert.deepEqual((await a.channelView()).playerIds, expectedAll);
        assert.deepEqual(await a.rejectedInternalPacket(), {
          reason: Rejection.packetNotAllowed,
          retryAfterMs: 0
        });

        const oldAtA = a.waitChannelView(a.markChannelViews(), (view) =>
          view.playerIds.join() === [a.playerId, same.playerId].sort().join()
        );
        const oldAtSame = same.waitChannelView(same.markChannelViews(), (view) =>
          view.playerIds.join() === [a.playerId, same.playerId].sort().join()
        );
        const movedAtDestination = moving.waitChannelView(moving.markChannelViews(), (view) =>
          view.channelId === Test.secondChannel && view.playerIds.length === 1
        );
        const movedMoving = await moving.change(Test.secondChannel);
        assert.deepEqual(movedMoving, {
          worldId: 'A',
          channel: Test.secondChannel,
          nodeId: 'channelA2',
          gold: 0n
        });
        const [oldAView, oldSameView, destinationView] = await Promise.all([
          oldAtA,
          oldAtSame,
          movedAtDestination
        ]);
        assert.deepEqual(oldAView.playerIds, [a.playerId, same.playerId].sort());
        assert.deepEqual(oldSameView.playerIds, [a.playerId, same.playerId].sort());
        assert.deepEqual(destinationView.playerIds, [moving.playerId]);

        const afterDisconnect = a.waitChannelView(a.markChannelViews(), (view) =>
          view.playerIds.length === 1 && view.playerIds[0] === a.playerId
        );
        await same.close();
        assert.deepEqual((await afterDisconnect).playerIds, [a.playerId]);
      }

      const movedAtDestination = a.waitChannelView(a.markChannelViews(), (view) =>
        view.channelId === Test.secondChannel && view.playerIds[0] === a.playerId
      );
      const moved = await a.change(Test.secondChannel);
      assert.deepEqual(moved, {
        worldId: 'A',
        channel: Test.secondChannel,
        nodeId: 'channelA2',
        gold: Test.earnedGold
      });
      assert.deepEqual(await a.sessionInfo(), binding);
      assert.equal(a.disconnects, 0);
      const expectedDestination =
        name === 'channel' ? [a.playerId, movingPlayerId].sort() : [a.playerId];
      assert.deepEqual((await movedAtDestination).playerIds, expectedDestination);
      const invalidChannel = await a.rejectedChannel(GameRules.maxInteger + 1n);
      assert.deepEqual(invalidChannel, { reason: Rejection.integerOutOfRange, retryAfterMs: 0 });
      assert.deepEqual(await a.view(), moved);
      assert.deepEqual((await a.channelView()).playerIds, expectedDestination);
      assert.deepEqual(await a.rejectedInternalPacket(), {
        reason: Rejection.packetNotAllowed,
        retryAfterMs: 0
      });
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
      await a.waitChannelView(initialChannelMark, (view) =>
        view.channelId === Game.firstChannel &&
        view.playerIds.length === 1 &&
        view.playerIds[0] === a.playerId
      );
      const b = create('B', 'b', a.playerId),
        worldB = await b.enter(),
        bindingB = await b.sessionInfo();
      await control.drain(before.nodeId);
      const moved = await a.view();
      assert.deepEqual(moved, { ...before, nodeId: 'channelA2' });
      assert.deepEqual((await a.channelView()).playerIds, [a.playerId]);
      assert.deepEqual(await a.sessionInfo(), binding);
      assert.deepEqual(await b.view(), worldB);
      assert.deepEqual(await b.sessionInfo(), bindingB);
      await control.replace(before.nodeId);
      assert.deepEqual(await a.view(), moved);
      assert.deepEqual(await a.sessionInfo(), binding);
      await control.createUpdateZone(before.nodeId);
      const newcomer = create('A', 'new', `${name}-${runId}-new`);
      const admitted = await newcomer.enter();
      assert.deepEqual(admitted, {
        worldId: 'A',
        channel: Game.firstChannel,
        nodeId: 'channelA2',
        gold: 0n
      });
      const replacementView = {
        worldId: 'A',
        channel: Game.updateChannel,
        nodeId: before.nodeId,
        gold: 0n
      };
      assert.deepEqual(await newcomer.change(Game.updateChannel), replacementView);
      assert.deepEqual((await newcomer.channelView()).playerIds, [newcomer.playerId]);
      assert.deepEqual(await newcomer.earn(), {
        ...replacementView,
        gold: Test.earnedGold
      });
      assert.equal(newcomer.disconnects, 0);
      assert.equal(b.disconnects, 0);
    } else throw new Error(`Unknown scenario ${name}`);
    if (name !== 'reconnect' && name !== 'channel') assert.equal(a.disconnects, 0);
  } catch (error) {
    failures.push(error);
  } finally {
    const results = await Promise.allSettled(clients.map((c) => c.close()));
    for (const result of results) if (result.status === 'rejected') failures.push(result.reason);
  }
  if (failures.length) throw new AggregateError(failures, `Scenario ${name} or cleanup failed`);
}
