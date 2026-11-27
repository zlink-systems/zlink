import assert from 'node:assert/strict';
import { after, test } from 'node:test';
import { performance } from 'node:perf_hooks';
import { Pool } from '../server/matchmaker/pool';
import type { Ticket } from '../server/matchmaker/domain/queue';
import { RoomProvisioner } from '../server/table/infrastructure/room-provisioner';
import { TableState } from '../server/table/domain/table-state';
import {
  deserializeTableSnapshot,
  serializeTableSnapshot
} from '../server/table/infrastructure/table-snapshot-json';
import { Game, Matchmaking, Names, Reasons } from '../server/shared/constants';
import {
  HandResult,
  ReturnReport,
  type RoomClosed,
  WithdrawRoom,
  MaxContractInteger,
  isContractInteger,
  tryAddContractIntegers
} from '../server/shared/messages';

let completedChecks = 0;

function completeCheck(): void {
  completedChecks++;
}

after(() => {
  if (completedChecks === 9) process.stdout.write('Domain checks: 9/9\n');
});

test('a: shuffled deck contains every card exactly once and admission remains stable', () => {
  const table = new TableState();
  table.admit('p1', Game.initialChips);
  const onePlayer = structuredClone(table.snapshot);
  assert.equal(table.start('p1'), Reasons.players);
  assert.deepEqual(table.snapshot, onePlayer);
  assert.equal(table.admit('p1', Game.initialChips), true);
  assert.deepEqual(table.snapshot, onePlayer);

  for (const id of ['p2', 'p3', 'p4']) table.admit(id, Game.initialChips);
  const seated = structuredClone(table.snapshot.seats);
  assert.equal(table.admit('p5', Game.initialChips), false);
  assert.equal(table.admit('p1', Game.initialChips), true);
  assert.deepEqual(table.snapshot.seats, seated);
  assert.equal(table.start('p1'), undefined);

  const allCards = [...table.snapshot.deck, ...table.snapshot.seats.flatMap((seat) => seat.cards)];
  assert.deepEqual(
    allCards.sort((left, right) => left - right),
    Array.from({ length: Game.deckSize }, (_, card) => card)
  );
  const playing = structuredClone(table.snapshot);
  assert.equal(table.start('p1'), Reasons.playing);
  assert.deepEqual(table.snapshot, playing);
  completeCheck();
});

test('b: the constructor test RNG produces a deterministic deal order', () => {
  const bounds: number[] = [];
  const table = new TableState((exclusiveMax) => {
    bounds.push(exclusiveMax);
    return exclusiveMax - 1;
  });
  table.admit('p1', Game.initialChips);
  table.admit('p2', Game.initialChips);
  assert.equal(table.start('p1'), undefined);
  assert.deepEqual(
    bounds,
    Array.from({ length: Game.deckSize - 1 }, (_, index) => Game.deckSize - index)
  );
  assert.deepEqual(
    table.snapshot.seats.map((seat) => seat.cards),
    [
      [50, 51],
      [48, 49]
    ]
  );
  assert.deepEqual(
    table.snapshot.deck,
    Array.from({ length: 48 }, (_, card) => card)
  );
  completeCheck();
});

test('c: relocation restores a live hand and settlement clears every private hand', () => {
  const source = new TableState((exclusiveMax) => exclusiveMax - 1);
  for (const player of ['p1', 'p2', 'p3']) source.admit(player, Game.initialChips);
  assert.equal(source.start('p1'), undefined);
  source.snapshot.seats[0].cards = [0, 1];
  source.snapshot.seats[1].cards = [13, 14];
  source.snapshot.seats[2].cards = [26, 39];

  const restored = new TableState();
  restored.snapshot = deserializeTableSnapshot(serializeTableSnapshot(source.snapshot));

  assert.equal(restored.bet('p1', 1n, 1n), undefined);
  assert.equal(restored.bet('p2', 1n, 1n), undefined);
  const result = restored.bet('p3', 1n, 1n);
  assert(result instanceof HandResult);
  assert.deepEqual(result.winners, [0, 1]);
  assert.deepEqual(
    result.seats.map((seat) => seat.chips),
    [1001n, 1000n, 999n]
  );
  assert.equal(
    result.seats.reduce((sum, seat) => sum + seat.chips, 0n),
    3000n
  );
  assert(result.seats.every((seat) => seat.cardCount === 0));
  assert.deepEqual(
    restored.snapshot.seats.map((seat) => seat.cards),
    [[], [], []]
  );
  assert.deepEqual(restored.cards('p1'), []);
  assert.equal(restored.snapshot.turnSeat, Game.noSeat);
  assert.equal(restored.snapshot.pot, 0n);
  assert.equal(restored.bet('p1', 1n, 0n), Reasons.noHand);
  completeCheck();
});

