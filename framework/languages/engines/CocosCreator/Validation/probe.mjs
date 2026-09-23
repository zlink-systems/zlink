import assert from 'node:assert/strict';
import { EngineLobbyClient } from './dist/EngineLobbyClient.js';

const endpoint = process.env.ENGINE_LOBBY_ENDPOINT;
if (!endpoint) throw new Error('ENGINE_LOBBY_ENDPOINT is required.');
assert.throws(
  () => new EngineLobbyClient('tcp://127.0.0.1:22700', () => {}, () => {}),
  error => error.error?.code === 'configurationError'
);

const receivedAlice = [];
const receivedBob = [];
const alice = new EngineLobbyClient(endpoint, () => {}, value => receivedAlice.push(value));
const bob = new EngineLobbyClient(endpoint, () => {}, value => receivedBob.push(value));
const pump = setInterval(() => {
  void alice.dispatch();
  void bob.dispatch();
}, 10);

try {
  const joinedAlice = await alice.start('alice');
  const joinedBob = await bob.start('bob');
  assert.notEqual(joinedAlice.actorId, joinedBob.actorId);
  await alice.sendChat('hello');
  await new Promise((resolve, reject) => {
    const deadline = setTimeout(() => reject(new Error('ChatNotify did not arrive.')), 5000);
    const check = setInterval(() => {
      if (receivedAlice.length >= 1 && receivedBob.length >= 1) {
        clearInterval(check);
        clearTimeout(deadline);
        resolve();
      }
    }, 10);
  });
  for (const received of [receivedAlice, receivedBob]) {
    assert.deepEqual(received[0], {
      actorId: joinedAlice.actorId,
      name: 'alice',
      text: 'hello'
    });
  }
  console.log('cocos-engine-lobby-probe=ok');
} finally {
  clearInterval(pump);
  await Promise.all([alice.close(), bob.close()]);
}
