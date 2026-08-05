const assert = require('node:assert/strict');
const childProcess = require('node:child_process');
const crypto = require('node:crypto');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const protocolCodecs = require('./helpers/stream-protocol-codecs');

let browserEntry;

test.before(async () => {
  browserEntry = await import('../../packages/stream-connector/dist/browser/index.mjs');
});

test('package root rejects tcp and tls immediately', () => {
  for (const endpoint of ['tcp://127.0.0.1:19000', 'tls://127.0.0.1:19000']) {
    assert.throws(
      () => browserEntry.zlinkStreamConnectorFactory.create({ endpoint }),
      (error) => error.error?.code === browserEntry.ZlinkStreamErrorCode.ConfigurationError
    );
    assert.throws(
      () => new browserEntry.DefaultZlinkStreamConnector({ endpoint }),
      (error) => error.error?.code === browserEntry.ZlinkStreamErrorCode.ConfigurationError
    );
  }
});

test('package root dispatches every stream frame in one native WebSocket message', async () => {
  const originalWebSocket = globalThis.WebSocket;
  const originalCrypto = globalThis.crypto;
  globalThis.WebSocket = FakeWebSocket;
  globalThis.crypto = crypto.webcrypto;

  try {
    const instance = browserEntry.zlinkStreamConnectorFactory.create({
      endpoint: 'wss://browser.example.test/stream',
      heartbeat: { enabled: false }
    });
    const pushed = [];
    instance.on('BrowserPush', (message) => pushed.push(new TextDecoder().decode(message.payload.payload)));

    await instance.connect();
    assert.equal(FakeWebSocket.last.url, 'wss://browser.example.test/stream');
    assert.equal(FakeWebSocket.last.binaryType, 'arraybuffer');

    const pending = instance.request({
      codec: browserEntry.ZlinkStreamCodec.Raw,
      payload: new TextEncoder().encode('request')
    }).packetName('BrowserRequest').timeout(1000).submitEncoded();

    await instance.dispatch();
    assert.equal(new TextDecoder().decode((await pending).payload), 'reply');
    assert.deepEqual(pushed, ['progress', 'completed']);
    await instance.close();
    assert.equal(FakeWebSocket.last.closed, true);
  } finally {
    if (originalWebSocket === undefined) {
      delete globalThis.WebSocket;
    } else {
      globalThis.WebSocket = originalWebSocket;
    }
    if (originalCrypto === undefined) {
      delete globalThis.crypto;
    } else {
      globalThis.crypto = originalCrypto;
    }
  }
});

test('browser transport retains many unread payload-sized messages without disconnecting', async () => {
  const originalWebSocket = globalThis.WebSocket;
  const originalCrypto = globalThis.crypto;
  globalThis.WebSocket = FakeWebSocket;
  globalThis.crypto = crypto.webcrypto;

  try {
    const instance = browserEntry.zlinkStreamConnectorFactory.create({
      endpoint: 'wss://browser.example.test/stream',
      heartbeat: { enabled: false }
    });
    let received = 0;
    instance.on('Backlog', () => { received += 1; });
    await instance.connect();

    const frame = encodeFrame({
      kind: browserEntry.ZlinkStreamMessageKind.Send,
      codec: browserEntry.ZlinkStreamCodec.Raw,
      flags: browserEntry.ZlinkStreamHeaderFlags.None,
      name: 'Backlog'
    }, 'x'.repeat(2048));
    for (let index = 0; index < 40; index += 1) {
      FakeWebSocket.last.emit('message', { data: exactArrayBuffer(frame) });
    }

    assert.equal(FakeWebSocket.last.closed, false);
    for (let index = 0; index < 40; index += 1) {
      await instance.dispatch();
    }
    assert.equal(received, 40);
    await instance.close();
  } finally {
    if (originalWebSocket === undefined) delete globalThis.WebSocket;
    else globalThis.WebSocket = originalWebSocket;
    if (originalCrypto === undefined) delete globalThis.crypto;
    else globalThis.crypto = originalCrypto;
  }
});

