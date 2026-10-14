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
  JoinTownRequest,
  ChangeChannelRequest,
  CreatePartyRequest,
  JoinPartyRequest,
  EnterDungeonRequest,
  AdvanceRequest,
  ViewRequest,
  SessionInfoRequest,
  type SessionInfo,
  type TownView,
  type DungeonView
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
    for (const name of [PacketNames.townView, PacketNames.dungeonView, PacketNames.actionRejected])
      this.connector.on(name, (message) => {
        if (name === PacketNames.actionRejected)
          this.failure = new Error(`Action rejected: ${JSON.stringify(message.payload)}`);
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
    name: string | string[],
    from: number,
    predicate: (payload: T) => boolean = () => true
  ): Promise<T> {
    const find = (): T | undefined =>
      this.messages.find(
        (m, index) =>
          index >= from &&
          (Array.isArray(name) ? name.includes(m.name) : m.name === name) &&
          predicate(m.payload as T)
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
  async join(playerId: string, channel: number): Promise<TownView | DungeonView> {
    await this.connector.connect();
    const index = this.messages.length;
    await this.connector.send(new JoinTownRequest(playerId, channel)).submit();
    return this.wait<TownView | DungeonView>(
      [PacketNames.townView, PacketNames.dungeonView],
      index
    );
  }
  async sessionInfo(): Promise<SessionInfo> {
    return this.connector.request(new SessionInfoRequest()).submit<SessionInfo>();
  }
  async send<T>(
    request: object,
    name: string,
    predicate: (payload: T) => boolean = () => true
  ): Promise<T> {
    const index = this.messages.length;
    await this.connector.send(request).submit();
    return this.wait<T>(name, index, predicate);
  }
  async dungeon(): Promise<DungeonView> {
    return this.send(new ViewRequest(), PacketNames.dungeonView);
  }
  async close(): Promise<void> {
    await this.connector.close();
  }
}
function sameActor(before: SessionInfo, after: SessionInfo): void {
  assert.equal(after.actorId, before.actorId);
  assert.equal(after.objectGeneration, before.objectGeneration);
}
function sameDungeon(before: DungeonView, after: DungeonView): void {
  assert.equal(after.dungeonId, before.dungeonId);
  assert.equal(after.partyId, before.partyId);
  assert.equal(after.stage, before.stage);
  assert.deepEqual(after.members, before.members);
}
export async function scenarios(
  selected: readonly string[],
  endpoint: string,
  runId: string,
  logDir: string,
  control: NodeControl
): Promise<{ passed: number; errors: unknown[] }> {
  const clients: PlayerClient[] = [],
    errors: unknown[] = [];
  let passed = 0,
    current = selected[0];
  const create = (suffix: string): PlayerClient => {
    const c = new PlayerClient(endpoint, `${logDir}/${suffix}.jsonl`);
    clients.push(c);
    return c;
  };
  const p1 = create('p1'),
    id1 = `p1-${runId}`,
    id2 = `p2-${runId}`;
  async function enter(
    leader: PlayerClient,
    member: PlayerClient,
    partyId: string,
    first: string,
    second: string
  ): Promise<DungeonView> {
    await leader.send<TownView>(new CreatePartyRequest(partyId), PacketNames.townView, (v) =>
      v.parties.some((p) => p.partyId === partyId && p.members.length === 1)
    );
    const mark = leader.messages.length;
    await member.send<TownView>(new JoinPartyRequest(partyId), PacketNames.townView, (v) =>
      v.parties.some((p) => p.partyId === partyId && p.members.length === 2)
    );
    await leader.wait<TownView>(PacketNames.townView, mark, (v) =>
      v.parties.some((p) => p.partyId === partyId && p.members.length === 2)
    );
    const memberMark = member.messages.length;
    const view = await leader.send<DungeonView>(new EnterDungeonRequest(), PacketNames.dungeonView);
    const other = await member.wait<DungeonView>(PacketNames.dungeonView, memberMark);
    sameDungeon(view, other);
    assert.equal(view.nodeId, other.nodeId);
    assert.equal(view.dungeonId, `dungeon-${partyId}`);
    assert.equal(view.stage, 0);
    assert.deepEqual(view.members, [first, second]);
    // Current-Spot replies confirm both deferred joins finished before Advance.
    sameDungeon(view, await leader.dungeon());
    sameDungeon(view, await member.dungeon());
    return view;
  }
  const completed = (name: string): void => {
    passed++;
    console.log(`✓ ${name}`);
  };
  try {
    if (selected.includes('channel')) {
      current = 'channel';
      const initial = (await p1.join(id1, 1)) as TownView;
      assert.equal(initial.townId, 'town-1');
      assert.equal(initial.nodeId, 'townA');
      const session = await p1.sessionInfo();
      assert.equal(session.actorId, id1);
      const changed = await p1.send<TownView>(
        new ChangeChannelRequest(2),
        PacketNames.townView,
        (v) => v.townId === 'town-2'
      );
      assert.equal(changed.nodeId, 'townB');
      assert(changed.players.includes(id1));
      assert.deepEqual(await p1.sessionInfo(), session);
      assert.equal(p1.disconnects, 0);
      completed('channel');
    }
    const dungeonScenarios = selected.filter((name) => name !== 'channel');
    if (dungeonScenarios.length) {
      current = dungeonScenarios[0];
      const initial = p1.connector.isConnected
        ? await p1.send<TownView>(new JoinTownRequest(id1, 2), PacketNames.townView)
        : ((await p1.join(id1, 2)) as TownView);
      assert.equal(initial.townId, 'town-2');
      assert.equal(initial.nodeId, 'townB');
      let p2 = create('p2');
      await p2.join(id2, 2);
      const session1 = await p1.sessionInfo(),
        session2 = await p2.sessionInfo();
      assert.equal(session1.actorId, id1);
      assert.equal(session2.actorId, id2);
      const dungeon = await enter(p1, p2, `party-1-${runId}`, id1, id2);
      const mark = p2.messages.length;
      const advanced = await p1.send<DungeonView>(
        new AdvanceRequest(),
        PacketNames.dungeonView,
        (v) => v.stage === 1
      );
      sameDungeon(
        advanced,
        await p2.wait<DungeonView>(PacketNames.dungeonView, mark, (v) => v.stage === 1)
      );
      assert.equal(advanced.nodeId, dungeon.nodeId);
      assert.deepEqual(await p1.sessionInfo(), session1);
      assert.deepEqual(await p2.sessionInfo(), session2);
      if (selected.includes('dungeon')) completed('dungeon');
      if (selected.includes('reconnect')) {
        current = 'reconnect';
        await p2.close();
        p2 = create('reconnected');
        const restored = (await p2.join(id2, 2)) as DungeonView;
        sameDungeon(advanced, restored);
        assert.equal(restored.nodeId, advanced.nodeId);
        const rebound = await p2.sessionInfo();
        sameActor(session2, rebound);
        assert.notEqual(rebound.sessionId, session2.sessionId);
        completed('reconnect');
      }
      if (selected.includes('update')) {
        current = 'update';
        const before1 = await p1.sessionInfo(),
          before2 = await p2.sessionInfo();
        await control.drain(advanced.nodeId);
        const moved = await p1.dungeon(),
          other = await p2.dungeon();
        sameDungeon(advanced, moved);
        sameDungeon(moved, other);
        assert.equal(moved.nodeId, other.nodeId);
        assert.notEqual(moved.nodeId, advanced.nodeId);
        assert(['instanceA', 'instanceB'].includes(moved.nodeId));
        assert.deepEqual(await p1.sessionInfo(), before1);
        assert.deepEqual(await p2.sessionInfo(), before2);
        assert.equal(p1.disconnects, 0);
        assert.equal(p2.disconnects, 0);
        await control.replace(advanced.nodeId);
        const p3 = create('p3'),
          p4 = create('p4'),
          id3 = `p3-${runId}`,
          id4 = `p4-${runId}`;
        await p3.join(id3, 2);
        await p4.join(id4, 2);
        const fresh = await enter(p3, p4, `party-2-${runId}`, id3, id4);
        assert.equal(fresh.nodeId, advanced.nodeId, 'Replacement did not accept new dungeon');
        completed('update');
      }
      assert.equal(p1.disconnects, 0);
      assert.equal(p2.disconnects, 0);
    }
  } catch (error) {
    errors.push(error);
    console.error(`✗ ${current}`, error);
    for (const name of selected.slice(selected.indexOf(current) + 1))
      console.error(`✗ ${name} (prerequisite ${current} failed)`);
  } finally {
    const results = await Promise.allSettled(clients.map((c) => c.close()));
    for (const result of results) if (result.status === 'rejected') errors.push(result.reason);
  }
  return { passed, errors };
}
