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
  AdvanceRequest,
  type SessionInfo,
  type WorldState,
  type BorderVisible,
  type FieldView,
  type DungeonView
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
    for (const name of [
      PacketNames.worldState,
      PacketNames.borderVisible,
      PacketNames.fieldView,
      PacketNames.dungeonView,
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
  const owns = (v: FieldView, count: number): boolean =>
    v.parties.some((p) => p.partyId === party && p.members.length === count);
  const created = await a.send<FieldView>(
    new CreatePartyRequest(party),
    PacketNames.fieldView,
    (v) => owns(v, 1)
  );
  assert.deepEqual(created.parties.find((p) => p.partyId === party)!.members, [a.playerId]);
  const markA = a.messages.length;
  const joined = await b.send<FieldView>(new JoinPartyRequest(party), PacketNames.fieldView, (v) =>
    owns(v, 2)
  );
  assert.deepEqual(joined.parties.find((p) => p.partyId === party)!.members, [
    a.playerId,
    b.playerId
  ]);
  assert.deepEqual(
    joined,
    await a.wait<FieldView>(PacketNames.fieldView, markA, (v) => owns(v, 2))
  );
  const markB = b.messages.length;
  const first = await a.send<DungeonView>(
    new EnterDungeonRequest(),
    PacketNames.dungeonView,
    (v) => v.partyId === party
  );
  const second = await b.wait<DungeonView>(
    PacketNames.dungeonView,
    markB,
    (v) => v.partyId === party
  );
  assert.deepEqual(first, second);
  assert.equal(first.dungeonId, dungeonId(party));
  assert.equal(first.stage, 0);
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
      const identity = await a.sessionInfo(),
        mark = b.messages.length;
      const moved = await a.send<WorldState>(
        new MoveRequest(Positions.fieldDestination, 1),
        PacketNames.worldState,
        (v) => v.sequence === 1
      );
      assert.equal(moved.fieldId, 'field-3');
      assert.equal(moved.nodeId, 'fieldB');
      assert.equal(moved.x, Positions.fieldDestination);
      const visible = await b.wait<BorderVisible>(
        PacketNames.borderVisible,
        mark,
        (v) => v.playerId === a.playerId && v.sequence === 1
      );
      assert.equal(visible.observerFieldId, 'field-2');
      assert.equal(visible.sourceFieldId, 'field-3');
      assert.equal(visible.x, Positions.fieldDestination);
      assert.deepEqual(await a.sessionInfo(), identity);
    } else {
      await a.join(Positions.dungeon);
      await b.join(Positions.dungeon);
      const sa = await a.sessionInfo(),
        sb = await b.sessionInfo();
      const initial = await enter(a, b, `party-${runId}`),
        mark = b.messages.length;
      const advanced = await a.send<DungeonView>(
        new AdvanceRequest(),
        PacketNames.dungeonView,
        (v) => v.stage === 1
      );
      assert.deepEqual(
        advanced,
        await b.wait<DungeonView>(PacketNames.dungeonView, mark, (v) => v.stage === 1)
      );
      assert.equal(advanced.nodeId, initial.nodeId);
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
        assert.equal(fresh.nodeId, advanced.nodeId, 'Replacement instance did not accept new work');
        assert.deepEqual(await a.sessionInfo(), sa);
        assert.deepEqual(await b.sessionInfo(), sb);
      } else assert.equal(name, 'dungeon');
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
