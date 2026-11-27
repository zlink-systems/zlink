const assert = require('node:assert/strict');
const { once } = require('node:events');
const fs = require('node:fs');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const tls = require('node:tls');
const { setTimeout: delay } = require('node:timers/promises');
const { Agent, WebSocket } = require('undici');
const zlink = require('@zlink-systems/zlink');
const connector = require('../../packages/stream-connector/dist');
const {
  ZLinkStreamBindingRuntime,
  ZLinkStreamSessionNodeRuntime
} = require('../../packages/framework/dist/internal');
const { ZLinkNodeBackendAdapterFactory } = require('../../packages/framework/dist/runtime/backend');
const {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} = require('../../packages/framework/dist/runtime/host/application-job-queue');
const {
  ZLinkStreamCloseReasonCode
} = require('../../packages/framework/dist/runtime/streams/protocol');
const {
  ZlinkStreamFrameCodec,
  ZlinkStreamHeaderCodec
} = require('./helpers/stream-protocol-codecs');
const { FakeLivenessClock } = require('./helpers/stream-liveness-clock');

const interval = 200;
const heartbeat = 1000;
const idle = 450;
const ping = '$zlink.heartbeat.ping';
const pong = '$zlink.heartbeat.pong';
const cert = path.resolve(__dirname, '../fixtures/tls/server-cert.pem');
const key = path.resolve(__dirname, '../fixtures/tls/server-key.pem');
const transports = ['tcp', 'tls', 'ws', 'wss', 'inproc'];
if (zlink.has('ipc')) transports.push('ipc');

for (const transport of transports) {
  for (const [name, hold, reason] of [
    ['unsaturated', 0],
    ['below_timeout', 300],
    ['at_timeout', heartbeat],
    ['above_timeout', 1500],
    ['silent_after_release', 1500, ZLinkStreamCloseReasonCode.HeartbeatTimeout],
    ['idle_after_release', 1500, ZLinkStreamCloseReasonCode.IdleTimeout]
  ]) {
    test(`STREAM permit liveness ${transport} ${name}`, { timeout: 10000 }, async (t) => {
      const clock = new FakeLivenessClock();
      const queue = new ApplicationJobQueue(
        resolveApplicationJobQueueConfiguration({ maxQueuedApplicationJobs: 2n })
      );
      const captures = [];
      const sessions = new Map();
      const errors = [];
      const factory = new ZLinkNodeBackendAdapterFactory();
      const context = factory.createChannelAdapter().createContext();
      const adapter = factory.createStreamAdapter();
      const socket = adapter.createStreamSocket(context);
      let tempDir;
      let runtime;
      const peers = [];
      t.after(async () => {
        await runtime?.dispose();
        for (const peer of peers) await peer.close();
        await socket.dispose();
        await context.dispose();
        if (tempDir !== undefined) fs.rmSync(tempDir, { recursive: true });
      });

      if (transport === 'tls' || transport === 'wss') socket.setTlsServer(cert, key);
      let bind = `${transport}://127.0.0.1:*`;
      if (transport === 'ipc') {
        tempDir = fs.mkdtempSync(path.join(os.tmpdir(), 'zlink-stream-permit-'));
        bind = `ipc://${path.join(tempDir, 'stream.sock')}`;
      } else if (transport === 'inproc') {
        bind = 'inproc://stream-permit-liveness';
      }
      socket.bind(bind);
      const submit = socket.submit.bind(socket);
      t.mock.method(socket, 'submit', async (rid, message) => {
        const frame = ZlinkStreamFrameCodec.decode(message.data());
        const header = ZlinkStreamHeaderCodec.decode(frame.header);
        const at = clock.now();
        await submit(rid, message);
        captures.push({ rid: rid.toHex(), name: header.name, at, reason: frame.payload[1] });
      });
      runtime = new ZLinkStreamSessionNodeRuntime({
        socket,
        readablePoller: adapter.createReadablePoller(socket),
        createPacket: () => adapter.createStreamPacket(),
        applicationJobQueue: queue,
        livenessClock: clock,
        heartbeatIntervalMs: interval,
        heartbeatTimeoutMs: heartbeat,
        idleTimeoutMs: reason === ZLinkStreamCloseReasonCode.IdleTimeout ? idle : 0,
        bindingRuntime: new ZLinkStreamBindingRuntime({
          messageFactory: {
            createTextMessage: (value) => zlink.Message.from(Buffer.from(value)),
            createBinaryMessage: (value) => zlink.Message.from(Buffer.from(value))
          }
        }),
        sessionFactory(sessionContext) {
          return {
            context: sessionContext,
            async onDispatch(header) {
              sessions.set(header.packetName, sessionContext.routingId);
            }
          };
        },
        onError: (error) => errors.push(error)
      });
      runtime.start();

      async function connect(tag) {
        const peer = await openPeer(socket.lastEndpoint, transport, context.nativeInstance);
        peers.push(peer);
        await peer.send(
          encode(
            tag,
            connector.ZlinkStreamMessageKind.Send,
            connector.ZlinkStreamCodec.Json,
            Buffer.from('null')
          )
        );
        await until(
          () => sessions.has(tag) && queue.snapshot().permitsInUse === 0n,
          `initial application frame on ${tag}`
        );
        const rid = sessions.get(tag);
        runtime.markConnected(rid);
        await clock.flush();
        return { peer, rid };
      }

      const target = await connect('target');
      const probe =
        reason === ZLinkStreamCloseReasonCode.HeartbeatTimeout ? await connect('probe') : target;
      const targetFrames = (frameName) =>
        captures.filter((frame) => frame.rid === target.rid && frame.name === frameName);
      const probed = () =>
        captures.filter((frame) => frame.rid === probe.rid && frame.name === pong).length;
      const pingFrame = encode(
        ping,
        connector.ZlinkStreamMessageKind.Control,
        connector.ZlinkStreamCodec.Raw,
        Buffer.alloc(0)
      );
      const sendPing = async () => {
        const count = probed();
        await probe.peer.send(pingFrame);
        await until(() => probed() === count + 1, 'pong');
      };
      const closing = () => targetFrames('session-closing');

      if (hold === 0) {
        for (let step = 0; step < 5; step += 1) {
          await clock.advance(300);
          await sendPing();
        }
        assert.equal(probed(), 5);
        assert.deepEqual(closing(), []);
        assert.deepEqual(errors, []);
        return;
      }

      // Separate the session age from the duration of the permit wait.
      await clock.advance(100);
      // Hold all capacity for the paused read. After release, the second slot
      // lets the reader drain readiness while the control reply owns its permit.
      const held = await Promise.all([
        queue.acquire(undefined, 'remote'),
        queue.acquire(undefined, 'remote')
      ]);
      let releasedAt;
      try {
        await probe.peer.send(pingFrame);
        await until(() => queue.snapshot().capacityWaiters === 1n, 'reader waiting for permit');
        await clock.advance(hold);
        const expectedPings = Math.floor(clock.now() / interval);
        await until(
          () => closing().length > 0 || targetFrames(ping).length === expectedPings,
          'server liveness'
        );
        t.diagnostic(
          `hold=${hold} pings=${targetFrames(ping).length} close=${JSON.stringify(closing())}`
        );
        assert.deepEqual(closing(), [], 'session closed during application permit wait');
        assert.deepEqual(
          targetFrames(ping)
            .map((frame) => frame.at)
            .sort((left, right) => left - right),
          Array.from({ length: expectedPings }, (_, index) => (index + 1) * interval)
        );
        assert.equal(probed(), 0, 'inbound frame must still be waiting for the reader');
      } finally {
        releasedAt = clock.now();
        for (const permit of held) permit.releaseAfterInternalProcessing();
      }
      await until(() => probed() === 1, 'pong after permit grant');
      assert.deepEqual(closing(), []);

      if (reason === undefined) {
        await clock.advance(300);
        await sendPing();
        assert.deepEqual(closing(), []);
      } else {
        const timeout = reason === ZLinkStreamCloseReasonCode.IdleTimeout ? idle : heartbeat;
        await clock.advance(interval);
        await until(
          () =>
            targetFrames(ping).some(
              (frame) => frame.at > releasedAt && frame.at <= releasedAt + interval
            ),
          'first ping after grant must keep its phase'
        );
        if (reason === ZLinkStreamCloseReasonCode.IdleTimeout) await sendPing();
        await clock.advance(timeout - interval - 1);
        assert.deepEqual(closing(), [], 'session closed before its restarted deadline');
        await clock.advance(1);
        await until(() => closing().length === 1, 'session-closing after permit grant');
        assert.equal(closing()[0].reason, reason);
        assert.equal(closing()[0].at - releasedAt, timeout);
        t.diagnostic(`close_after_grant_ms=${timeout} reason=${reason}`);
      }
      assert.deepEqual(errors, []);
    });
  }
}

