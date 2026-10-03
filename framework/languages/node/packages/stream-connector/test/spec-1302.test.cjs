const assert = require('node:assert/strict');
const test = require('node:test');
const api = require('../dist');
const { ZlinkStreamFrameSender } = require('../dist/Runtime/ZlinkStreamFrameSender');

for (const terminal of ['timeout', 'cancel']) {
  test(`G01 ${terminal} removes a frame whose write has not started`, async () => {
    let release;
    const written = [];
    const connection = {
      write: (frame) => {
        written.push(frame);
        return new Promise((resolve) => {
          release = resolve;
        });
      }
    };
    const sender = new ZlinkStreamFrameSender({ encode: (_kind, name) => name });
    sender.open(connection, () => assert.fail('unexpected write failure'));
    const send = (name, signal, expiry) =>
      sender.send(
        connection,
        1,
        name,
        {},
        {},
        false,
        undefined,
        signal,
        undefined,
        undefined,
        expiry
      );
    const first = send('first');
    const controller = new AbortController();
    let expire;
    const expiry = new Promise((_resolve, reject) => {
      expire = reject;
    });
    const second = send('second', controller.signal, expiry);
    const failed = assert.rejects(second);
    if (terminal === 'cancel') controller.abort();
    else expire(new Error('request timed out'));
    await failed;
    release();
    await first;
    await Promise.resolve();
    assert.deepEqual(written, ['first']);
  });
}

test('R06 every public named surface rejects whitespace', async () => {
  const connector = api.zlinkStreamConnectorFactory.create({ endpoint: 'ws://node-r3.invalid' });
  for (const name of ['', ' ', '\t\r\n', '\u3000']) {
    const invalid = (error) => error.error?.code === api.ZlinkStreamErrorCode.ValidationFailed;
    assert.throws(() => connector.on(name, () => {}), invalid);
    assert.throws(() => connector.waitFor(name), invalid);
    assert.throws(
      () =>
        connector
          .send({ codec: api.ZlinkStreamCodec.Raw, payload: new Uint8Array() })
          .packetName(name),
      invalid
    );
    assert.throws(
      () =>
        connector
          .request({ codec: api.ZlinkStreamCodec.Raw, payload: new Uint8Array() })
          .packetName(name),
      invalid
    );
  }
  await connector.close();
});

for (const dispatchMode of [
  api.ZlinkStreamDispatchMode.Manual,
  api.ZlinkStreamDispatchMode.Immediate
]) {
  test(`R12 close reports a closed state with transport failure (${dispatchMode})`, async () => {
    const cause = new Error('close failed');
    const connector = api.zlinkStreamConnectorFactory.create({
      endpoint: 'ws://node-r3.invalid',
      heartbeat: { enabled: false },
      reconnect: { enabled: false },
      dispatchMode,
      transportFactory: {
        connect: async () => ({
          write: async () => {},
          read: () => new Promise(() => {}),
          close: async () => {
            throw cause;
          }
        })
      }
    });
    const errors = [];
    connector.onErrorReceived((error) =>
      errors.push({ error, state: connector.state, reason: connector.closeReason })
    );
    await connector.connect();
    await connector.close();
    await connector.dispatch();
    assert.equal(connector.closeReason, 'ClientClose');
    assert.equal(connector.state, api.ZlinkStreamConnectionState.Closed);
    assert.equal(errors.length, 1);
    assert.equal(errors[0].error.code, api.ZlinkStreamErrorCode.Disconnected);
    assert.equal(errors[0].error.cause, cause);
    assert.equal(errors[0].state, api.ZlinkStreamConnectionState.Closed);
    assert.equal(errors[0].reason, 'ClientClose');
  });
}

test('assert helpers propagate unclassified action failures unchanged', async () => {
  for (const cause of [
    new Error('action failed'),
    new TypeError('action failed'),
    'failure',
    undefined
  ]) {
    for (const helper of ['expectFailure', 'expectTimeout']) {
      await assert.rejects(
        api.zlinkStreamAssert[helper](async () => {
          throw cause;
        }),
        (error) => error === cause
      );
    }
  }
});

test('R12 transport failure retains its close reason when transport close also fails', async () => {
  let rejectRead;
  let notifyRead;
  const reading = new Promise((resolve) => {
    notifyRead = resolve;
  });
  const closeFailure = new Error('close failed');
  const connector = api.zlinkStreamConnectorFactory.create({
    endpoint: 'ws://node-r3.invalid',
    heartbeat: { enabled: false },
    reconnect: { enabled: false },
    dispatchMode: api.ZlinkStreamDispatchMode.Immediate,
    transportFactory: {
      connect: async () => ({
        write: async () => {},
        read: () =>
          new Promise((_resolve, reject) => {
            rejectRead = reject;
            notifyRead();
          }),
        close: async () => {
          throw closeFailure;
        }
      })
    }
  });
  const errors = [];
  const closeObservations = [];
  connector.onErrorReceived((error) => {
    errors.push(error);
    if (error.cause === closeFailure)
      closeObservations.push({ state: connector.state, reason: connector.closeReason });
  });
  const disconnected = new Promise((resolve) => connector.onDisconnected(resolve));
  await connector.connect();
  await reading;
  rejectRead(new Error('read failed'));
  await disconnected;
  assert.equal(connector.closeReason, 'TransportError');
  assert.equal(connector.state, api.ZlinkStreamConnectionState.Disconnected);
  assert.equal(errors.filter((error) => error.cause === closeFailure).length, 1);
  assert.equal(
    errors.find((error) => error.cause === closeFailure).code,
    api.ZlinkStreamErrorCode.Disconnected
  );
  assert.deepEqual(closeObservations, [
    { state: api.ZlinkStreamConnectionState.Disconnected, reason: 'TransportError' }
  ]);
  await connector.close();
});

test('R12 synchronous transport close claims teardown before a state handler reconnects', async () => {
  let rejectRead;
  let readStarted;
  const reading = new Promise((resolve) => {
    readStarted = resolve;
  });
  let connections = 0;
  let reconnect;
  const order = [];
  const connector = api.zlinkStreamConnectorFactory.create({
    endpoint: 'ws://node-r3.invalid',
    heartbeat: { enabled: false },
    reconnect: { enabled: false },
    dispatchMode: api.ZlinkStreamDispatchMode.Immediate,
    transportFactory: {
      connect: async () => {
        connections += 1;
        if (connections > 1) order.push('connect');
        return {
          write: async () => {},
          read: () =>
            connections === 1
              ? new Promise((_resolve, reject) => {
                  rejectRead = reject;
                  readStarted();
                })
              : new Promise(() => {}),
          close: () => {
            throw new Error('synchronous close failure');
          }
        };
      }
    }
  });
  connector.onConnectionStateChanged((change) => {
    if (change.current === api.ZlinkStreamConnectionState.Disconnected && reconnect === undefined)
      reconnect = connector.connect();
  });
  const disconnected = new Promise((resolve) =>
    connector.onDisconnected(() => {
      order.push('disconnected');
      resolve();
    })
  );
  await connector.connect();
  await reading;
  rejectRead(new Error('read failed'));
  await disconnected;
  await reconnect;
  assert.deepEqual(order, ['disconnected', 'connect']);
  await connector.close();
});