test('d: the maximum bet is accepted and a larger bet rejects without mutation', () => {
  assert(isContractInteger(0n));
  assert(isContractInteger(MaxContractInteger));
  assert.equal(isContractInteger(-1n), false);
  assert.equal(isContractInteger(MaxContractInteger + 1n), false);
  assert.equal(tryAddContractIntegers(MaxContractInteger - 1n, 1n), MaxContractInteger);
  assert.equal(tryAddContractIntegers(MaxContractInteger, 1n), undefined);

  const table = new TableState((exclusiveMax) => exclusiveMax - 1);
  table.admit('p1', Game.initialChips);
  table.admit('p2', Game.initialChips);
  assert.equal(table.start('p1'), undefined);
  table.snapshot.seats[0].chips = MaxContractInteger;
  assert.equal(table.bet('p1', 1n, MaxContractInteger), undefined);
  assert.equal(table.snapshot.seats[0].chips, 0n);
  assert.equal(table.snapshot.pot, MaxContractInteger);
  assert.equal(table.snapshot.turnSeat, 1);

  for (const [player, hand, amount, expected] of [
    ['unknown', 1n, 0n, Reasons.seated],
    ['p1', 1n, 0n, Reasons.turn],
    ['p2', 2n, 0n, Reasons.noHand],
    ['p2', 1n, -1n, Reasons.amount],
    ['p2', 1n, Game.initialChips + 1n, Reasons.amount],
    ['p2', 1n, MaxContractInteger + 1n, Reasons.integerOutOfRange],
    ['p2', MaxContractInteger + 1n, 0n, Reasons.integerOutOfRange]
  ] as const) {
    const before = structuredClone(table.snapshot);
    assert.equal(table.bet(player, hand, amount), expected);
    assert.deepEqual(table.snapshot, before);
  }

  const inHand = structuredClone(table.snapshot);
  assert.equal(table.start('p1'), Reasons.playing);
  assert.deepEqual(table.snapshot, inHand);
  completeCheck();
});

test('e: a pot sum overflow rejects without mutation', () => {
  const table = new TableState((exclusiveMax) => exclusiveMax - 1);
  table.admit('p1', Game.initialChips);
  table.admit('p2', Game.initialChips);
  assert.equal(table.start('p1'), undefined);
  table.snapshot.seats[0].chips = 1n;
  table.snapshot.pot = MaxContractInteger;
  const before = structuredClone(table.snapshot);
  assert.equal(table.bet('p1', 1n, 1n), Reasons.integerOutOfRange);
  assert.deepEqual(table.snapshot, before);
  completeCheck();
});

test('f: a payout overflow rejects without mutation', () => {
  const table = new TableState((exclusiveMax) => exclusiveMax - 1);
  table.admit('p1', Game.initialChips);
  table.admit('p2', Game.initialChips);
  assert.equal(table.start('p1'), undefined);
  table.snapshot.seats[0].chips = MaxContractInteger;
  table.snapshot.seats[0].cards = [12, 25];
  table.snapshot.seats[1].cards = [0, 1];
  assert.equal(table.bet('p1', 1n, 1n), undefined);
  const before = structuredClone(table.snapshot);
  assert.equal(table.bet('p2', 1n, 1n), Reasons.integerOutOfRange);
  assert.deepEqual(table.snapshot, before);
  completeCheck();
});

