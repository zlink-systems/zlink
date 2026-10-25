import assert from 'node:assert/strict';
import { after, test } from 'node:test';
import { TableState } from '../server/table/domain/table-state';
import {
  deserializeTableSnapshot,
  serializeTableSnapshot
} from '../server/table/infrastructure/table-snapshot-json';
import { Game, Reasons } from '../server/shared/constants';
import {
  HandResult,
  MaxContractInteger,
  isContractInteger,
  tryAddContractIntegers
} from '../server/shared/messages';

let completedChecks = 0;

function completeCheck(): void {
  completedChecks++;
}

after(() => {
  if (completedChecks === 7) process.stdout.write('Domain checks: 7/7\n');
});

test('a: shuffled deck contains every card exactly once and admission remains stable', () => {
  const table = new TableState();
  table.admit('p1');
  const onePlayer = structuredClone(table.snapshot);
  assert.equal(table.start('p1'), Reasons.players);
  assert.deepEqual(table.snapshot, onePlayer);
  assert.equal(table.admit('p1'), true);
  assert.deepEqual(table.snapshot, onePlayer);

  for (const id of ['p2', 'p3', 'p4']) table.admit(id);
  const seated = structuredClone(table.snapshot.seats);
  assert.equal(table.admit('p5'), false);
  assert.equal(table.admit('p1'), true);
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
  table.admit('p1');
  table.admit('p2');
  assert.equal(table.start('p1'), undefined);
  assert.deepEqual(
    bounds,
    Array.from({ length: Game.deckSize - 1 }, (_, index) => Game.deckSize - index)
  );
  assert.deepEqual(table.snapshot.seats.map((seat) => seat.cards), [
    [50, 51],
    [48, 49]
  ]);
  assert.deepEqual(table.snapshot.deck, Array.from({ length: 48 }, (_, card) => card));
  completeCheck();
});

test('c: relocation restores a live hand and settlement clears every private hand', () => {
  const source = new TableState((exclusiveMax) => exclusiveMax - 1);
  for (const player of ['p1', 'p2', 'p3']) source.admit(player);
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
  assert.deepEqual(restored.snapshot.seats.map((seat) => seat.cards), [[], [], []]);
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
  table.admit('p1');
  table.admit('p2');
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
  table.admit('p1');
  table.admit('p2');
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
  table.admit('p1');
  table.admit('p2');
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
  table.admit('p1');
  table.admit('p2');
  table.snapshot.handNumber = MaxContractInteger;
  const before = structuredClone(table.snapshot);
  assert.equal(table.start('p1'), Reasons.integerOutOfRange);
  assert.deepEqual(table.snapshot, before);
  completeCheck();
});
