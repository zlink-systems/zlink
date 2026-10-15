import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { appendFileSync } from 'node:fs';
import {
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec,
  ZlinkStreamDispatchMode,
  type ZlinkStreamConnector
} from '@zlink-systems/stream-connector';
import { Game, Limits, PacketNames, Reasons } from '../server/shared/constants.js';
import {
  BetRequest,
  JoinTableRequest,
  SessionInfoRequest,
  StartHandRequest,
  TableViewRequest,
  type ActionRejected,
  type HandDealt,
  type HandResult,
  type SessionInfo,
  type TableView
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
      PacketNames.tableView,
      PacketNames.handDealt,
      PacketNames.handResult,
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
  async join(playerId: string, tableId: string): Promise<TableView> {
    await this.connector.connect();
    const index = this.messages.length;
    await this.connector.send(new JoinTableRequest(playerId, tableId)).submit();
    const view = await this.wait<TableView>(
      PacketNames.tableView,
      index,
      (v) => v.tableId === tableId
    );
    assert.equal(view.tableId, tableId);
    assert.equal((await this.sessionInfo()).actorId, playerId);
    return view;
  }
  async sessionInfo(): Promise<SessionInfo> {
    return this.connector.request(new SessionInfoRequest()).submit<SessionInfo>();
  }
  async view(): Promise<TableView> {
    const index = this.messages.length;
    await this.connector.send(new TableViewRequest()).submit();
    return this.wait<TableView>(PacketNames.tableView, index);
  }
  async close(): Promise<void> {
    await this.connector.close();
  }
}
function chips(view: { seats: { chips: number }[] }): number {
  return view.seats.reduce((sum, s) => sum + s.chips, 0);
}
function identity(before: TableView, after: TableView): void {
  for (const key of ['yourSeat', 'handNumber', 'inHand', 'turnSeat', 'pot'] as const)
    assert.equal(after[key], before[key], `Preserved ${key}`);
  assert.deepEqual(after.seats, before.seats);
}
function privacy(client: PlayerClient, cards: number[]): void {
  for (const message of client.messages) {
    if (message.name === PacketNames.handResult) continue;
    if (message.name === PacketNames.handDealt) {
      assert.deepEqual((message.payload as HandDealt).cards, cards);
      continue;
    }
    function inspect(value: unknown): void {
      if (!value || typeof value !== 'object') return;
      for (const [key, v] of Object.entries(value)) {
        assert.notEqual(key, 'cards', 'Private cards outside owner HandDealt');
        inspect(v);
      }
    }
    inspect(message.payload);
  }
}
function settlement(
  result: HandResult,
  first: HandDealt,
  second: HandDealt,
  amounts: number[]
): void {
  assert.equal(result.handNumber, first.handNumber);
  assert.equal(
    result.pot,
    amounts.reduce((a, b) => a + b, 0)
  );
  assert.deepEqual(
    result.hands.map((h) => h.seat),
    [0, 1]
  );
  assert.deepEqual(
    result.hands.map((h) => h.cards),
    [first.cards, second.cards]
  );
  const scores = [first.cards, second.cards].map((cards) =>
    cards.reduce((sum, c) => sum + (c % Game.ranks) + Game.lowestRank, 0)
  );
  const high = Math.max(...scores);
  const winners = scores.flatMap((score, i) => (score === high ? [i] : []));
  assert.deepEqual(result.winners, winners);
  const expected = amounts.map((amount) => Game.initialChips - amount);
  for (const winner of winners) expected[winner] += Math.floor(result.pot / winners.length);
  expected[winners[0]] += result.pot % winners.length;
  assert.deepEqual(
    result.seats.map((s) => s.chips),
    expected
  );
  assert.equal(chips(result), 2 * Game.initialChips);
}
export async function scenario(
  name: string,
  endpoint: string,
  runId: string,
  logDir: string,
  control: NodeControl
): Promise<void> {
  const tableId = `table-${name}-${runId}`,
    ids = [`p1-${name}-${runId}`, `p2-${name}-${runId}`];
  const clients: PlayerClient[] = [];
  const create = (suffix: string): PlayerClient => {
    const client = new PlayerClient(endpoint, `${logDir}/${name}-${suffix}.jsonl`);
    clients.push(client);
    return client;
  };
  const p1 = create('p1');
  let p2 = create('p2');
  try {
    const first = await p1.join(ids[0], tableId),
      second = await p2.join(ids[1], tableId);
    assert.equal(first.yourSeat, 0);
    assert.equal(second.yourSeat, 1);
    assert.deepEqual(
      second.seats.map((s) => s.chips),
      [Game.initialChips, Game.initialChips]
    );
    const session1 = await p1.sessionInfo(),
      session2 = await p2.sessionInfo();
    assert.notEqual(session1.sessionId, session2.sessionId);
    const start1 = p1.messages.length,
      start2 = p2.messages.length;
    await p1.connector.send(new StartHandRequest()).submit();
    const [v1, v2, h1, h2] = await Promise.all([
      p1.wait<TableView>(PacketNames.tableView, start1, (v) => v.inHand),
      p2.wait<TableView>(PacketNames.tableView, start2, (v) => v.inHand),
      p1.wait<HandDealt>(PacketNames.handDealt, start1),
      p2.wait<HandDealt>(PacketNames.handDealt, start2)
    ]);
    assert.equal(v1.turnSeat, 0);
    assert.equal(v2.turnSeat, 0);
    assert.equal(h1.handNumber, v1.handNumber);
    assert.equal(h2.handNumber, v2.handNumber);
    assert.equal(h1.cards.length, Game.cardsPerPlayer);
    assert.equal(h2.cards.length, Game.cardsPerPlayer);
    assert.equal(new Set([...h1.cards, ...h2.cards]).size, 4);
    for (const card of [...h1.cards, ...h2.cards])
      assert(Number.isInteger(card) && card >= 0 && card < Game.deckSize);
    if (name === 'table') {
      const index = p2.messages.length;
      await p2.connector.send(new BetRequest(v2.handNumber, 50)).submit();
      assert.equal(
        (await p2.wait<ActionRejected>(PacketNames.actionRejected, index)).reason,
        Reasons.turn
      );
      assert.deepEqual(await p2.view(), v2);
    }
    const amount = name === 'table' ? 100 : 10;
    const bet1 = p1.messages.length,
      bet2 = p2.messages.length;
    await p1.connector.send(new BetRequest(v1.handNumber, amount)).submit();
    const [b1, b2] = await Promise.all([
      p1.wait<TableView>(PacketNames.tableView, bet1, (v) => v.turnSeat === 1),
      p2.wait<TableView>(PacketNames.tableView, bet2, (v) => v.turnSeat === 1)
    ]);
    if (name === 'reconnect') {
      await p2.close();
      p2 = create('p2-reconnected');
      const mark = p2.messages.length;
      const restored = await p2.join(ids[1], tableId);
      identity(b2, restored);
      assert.equal(restored.seats[1].chips, Game.initialChips);
      const rebound = await p2.sessionInfo();
      assert.notEqual(rebound.sessionId, session2.sessionId);
      assert.equal(rebound.actorId, session2.actorId);
      assert.equal(rebound.objectGeneration, session2.objectGeneration);
      assert.deepEqual((await p2.wait<HandDealt>(PacketNames.handDealt, mark)).cards, h2.cards);
    } else if (name === 'update') {
      await control.drain(b1.nodeId);
      const mark1 = p1.messages.length,
        mark2 = p2.messages.length;
      const [after1, after2] = await Promise.all([p1.view(), p2.view()]);
      identity(b1, after1);
      identity(b2, after2);
      assert.notEqual(after1.nodeId, b1.nodeId);
      assert.equal(after2.nodeId, after1.nodeId);
      assert.deepEqual((await p1.wait<HandDealt>(PacketNames.handDealt, mark1)).cards, h1.cards);
      assert.deepEqual((await p2.wait<HandDealt>(PacketNames.handDealt, mark2)).cards, h2.cards);
      assert.deepEqual(await p1.sessionInfo(), session1);
      assert.deepEqual(await p2.sessionInfo(), session2);
    }
    privacy(p1, h1.cards);
    for (const client of clients) if (client !== p1) privacy(client, h2.cards);
    const end1 = p1.messages.length,
      end2 = p2.messages.length;
    await p2.connector.send(new BetRequest(v2.handNumber, 50)).submit();
    const results = await Promise.all([
      p1.wait<HandResult>(PacketNames.handResult, end1),
      p2.wait<HandResult>(PacketNames.handResult, end2)
    ]);
    assert.deepEqual(results[0], results[1]);
    settlement(results[0], h1, h2, [amount, 50]);
    for (const client of clients) {
      const expectedDeals = (name === 'table' && client === p2) || name === 'update' ? 2 : 1;
      assert.equal(
        client.messages.filter((m) => m.name === PacketNames.handDealt).length,
        expectedDeals,
        'HandDealt only at hand start or explicit view/reconnect response'
      );
    }
    assert.equal(p1.disconnects, 0);
    assert.equal(p2.disconnects, 0);
    if (name === 'update') {
      await control.replace(b1.nodeId);
      await scenario('table', endpoint, `${runId}-replacement`, logDir, control);
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
