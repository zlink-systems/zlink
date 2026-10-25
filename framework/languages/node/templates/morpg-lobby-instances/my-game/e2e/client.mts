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
import { Limits, PacketNames, Names, Rejection } from '../server/shared/constants.js';
import {
  JoinLobbyRequest, LobbyView,
  CreatePartyRequest,
  JoinPartyRequest,
  ReadyRequest,
  EnterDungeonRequest,
  AdvanceRequest,
  ViewRequest,
  SessionInfoRequest,
  type SessionInfo,
  PartyView,
  DungeonView,
  JoinDungeon, RunPhase, FinalStage,
  ActionRejected
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
    for (const packetType of [LobbyView, PartyView, DungeonView, ActionRejected])
      this.connector.on(packetType, (message) => {
        const name = packetType.name;
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
  async login(): Promise<void> {
    await this.connector.connect();
    await this.connector.send(new JoinLobbyRequest(this.playerId)).submit();
  }
  async sessionInfo(): Promise<SessionInfo> {
    return this.connector.request(new SessionInfoRequest()).submit<SessionInfo>();
  }
  async view(): Promise<DungeonView> {
    const index = this.messages.length;
    await this.connector.send(new ViewRequest()).submit();
    return this.wait<DungeonView>(PacketNames.dungeonView, index);
  }
  async close(): Promise<void> {
    await this.connector.close();
  }
}
function sameState(before: DungeonView, after: DungeonView): void {
  assert.deepEqual(after, { ...before, nodeId: after.nodeId }, 'Dungeon state changed');
}
function sameActor(before: SessionInfo, after: SessionInfo): void {
  assert.equal(after.actorId, before.actorId);
  assert.equal(after.objectGeneration, before.objectGeneration);
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
  async function enter(a: PlayerClient, b: PlayerClient): Promise<DungeonView> {
    await a.login();
    await a.wait(PacketNames.lobbyView, 0);
    await b.login();
    await b.wait(PacketNames.lobbyView, 0);
    let ma = a.messages.length,
      mb = b.messages.length;
    await a.connector.send(new JoinPartyRequest('party-' + runId.replaceAll('-', ''))).submit();
    assert.deepEqual(await a.wait<ActionRejected>(PacketNames.actionRejected, ma), { reason: Rejection.invalidParty, retryAfterMs: 0 });
    ma = a.messages.length;
    await a.connector.send(new CreatePartyRequest()).submit();
    const first = await a.wait<PartyView>(PacketNames.partyView, ma);
    const partyId = first.partyId; assert.match(partyId, /^party-[0-9a-f]{32}$/);
    assert.deepEqual(first, { partyId, members: [a.playerId], ready: [false], phase: RunPhase.Forming, runId: null });
    ma = a.messages.length;
    await b.connector.send(new JoinPartyRequest(partyId)).submit();
    const joined = await Promise.all([
      a.wait<PartyView>(PacketNames.partyView, ma),
      b.wait<PartyView>(PacketNames.partyView, mb)
    ]);
    assert.deepEqual(joined[0], joined[1]);
    assert.deepEqual(joined[0], {
      partyId,
      members: [a.playerId, b.playerId],
      ready: [false, false], phase: RunPhase.Forming, runId: null
    });
    ma = a.messages.length;
    await a.connector.send(new EnterDungeonRequest()).submit();
    assert.equal(
      (await a.wait<ActionRejected>(PacketNames.actionRejected, ma)).reason,
      Rejection.notReady
    );
    const unchanged = async (client: PlayerClient): Promise<void> => {
      const index = client.messages.length;
      await client.connector.send(new ViewRequest()).submit();
      assert.deepEqual(await client.wait<PartyView>(PacketNames.partyView, index), joined[0]);
    };
    await unchanged(a);
    mb = b.messages.length; await b.connector.send(new EnterDungeonRequest()).submit();
    assert.deepEqual(await b.wait<ActionRejected>(PacketNames.actionRejected, mb), { reason: Rejection.notLeader, retryAfterMs: 0 });
    await unchanged(b);
    mb = b.messages.length; await b.connector.send(new JoinDungeon('dungeon-forged', 'run-forged')).submit();
    assert.deepEqual(await b.wait<ActionRejected>(PacketNames.actionRejected, mb), { reason: Rejection.packetNotAllowed, retryAfterMs: 0 });
    await unchanged(b);
    for (const [actor, readiness] of [
      [a, [true, false]],
      [b, [true, true]]
    ] as const) {
      ma = a.messages.length;
      mb = b.messages.length;
      await actor.connector.send(new ReadyRequest()).submit();
      const views = await Promise.all([
        a.wait<PartyView>(PacketNames.partyView, ma),
        b.wait<PartyView>(PacketNames.partyView, mb)
      ]);
      assert.deepEqual(views[0], views[1]);
      assert.deepEqual(views[0].ready, readiness);
    }
    ma = a.messages.length;
    mb = b.messages.length;
    await a.connector.send(new EnterDungeonRequest()).submit();
    await a.connector.send(new EnterDungeonRequest()).submit();
    const arrived = await Promise.all([
      a.wait<DungeonView>(PacketNames.dungeonView, ma, view => view.phase === RunPhase.Active),
      b.wait<DungeonView>(PacketNames.dungeonView, mb, view => view.phase === RunPhase.Active)
    ]);
    assert.deepEqual(arrived[0], arrived[1]);
    assert.equal(arrived[0].stage, 0n);
    assert.match(arrived[0].runId!, /^run-[0-9a-f]{32}$/);
    assert.equal(arrived[0].dungeonId, Names.dungeonPrefix + arrived[0].runId);
    assert.equal(arrived[0].partyId, partyId);
    assert.deepEqual(arrived[0].members, [a.playerId, b.playerId]);
    // A query from each current Spot confirms deferred joins finished.
    sameState(arrived[0], await a.view());
    sameState(arrived[1], await b.view());
    return arrived[0];
  }
  async function advance(a: PlayerClient, b: PlayerClient, stage: bigint): Promise<DungeonView> {
    const ma = a.messages.length,
      mb = b.messages.length;
    await a.connector.send(new AdvanceRequest()).submit();
    const views = await Promise.all([
      a.wait<DungeonView>(PacketNames.dungeonView, ma),
      b.wait<DungeonView>(PacketNames.dungeonView, mb)
    ]);
    assert.deepEqual(views[0], views[1]);
    assert.equal(views[0].stage, stage);
    return views[0];
  }
  const a = create('p1'),
    b = create('p2');
  const failures: unknown[] = [];
  try {
    const initial = await enter(a, b);
    const sa = await a.sessionInfo(),
      sb = await b.sessionInfo();
    assert.equal(sa.actorId, a.playerId);
    assert.equal(sb.actorId, b.playerId);
    const before = await advance(a, b, 1n);
    assert.equal(before.nodeId, initial.nodeId);
    assert.deepEqual(await a.sessionInfo(), sa);
    assert.deepEqual(await b.sessionInfo(), sb);
    if (name === 'reconnect') {
      await b.close();
      const again = create('reconnected', b.playerId);
      await again.login();
      const restored = await again.wait<DungeonView>(PacketNames.dungeonView, 0);
      sameState(before, restored);
      assert.equal(restored.nodeId, before.nodeId);
      const identity = await again.sessionInfo();
      sameActor(sb, identity);
      assert.notEqual(identity.sessionId, sb.sessionId);
      await advance(a, again, 2n);
      assert.equal(again.disconnects, 0);
    } else if (name === 'update') {
      await control.drain(before.nodeId);
      assert.deepEqual(await a.sessionInfo(), sa);
      assert.deepEqual(await b.sessionInfo(), sb);
      const afterA = await a.view(),
        afterB = await b.view();
      assert.deepEqual(afterA, afterB);
      assert.notEqual(afterA.nodeId, before.nodeId);
      sameState(before, afterA);
      await control.replace(before.nodeId);
      const freshA = create('new-a'), freshB = create('new-b');
      const fresh = await enter(freshA, freshB);
      await advance(freshA, freshB, 1n);
      assert.equal(fresh.nodeId, before.nodeId, 'Replacement did not accept new Dungeon');
      await advance(a, b, 2n);
      assert.deepEqual(await a.sessionInfo(), sa);
      assert.deepEqual(await b.sessionInfo(), sb);
      assert.notEqual(fresh.runId, initial.runId); assert.notEqual(fresh.dungeonId, initial.dungeonId);
    } else {
      assert.equal(name, 'party');
      const separate = await enter(create('second-a'), create('second-b'));
      assert.notEqual(separate.runId, initial.runId); assert.notEqual(separate.dungeonId, initial.dungeonId);
      await advance(a, b, 2n);
    }
    const currentB = name === 'reconnect' ? clients[clients.length - 1] : b;
    const completed = await advance(a, currentB, FinalStage);
    assert.equal(completed.phase, RunPhase.Completed);
    const index = a.messages.length; await a.connector.send(new AdvanceRequest()).submit();
    assert.deepEqual(await a.wait<ActionRejected>(PacketNames.actionRejected, index), { reason: Rejection.runNotActive, retryAfterMs: 0 });
    sameState(completed, await a.view());
    assert.equal(a.disconnects, 0);
    if (name !== 'reconnect') assert.equal(b.disconnects, 0);
  } catch (error) {
    failures.push(error);
  } finally {
    for (const result of await Promise.allSettled(clients.map((c) => c.close())))
      if (result.status === 'rejected') failures.push(result.reason);
  }
  if (failures.length)
    throw new AggregateError(failures, `Scenario ${name} or client cleanup failed`);
}
