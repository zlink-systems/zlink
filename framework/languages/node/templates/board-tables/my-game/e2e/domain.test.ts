import { test } from 'node:test';
import assert from 'node:assert/strict';
import { TableState } from '../server/table/domain/table-state';
import { Game, Reasons } from '../server/shared/constants';
import { HandResult } from '../server/shared/messages';
test('Rejections preserve state and repeated joins preserve seats', () => {
  const table = new TableState();
  table.admit('p1');
  const before = structuredClone(table.snapshot);
  assert.equal(table.start('p1'), Reasons.players);
  assert.deepEqual(table.snapshot, before);
  table.admit('p1');
  assert.deepEqual(table.snapshot, before);
  for (const id of ['p2', 'p3', 'p4']) table.admit(id);
  const seated = structuredClone(table.snapshot.seats);
  assert.equal(table.admit('p5'), false);
  assert.equal(table.admit('p1'), true);
  assert.deepEqual(table.snapshot.seats, seated);
  assert.equal(table.start('p1'), undefined);
  for (const [player, hand, amount, expected] of [
    ['unknown', 1, 0, Reasons.seated],
    ['p2', 1, 0, Reasons.turn],
    ['p1', 2, 0, Reasons.noHand],
    ['p1', 1, -1, Reasons.amount],
    ['p1', 1, 1001, Reasons.amount],
    ['p1', 1, 0.5, Reasons.amount]
  ] as const) {
    const snapshot = structuredClone(table.snapshot);
    assert.equal(table.bet(player, hand, amount), expected);
    assert.deepEqual(table.snapshot, snapshot);
  }
  const playing = structuredClone(table.snapshot);
  assert.equal(table.start('p1'), Reasons.playing);
  assert.deepEqual(table.snapshot, playing);
  assert.equal(
    new Set([...table.snapshot.deck, ...table.snapshot.seats.flatMap((s) => s.cards)]).size,
    Game.deckSize
  );
});
test('Tie splits the remainder to the lowest winning seat; snapshot restores a live hand', () => {
  const source = new TableState();
  for (const player of ['p1', 'p2', 'p3']) source.admit(player);
  source.start('p1');
  source.snapshot.seats[0].cards = [0, 1];
  source.snapshot.seats[1].cards = [13, 14];
  source.snapshot.seats[2].cards = [26, 39];
  const restored = new TableState();
  restored.snapshot = JSON.parse(JSON.stringify(source.snapshot)) as typeof source.snapshot;
  assert.equal(restored.bet('p1', 1, 1), undefined);
  assert.equal(restored.bet('p2', 1, 1), undefined);
  const result = restored.bet('p3', 1, 1);
  assert(result instanceof HandResult);
  assert.deepEqual(result.winners, [0, 1]);
  assert.deepEqual(
    result.seats.map((s) => s.chips),
    [1001, 1000, 999]
  );
  assert.equal(
    result.seats.reduce((sum, s) => sum + s.chips, 0),
    3000
  );
  assert.equal(restored.snapshot.turnSeat, Game.noSeat);
  assert.equal(restored.snapshot.pot, 0);
  assert.equal(restored.bet('p1', 1, 0), Reasons.noHand);
});