test('browser close waits for the WebSocket close event', async () => {
  const originalWebSocket = globalThis.WebSocket;
  const originalCrypto = globalThis.crypto;
  globalThis.WebSocket = ControlledCloseWebSocket;
  globalThis.crypto = crypto.webcrypto;

  try {
    const instance = browserEntry.zlinkStreamConnectorFactory.create({
      endpoint: 'ws://browser.example.test/stream',
      heartbeat: { enabled: false }
    });
    await instance.connect();

    let resolved = false;
    const closing = instance.close().then(() => { resolved = true; });
    await new Promise((resolve) => setTimeout(resolve, 0));
    assert.equal(FakeWebSocket.last.closeRequested, true);
    assert.equal(resolved, false);

    FakeWebSocket.last.finishClose();
    await closing;
    assert.equal(resolved, true);
  } finally {
    if (originalWebSocket === undefined) delete globalThis.WebSocket;
    else globalThis.WebSocket = originalWebSocket;
    if (originalCrypto === undefined) delete globalThis.crypto;
    else globalThis.crypto = originalCrypto;
  }
});

test('explicit browser flow propagation does not leak into unrelated outbound work', async () => {
  const originalCrypto = globalThis.crypto;
  globalThis.crypto = crypto.webcrypto;
  const connection = new MemoryConnection();
  try {
    const instance = browserEntry.zlinkStreamConnectorFactory.create({
      endpoint: 'ws://browser.example.test/stream',
      transportFactory: { connect: async () => connection },
      heartbeat: { enabled: false }
    });
    await instance.connect();
    const inboundFlow = {
      flowId: '018f0f7c-7b4d-7abc-8def-0123456789ab',
      flowOrigin: 'Inbound'
    };
    instance.send({ codec: browserEntry.ZlinkStreamCodec.Raw, payload: new Uint8Array([1]) })
      .packetName('Related')
      .flowFrom(inboundFlow)
      .submit();
    instance.send({ codec: browserEntry.ZlinkStreamCodec.Raw, payload: new Uint8Array([2]) })
      .packetName('Unrelated')
      .submit();
    await new Promise((resolve) => setTimeout(resolve, 0));

    const headers = connection.frames.map((frame) => protocolCodecs.ZlinkStreamHeaderCodec.decode(
      protocolCodecs.ZlinkStreamFrameCodec.decode(frame).header
    ));
    assert.equal(headers[0].flowId, inboundFlow.flowId);
    assert.equal(headers[0].flowOrigin, inboundFlow.flowOrigin);
    assert.notEqual(headers[1].flowId, inboundFlow.flowId);
    assert.equal(headers[1].flowOrigin, 'Application');
    await instance.close();
  } finally {
    if (originalCrypto === undefined) delete globalThis.crypto;
    else globalThis.crypto = originalCrypto;
  }
});

