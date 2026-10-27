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
import { Limits, PacketNames } from '../server/shared/constants.js';
import {
  JoinWorldRequest,
  MoveRequest,
  ViewRequest,
  SessionInfoRequest,
  CreatePartyRequest,
  JoinPartyRequest,
  EnterDungeonRequest,
  AdvanceRequest, JoinDungeon,
  type SessionInfo,
  WorldState,
  BorderVisible,
  ActionRejected, PartyView, MaxSafeInteger,
  DungeonView
} from '../server/shared/messages.js';
import { dungeonId } from '../server/shared/constants.js';
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
    for (const packetType of [WorldState, BorderVisible, PartyView, DungeonView, ActionRejected])
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
  async connect(): Promise<void> {
    await this.connector.connect();
  }
  async send<T>(
    message: object,
    name: string,
    predicate: (value: T) => boolean = () => true
  ): Promise<T> {
    const index = this.messages.length;
    await this.connector.send(message).submit();
    return this.wait<T>(name, index, predicate);
  }
  async join(x: number): Promise<WorldState> {
    return this.send(new JoinWorldRequest(this.playerId, x), PacketNames.worldState);
  }
  async sessionInfo(): Promise<SessionInfo> {
    return this.connector.request(new SessionInfoRequest()).submit<SessionInfo>();
  }
  async view(): Promise<DungeonView> {
    return this.send(new ViewRequest(), PacketNames.dungeonView);
  }
  async close(): Promise<void> {
    await this.connector.close();
  }
}
const Positions = {
  fieldObserver: 199,
  fieldOrigin: 198,
  fieldDestination: 200,
  dungeon: 250
} as const;
function sameDungeon(before: DungeonView, after: DungeonView): void {
  assert.deepEqual(after, { ...before, nodeId: after.nodeId }, 'Dungeon state changed');
}
function sameActor(before: SessionInfo, after: SessionInfo): void {
  assert.equal(after.actorId, before.actorId);
  assert.equal(after.objectGeneration, before.objectGeneration);
}
async function enter(a: PlayerClient, b: PlayerClient, party: string): Promise<DungeonView> {
  const created = await a.send<PartyView>(new CreatePartyRequest(), PacketNames.partyView);
  party = created.partyId;
  assert.match(party, /^party-[0-9a-f]{32}$/);
  assert.deepEqual(created.members, [a.playerId]);
  const markA = a.messages.length;
  const joined = await b.send<PartyView>(new JoinPartyRequest(party), PacketNames.partyView, v => v.members.length === 2);
  assert.deepEqual(joined.members, [a.playerId, b.playerId]);
  assert.deepEqual(joined, await a.wait<PartyView>(PacketNames.partyView, markA, v => v.members.length === 2));
  const rejected = await b.send<ActionRejected>(new EnterDungeonRequest(), PacketNames.actionRejected);
  assert.deepEqual(rejected, { reason: 'not-leader', retryAfterMs: 0 });
  assert.deepEqual(await b.send<PartyView>(new ViewRequest(), PacketNames.partyView), joined);
  const markB = b.messages.length, markLeader = a.messages.length;
  await a.connector.send(new EnterDungeonRequest()).submit();
  await a.connector.send(new EnterDungeonRequest()).submit();
  const first = await a.wait<DungeonView>(PacketNames.dungeonView, markLeader, v => v.partyId === party);
  const second = await b.wait<DungeonView>(PacketNames.dungeonView, markB, v => v.partyId === party);
  assert.deepEqual(first, second);
  assert.equal(first.dungeonId, dungeonId(first.runId));
  assert.equal(first.stage, 0);
  assert.equal(first.phase, 'Active');
  assert.match(first.runId, /^run-[0-9a-f]{32}$/);
  assert.deepEqual(first.members, [a.playerId, b.playerId]);
  // Current Spot replies confirm that both deferred joins have completed.
  sameDungeon(first, await a.view());
  sameDungeon(first, await b.view());
  return first;
}
export async function scenario(
  name: string,
  endpoint: string,
  runId: string,
  logDir: string,
  control: NodeControl
): Promise<void> {
  const clients: PlayerClient[] = [];
  const create = async (
    suffix: string,
    id = `${name}-${runId}-${suffix}`
  ): Promise<PlayerClient> => {
    const client = new PlayerClient(endpoint, `${logDir}/${name}-${suffix}.jsonl`, id);
    clients.push(client);
    await client.connect();
    return client;
  };
  const failures: unknown[] = [];
  try {
    const a = await create('p1'),
      b = await create('p2');
    if (name === 'field') {
      const observed = await b.join(Positions.fieldObserver),
        origin = await a.join(Positions.fieldOrigin);
      assert.equal(origin.fieldId, 'field-2');
      assert.equal(origin.nodeId, 'fieldA');
      assert.equal(observed.nodeId, 'fieldA');
      assert.deepEqual(await a.send<ActionRejected>(new JoinPartyRequest('party-' + '0'.repeat(32)),
        PacketNames.actionRejected), { reason: 'invalid-party', retryAfterMs: 0 });
      const party = await a.send<PartyView>(new CreatePartyRequest(), PacketNames.partyView);
      await b.send<PartyView>(new JoinPartyRequest(party.partyId), PacketNames.partyView, v => v.members.length === 2);
      assert.deepEqual(await a.send<ActionRejected>(new JoinDungeon('internal', 'internal'), PacketNames.actionRejected),
        { reason: 'packet-not-allowed', retryAfterMs: 0 });
      const identity = await a.sessionInfo(),
        mark = b.messages.length;
      const moved = await a.send<WorldState>(
        new MoveRequest(Positions.fieldDestination - Positions.fieldOrigin, 1n),
        PacketNames.worldState,
        (v) => v.sequence === 1n
      );
      assert.equal(moved.fieldId, 'field-3');
      assert.equal(moved.nodeId, 'fieldB');
      assert.equal(moved.x, Positions.fieldDestination);
      const visible = await b.wait<BorderVisible>(
        PacketNames.borderVisible,
        mark,
        (v) => v.playerId === a.playerId && v.sequence === 1n
      );
      assert.equal(visible.observerFieldId, 'field-2');
      assert.equal(visible.sourceFieldId, 'field-3');
      assert.equal(visible.x, Positions.fieldDestination);
      assert.deepEqual(await a.sessionInfo(), identity);
      const cross = await a.send<PartyView>(new ViewRequest(), PacketNames.partyView);
      const other = await b.send<PartyView>(new ViewRequest(), PacketNames.partyView);
      assert.deepEqual(cross, other);
      assert.equal(cross.partyId, party.partyId);
      assert.deepEqual(cross.members, [a.playerId, b.playerId]);
      assert.equal(cross.leaderId, a.playerId);
      for (const [request, reason] of [
        [new MoveRequest(11, 2n), 'step-too-large'],
        [new MoveRequest(0, MaxSafeInteger + 1n), 'integer-out-of-range'],
        [new MoveRequest(0, 1n), 'stale-sequence']
      ] as const) {
        assert.deepEqual(await a.send<ActionRejected>(request, PacketNames.actionRejected), { reason, retryAfterMs: 0 });
        const state = await a.send<WorldState>(new ViewRequest(), PacketNames.worldState);
        assert.equal(state.x, moved.x); assert.equal(state.sequence, moved.sequence);
      }
      const markMove = a.messages.length;
      await a.connector.send(new MoveRequest(0, 2n)).submit();
      await a.connector.send(new MoveRequest(10, 3n)).submit();
      await a.wait<WorldState>(PacketNames.worldState, markMove, v => v.sequence === 2n);
      const rejection = await a.wait<ActionRejected>(PacketNames.actionRejected, markMove, v => v.reason === 'move-budget');
      assert.ok(rejection.retryAfterMs > 0);
      const unchanged = await a.send<WorldState>(new ViewRequest(), PacketNames.worldState);
      assert.equal(unchanged.x, moved.x); assert.equal(unchanged.sequence, 2n);
      await new Promise(resolve => setTimeout(resolve, rejection.retryAfterMs));
      await a.send<WorldState>(new MoveRequest(10, 3n), PacketNames.worldState, v => v.sequence === 3n);

    } else {
      await a.join(Positions.dungeon);
      await b.join(Positions.dungeon);
      const sa = await a.sessionInfo(),
        sb = await b.sessionInfo();
      const initial = await enter(a, b, `party-${runId}`),
        mark = b.messages.length;
      let advanced = await a.send<DungeonView>(
        new AdvanceRequest(),
        PacketNames.dungeonView,
        (v) => v.stage === 1
      );
      assert.deepEqual(
        advanced,
        await b.wait<DungeonView>(PacketNames.dungeonView, mark, (v) => v.stage === 1)
      );
      assert.equal(advanced.nodeId, initial.nodeId);
      for (let stage = 2; stage <= 3; stage++)
        advanced = await a.send<DungeonView>(new AdvanceRequest(), PacketNames.dungeonView, v => v.stage === stage);
      assert.equal(advanced.phase, 'Completed');
      assert.deepEqual(await a.send<ActionRejected>(new AdvanceRequest(), PacketNames.actionRejected),
        { reason: 'run-not-active', retryAfterMs: 0 });
      sameDungeon(advanced, await a.view());
      assert.deepEqual(await a.sessionInfo(), sa);
      assert.deepEqual(await b.sessionInfo(), sb);
      if (name === 'reconnect') {
        await b.close();
        const again = await create('reconnected', b.playerId);
        const restored = await again.send<DungeonView>(
          new JoinWorldRequest(again.playerId, Positions.dungeon),
          PacketNames.dungeonView
        );
        assert.deepEqual(restored, advanced);
        const identity = await again.sessionInfo();
        sameActor(sb, identity);
        assert.notEqual(identity.sessionId, sb.sessionId);
        assert.equal(again.disconnects, 0);
      } else if (name === 'update') {
        await control.drain(advanced.nodeId);
        const afterA = await a.view(),
          afterB = await b.view();
        sameDungeon(advanced, afterA);
        assert.deepEqual(afterA, afterB);
        assert.notEqual(afterA.nodeId, advanced.nodeId);
        assert.deepEqual(await a.sessionInfo(), sa);
        assert.deepEqual(await b.sessionInfo(), sb);
        assert.equal(a.disconnects, 0);
        assert.equal(b.disconnects, 0);
        await control.replace(advanced.nodeId);
        const c = await create('new-p1'),
          d = await create('new-p2');
        await c.join(Positions.dungeon);
        await d.join(Positions.dungeon);
        const fresh = await enter(c, d, `replacement-${runId}`);
        assert.notEqual(fresh.runId, advanced.runId);
        assert.notEqual(fresh.dungeonId, advanced.dungeonId);
        assert.equal(fresh.nodeId, advanced.nodeId, 'Replacement instance did not accept new work');
        assert.deepEqual(await a.sessionInfo(), sa);
        assert.deepEqual(await b.sessionInfo(), sb);
      } else {
        assert.equal(name, 'dungeon');
        const c = await create('other-p1'), d = await create('other-p2');
        await c.join(Positions.dungeon); await d.join(Positions.dungeon);
        const other = await enter(c, d, '');
        assert.notEqual(other.dungeonId, advanced.dungeonId);
        assert.notEqual(other.runId, advanced.runId);
      }
    }
    assert.equal(a.disconnects, 0);
    if (name !== 'reconnect') assert.equal(b.disconnects, 0);
  } catch (error) {
    failures.push(error);
  } finally {
    const results = await Promise.allSettled(clients.map((c) => c.close()));
    for (const result of results) if (result.status === 'rejected') failures.push(result.reason);
  }
  if (failures.length)
    throw new AggregateError(failures, `Scenario ${name} or client cleanup failed`);
}
