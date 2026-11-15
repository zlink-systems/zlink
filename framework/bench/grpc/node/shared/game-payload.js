// SPDX-License-Identifier: MPL-2.0
'use strict';

const fs = require('node:fs');
const path = require('node:path');

const FIXTURE_PATH = path.join(__dirname, '..', '..', 'ladder-mixed-payload.json');
const fixture = JSON.parse(fs.readFileSync(FIXTURE_PATH, 'utf8'));
const ITEM_COUNTS = fixture.itemCounts;

class GameItem {
  constructor(count, id, slot) {
    this.count = count;
    this.id = id;
    this.slot = slot;
  }
}

class GameMessage {
  constructor(run = '123', phase = '1', sequence = '456', sentNs = '1700000000000000000') {
    this.displayName = 'Player_12345678';
    // Node has no int64 JSON type; keep int64-semantic fields as decimal strings.
    this.gold = '987654321';
    this.heading = 271;
    this.hp = '287';
    this.items = [];
    this.level = '42';
    this.mp = '164';
    this.online = true;
    this.phase = String(phase);
    this.playerId = 'player-0000000000000001';
    this.roomId = 'room-000000000000000001';
    this.run = String(run);
    this.sentNs = String(sentNs);
    this.seq = String(sequence);
    this.session = 'session-0000000000000001';
    this.x = -123456;
    this.y = 7890;
    this.z = 654321;
  }
}

function itemCountForSize(payloadSize, implementation) {
  const count = ITEM_COUNTS[String(payloadSize)];
  if (count === undefined) {
    throw new RangeError(
      `${implementation} supports payload sizes 1024 and 4096; got ${payloadSize}`
    );
  }
  return count;
}

function createGameMessage(
  payloadSize,
  run = '123',
  phase = '1',
  sequence = '456',
  sentNs = '1700000000000000000',
  implementation = 'Node JSON'
) {
  const count = itemCountForSize(payloadSize, implementation);
  const value = new GameMessage(run, phase, sequence, sentNs);
  for (let index = 0; index < count; index++) {
    const oneBased = index + 1;
    value.items.push(
      new GameItem((index % 9) + 1, `item-${String(oneBased).padStart(8, '0')}`, index % 32)
    );
  }
  return value;
}

function assertGameMessageStamp(value, run, phase, sequence, payloadSize, implementation) {
  const expectedItems = itemCountForSize(payloadSize, implementation);
  if (
    value === null ||
    typeof value !== 'object' ||
    value.run !== String(run) ||
    value.phase !== String(phase) ||
    value.seq !== String(sequence) ||
    !/^\d+$/.test(value.sentNs) ||
    value.items?.length !== expectedItems
  ) {
    throw new Error('JSON game message returned an invalid benchmark stamp');
  }
}

module.exports = {
  FIXTURE_PATH,
  fixture,
  GameItem,
  GameMessage,
  itemCountForSize,
  createGameMessage,
  assertGameMessageStamp
};