test('g: starting at the maximum hand number rejects without mutation', () => {
  const table = new TableState();
  table.admit('p1', Game.initialChips);
  table.admit('p2', Game.initialChips);
  table.snapshot.handNumber = MaxContractInteger;
  const before = structuredClone(table.snapshot);
  assert.equal(table.start('p1'), Reasons.integerOutOfRange);
  assert.deepEqual(table.snapshot, before);
  completeCheck();
});

for (const missingReport of [true, false]) {
  test(
    missingReport
      ? 'h: withdrawal expiry releases both tickets when a return report is missing'
      : 'i: withdrawal expiry retries an occupied room close and releases both tickets',
    async (t) => {
      let now = Matchmaking.entryMs;
      t.mock.method(performance, 'now', () => now);
      const closeRequests: WithdrawRoom[] = [];
      const outbound = {
        sendToSpot: (id: string, message: object) => ({
          instanceSpot: () => ({
            inMesh: () => ({
              submit: async () => {
                if (id === Names.roomProvisionerKey && message instanceof WithdrawRoom)
                  closeRequests.push(message);
              }
            })
          }),
          submit: async () => {}
        })
      };
      const pool = new Pool({} as ConstructorParameters<typeof Pool>[0]);
      Object.defineProperty(pool, 'context', { value: { outbound } });
      const members: [Ticket, Ticket] = [
        { playerId: 'p1', ticketId: 't1', requestKey: 'r1', waitingSince: 0, assignmentId: 'old' },
        { playerId: 'p2', ticketId: 't2', requestKey: 'r2', waitingSince: 0, assignmentId: 'old' }
      ];
      for (const ticket of members) pool.tickets.set(ticket.playerId, ticket);
      pool.assignments.set('old', {
        id: 'old',
        state: { phase: 'entering' },
        deadline: now,
        members,
        entered: members.map(({ playerId }) => playerId),
        returned: []
      });

      await pool.advance();
      await pool.returned(new ReturnReport('t1', 'old'));

      let closeAttempts = 0;
      const provisioner = new RoomProvisioner({
        find: async () => ({}),
        close: async () => ++closeAttempts > 1 || missingReport
      } as unknown as ConstructorParameters<typeof RoomProvisioner>[0]);
      Object.defineProperty(provisioner, 'context', {
        value: {
          outbound: {
            sendToSpot: (_id: string, message: RoomClosed) => ({
              instanceSpot: () => ({
                inMesh: () => ({ submit: async () => pool.roomClosed(message) })
              })
            })
          }
        }
      });
      if (!missingReport) {
        await pool.returned(new ReturnReport('t2', 'old'));
        assert.deepEqual(
          closeRequests.map(({ assignmentId }) => assignmentId),
          ['old']
        );
        await assert.rejects(provisioner.close(closeRequests.shift()!.assignmentId), {
          message: 'Room still occupied: old'
        });
      }

      now += Matchmaking.entryMs - 1;
      await pool.advance();
      assert(pool.assignments.has('old'));
      assert(members.every((ticket) => ticket.assignmentId === 'old'));
      assert.equal(closeRequests.length, 0);

      now++;
      await pool.advance();
      assert(!pool.assignments.has('old'), 'the expired withdrawal released its assignment');
      const replacementId = members[0].assignmentId;
      assert(replacementId && replacementId !== 'old');
      assert.equal(members[1].assignmentId, replacementId);
      assert.deepEqual(pool.assignments.get(replacementId)?.members, members);
      assert.deepEqual(
        closeRequests.map(({ assignmentId }) => assignmentId),
        ['old']
      );
      await provisioner.close(closeRequests[0].assignmentId);
      assert.equal(closeAttempts, missingReport ? 1 : 2);
      assert.equal(pool.assignments.size, 1);
      assert(members.every((ticket) => ticket.assignmentId === replacementId));
      completeCheck();
    }
  );
}
