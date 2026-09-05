const assert = require('node:assert/strict');
const { once } = require('node:events');
const net = require('node:net');
const zlink = require('@zlink-systems/zlink');
const framework = require('../../../packages/framework/dist/internal');
const { ZLinkNodeBackendAdapterFactory } = require('../../../packages/framework/dist/runtime/backend');

async function main() {
  const factory = new ZLinkNodeBackendAdapterFactory();
  const context = factory.createChannelAdapter().createContext();
  const socket = factory.createStreamAdapter().createStreamSocket(context);
  socket.nativeInstance.options.sendHwm = 65536n;
  const monitor = socket.nativeInstance.monitorOpen();
  socket.bind('tcp://127.0.0.1:*');
  const endpoint = new URL(socket.nativeInstance.options.lastEndpoint);
  const peer = net.createConnection({ host: '127.0.0.1', port: Number(endpoint.port) });
  peer.pause();
  await once(peer, 'connect');
  let routingId;
  while (routingId === undefined) {
    const event = monitor.recv(zlink.RecvFlags.DontWait);
    if (event?.event === zlink.MonitorEventType.ConnectionReady) routingId = event.routingId;
    else await new Promise(resolve => setImmediate(resolve));
  }
  monitor.close();
  const payload = Buffer.alloc(65536);
  const failures = [];
  let completed = 0;
  const sends = Array.from({ length: 256 }, () => socket.submit(routingId, payload)
    .then(() => { completed++; return 'submitted'; }, error => { failures.push(error); return error; }));
  await new Promise(resolve => setTimeout(resolve, 50));
  assert.ok(completed < sends.length, 'the non-reading peer must leave native admission pending');
  assert.deepEqual(failures, []);
  const stream = new framework.ZLinkManagedStream(socket, routingId);
  let heartbeatCompleted = false;
  const heartbeat = stream.writeControl('$zlink.heartbeat.ping').then(
    () => { heartbeatCompleted = true; return 'submitted'; },
    error => { heartbeatCompleted = true; return error; }
  );
  await new Promise(resolve => setTimeout(resolve, 25));
  assert.equal(heartbeatCompleted, false,
    heartbeatCompleted ? String(await heartbeat) : 'heartbeat must still await real binding admission');
  process.once('SIGINT', () => {
    void (async () => {
      await socket.dispose();
      peer.destroy();
      await Promise.all(sends);
      await heartbeat;
      await context.dispose();
      process.send({ type: 'closed', heartbeatCompleted });
      process.disconnect();
    })().catch(error => { console.error(error); process.exitCode = 1; process.disconnect(); });
  });
  process.send({ type: 'pending', completed });
}
main().catch(error => { console.error(error); process.exitCode = 1; process.disconnect(); });
