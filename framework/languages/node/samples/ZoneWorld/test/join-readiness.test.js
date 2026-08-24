const assert = require('node:assert/strict');
const Module = require('node:module');
const path = require('node:path');
const test = require('node:test');

const sampleRoot = path.resolve(__dirname, '..');

class FakeWait {
  constructor(connector, packetName) {
    this.connector = connector;
    this.packetName = packetName;
    this.predicate = () => true;
  }

  where(predicate) {
    this.predicate = predicate;
    return this;
  }

  timeout() {
    return this;
  }

  submit() {
    return new Promise((resolve) => {
      this.connector.waiters.push({
        packetName: this.packetName,
        predicate: this.predicate,
        resolve
      });
    });
  }
}

class SettlingRejoinConnector {
  constructor() {
    this.waiters = [];
  }

  waitFor(packetName) {
    return new FakeWait(this, packetName);
  }

  send() {
    return {
      packetName() { return this; },
      submit: async () => {
        this.deliver('JoinWorldRes', {
          playerId: 'player-b8', zoneId: 'zone-sw', x: 25, y: 55, error: null
        });
        this.deliver('ZoneStateNotify', {
          zoneId: 'zone-nw', tick: 41,
          players: [{ playerId: 'player-b8', zoneId: 'zone-nw', x: 25, y: 45, isBot: false }]
        });
        setImmediate(() => this.deliver('ZoneStateNotify', {
          zoneId: 'zone-sw', tick: 42,
          players: [{ playerId: 'player-b8', zoneId: 'zone-sw', x: 25, y: 55, isBot: false }]
        }));
      }
    };
  }

  deliver(packetName, payload) {
    const message = { payload };
    const waiter = this.waiters.find((candidate) =>
      candidate.packetName === packetName && candidate.predicate(message));
    if (waiter === undefined) return;
    this.waiters.splice(this.waiters.indexOf(waiter), 1);
    waiter.resolve(message);
  }
}

test('B8 rejoin ignores a settling source state until the committed target state is published', async () => {
  const originalLoad = Module._load;
  Module._load = function load(request, parent, isMain) {
    if (request === '@zlink-systems/stream-connector') {
      return {
        zlinkStreamAssert: {
          ensure(condition, message) {
            if (!condition) throw new Error(message);
          }
        }
      };
    }
    return originalLoad.call(this, request, parent, isMain);
  };
  let joinAndWaitForOwnedState;
  try {
    ({ joinAndWaitForOwnedState } = require(path.join(
      sampleRoot,
      'dist/Client/join-readiness.js'
    )));
  } finally {
    Module._load = originalLoad;
  }
  const connector = new SettlingRejoinConnector();

  const joined = await joinAndWaitForOwnedState(connector, 'player-b8', 'zone-sw');

  assert.equal(joined.zoneId, 'zone-sw');
  assert.equal(joined.x, 25);
  assert.equal(joined.y, 55);
});
