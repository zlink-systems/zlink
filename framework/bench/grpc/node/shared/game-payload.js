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
    // The Framework int64 schema decodes decimal JSON strings to bigint.
    this.gold = 987654321n;
    this.heading = 271;
    this.hp = 287n;
    this.items = [];
    this.level = 42n;
    this.mp = 164n;
    this.online = true;
    this.phase = BigInt(phase);
    this.playerId = 'player-0000000000000001';
    this.roomId = 'room-000000000000000001';
    this.run = BigInt(run);
    this.sentNs = BigInt(sentNs);
    this.seq = BigInt(sequence);
    this.session = 'session-0000000000000001';
    this.x = -123456;
    this.y = 7890;
    this.z = 654321;
  }
}

// Both bench rows register and use this one Framework schema contract.
const itemProperties = {
  count: { type: 'int32' },
  id: { type: 'string' },
  slot: { type: 'int32' }
};
const itemSchema = {
  type: 'object',
  properties: itemProperties,
  required: Object.keys(itemProperties)
};
const gameProperties = {
  displayName: { type: 'string' },
  gold: { type: 'int64' },
  heading: { type: 'int32' },
  hp: { type: 'int64' },
  items: { type: 'array', items: itemSchema },
  level: { type: 'int64' },
  mp: { type: 'int64' },
  online: { type: 'boolean' },
  phase: { type: 'int64' },
  playerId: { type: 'string' },
  roomId: { type: 'string' },
  run: { type: 'int64' },
  sentNs: { type: 'int64' },
  seq: { type: 'int64' },
  session: { type: 'string' },
  x: { type: 'int32' },
  y: { type: 'int32' },
  z: { type: 'int32' }
};
const gameSchema = {
  type: 'object',
  properties: gameProperties,
  required: Object.keys(gameProperties)
};
const { ZLinkPacket } = require('@zlink-systems/framework');
const { readZLinkPacketJsonContract } = require('@zlink-systems/stream-wire/internal/json');
ZLinkPacket('BenchGameMessage', { payload: gameSchema, reply: gameSchema })(GameMessage);
const gameJsonContract = readZLinkPacketJsonContract('BenchGameMessage');

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
    value.run !== BigInt(run) ||
    value.phase !== BigInt(phase) ||
    value.seq !== BigInt(sequence) ||
    !/^\d+$/.test(value.sentNs) ||
    value.items?.length !== expectedItems
  ) {
    throw new Error('JSON game message returned an invalid benchmark stamp');
  }
}

module.exports = {
  gameJsonContract,
  FIXTURE_PATH,
  fixture,
  GameItem,
  GameMessage,
  itemCountForSize,
  createGameMessage,
  assertGameMessageStamp
};