test('package root creates a browser bundle without Node-only modules or Buffer', () => {
  const root = path.resolve(__dirname, '../..');
  const outputDirectory = fs.mkdtempSync(path.join(os.tmpdir(), 'zlink-browser-bundle-'));
  const output = path.join(outputDirectory, 'stream-connector-browser.js');
  const metadata = path.join(outputDirectory, 'metadata.json');
  try {
    childProcess.execFileSync(path.join(root, 'node_modules/.bin/esbuild'), [
      'packages/stream-connector/dist/browser/index.mjs',
      '--bundle',
      '--platform=browser',
      '--format=esm',
      `--outfile=${output}`,
      `--metafile=${metadata}`
    ], { cwd: root, stdio: 'pipe' });

    const graph = JSON.parse(fs.readFileSync(metadata, 'utf8'));
    const inputs = Object.keys(graph.inputs);
    const forbiddenModules = /(^|\/)(node:)?(net|tls|async_hooks|crypto)(\.[cm]?[jt]s)?$/;
    assert.equal(inputs.some((input) => forbiddenModules.test(input)), false, inputs.join('\n'));

    const bundle = fs.readFileSync(output, 'utf8');
    assert.doesNotMatch(bundle, /node:(?:net|tls|async_hooks|crypto)/);
    assert.doesNotMatch(bundle, /require\(["'](?:net|tls|async_hooks|crypto)["']\)/);
    assert.doesNotMatch(bundle, /\bBuffer\b/);
    const packageJson = JSON.parse(fs.readFileSync(path.join(root, 'packages/stream-connector/package.json'), 'utf8'));
    assert.equal(packageJson.exports['./browser'], undefined);
    assert.equal(packageJson.exports['.'].require, undefined);
    assert.equal(packageJson.files.includes('dist/server'), false);
  } finally {
    fs.rmSync(outputDirectory, { recursive: true, force: true });
  }
});

class FakeWebSocket {
  static last;

  constructor(url) {
    this.url = url;
    this.binaryType = 'blob';
    this.readyState = 0;
    this.closed = false;
    this.listeners = new Map();
    FakeWebSocket.last = this;
    queueMicrotask(() => {
      this.readyState = 1;
      this.emit('open', {});
    });
  }

  addEventListener(type, listener) {
    let listeners = this.listeners.get(type);
    if (listeners === undefined) {
      listeners = new Set();
      this.listeners.set(type, listeners);
    }
    listeners.add(listener);
  }

  removeEventListener(type, listener) {
    this.listeners.get(type)?.delete(listener);
  }

  send(data) {
    const frame = protocolCodecs.ZlinkStreamFrameCodec.decode(data);
    const header = protocolCodecs.ZlinkStreamHeaderCodec.decode(frame.header);
    const response = encodeFrame({
      kind: browserEntry.ZlinkStreamMessageKind.Response,
      codec: browserEntry.ZlinkStreamCodec.Raw,
      flags: browserEntry.ZlinkStreamHeaderFlags.HasRequestSeq,
      requestSeq: header.requestSeq,
      name: header.name
    }, 'reply');
    const progress = encodeFrame({
      kind: browserEntry.ZlinkStreamMessageKind.Send,
      codec: browserEntry.ZlinkStreamCodec.Raw,
      flags: browserEntry.ZlinkStreamHeaderFlags.None,
      name: 'BrowserPush'
    }, 'progress');
    const completed = encodeFrame({
      kind: browserEntry.ZlinkStreamMessageKind.Send,
      codec: browserEntry.ZlinkStreamCodec.Raw,
      flags: browserEntry.ZlinkStreamHeaderFlags.None,
      name: 'BrowserPush'
    }, 'completed');
    queueMicrotask(() => {
      this.emit('message', { data: exactArrayBuffer(concatenate(response, progress, completed)) });
    });
  }

  close() {
    this.closed = true;
    this.readyState = 3;
    this.emit('close', {});
  }

  emit(type, event) {
    for (const listener of [...(this.listeners.get(type) ?? [])]) {
      listener(event);
    }
  }
}

class ControlledCloseWebSocket extends FakeWebSocket {
  close() {
    this.closeRequested = true;
  }

  finishClose() {
    this.closed = true;
    this.readyState = 3;
    this.emit('close', {});
  }
}

class MemoryConnection {
  constructor() {
    this.frames = [];
  }

  async write(frame) {
    this.frames.push(frame);
  }

  async close() {}
}

function encodeFrame(header, payload) {
  return protocolCodecs.ZlinkStreamFrameCodec.encode(
    protocolCodecs.ZlinkStreamHeaderCodec.encode({
      ...header,
      metadata: browserEntry.ZlinkStreamMetadataMap.empty
    }),
    new TextEncoder().encode(payload)
  );
}

function exactArrayBuffer(bytes) {
  return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
}

function concatenate(...chunks) {
  const combined = new Uint8Array(chunks.reduce((size, chunk) => size + chunk.length, 0));
  let offset = 0;
  for (const chunk of chunks) {
    combined.set(chunk, offset);
    offset += chunk.length;
  }
  return combined;
}