function encode(name, kind, codec, body) {
  const header = ZlinkStreamHeaderCodec.encode({
    kind,
    codec,
    name,
    flags: connector.ZlinkStreamHeaderFlags.None,
    metadata: connector.ZlinkStreamMetadataMap.empty
  });
  return ZlinkStreamFrameCodec.encode(header, body);
}

async function openPeer(endpoint, transport, context) {
  if (transport === 'inproc') {
    const peer = zlink.createPairSocket(context);
    peer.options.linger = 0;
    peer.connect(endpoint);
    return {
      send: (frame) => peer.send().message(Buffer.from(frame)).submit().admitted,
      close: () => peer.close()
    };
  }
  if (transport === 'ws' || transport === 'wss') {
    const agent = new Agent({ connect: { ca: fs.readFileSync(cert), servername: 'localhost' } });
    const peer = new WebSocket(endpoint, { dispatcher: agent });
    try {
      const signal = AbortSignal.timeout(2000);
      await Promise.race([
        once(peer, 'open', { signal }),
        once(peer, 'error', { signal }).then(([event]) => {
          throw event.error ?? new Error(event.message);
        })
      ]);
    } catch (error) {
      await agent.destroy();
      throw error;
    }
    return {
      send: (frame) => peer.send(frame),
      async close() {
        peer.close();
        await agent.destroy();
      }
    };
  }
  const url = new URL(endpoint);
  const peer =
    transport === 'ipc'
      ? net.createConnection(url.pathname)
      : transport === 'tls'
        ? tls.connect({ host: url.hostname, port: Number(url.port), ca: fs.readFileSync(cert) })
        : net.createConnection({ host: url.hostname, port: Number(url.port) });
  try {
    await once(peer, transport === 'tls' ? 'secureConnect' : 'connect', {
      signal: AbortSignal.timeout(2000)
    });
  } catch (error) {
    peer.destroy();
    throw error;
  }
  peer.resume();
  return {
    send: (frame) => peer.write(frame),
    close: () => peer.destroy()
  };
}

async function until(predicate, label) {
  const deadline = performance.now() + 2000;
  while (!predicate() && performance.now() < deadline) await delay(2);
  assert.ok(predicate(), label);
}
