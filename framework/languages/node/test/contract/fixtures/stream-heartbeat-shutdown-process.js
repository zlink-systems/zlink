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
  const readyStatus = monitor.status();
  assert.equal(readyStatus.autoHwmAppliedSndHwmBytes, 65536n);
  const payload = Buffer.alloc(Number(
    readyStatus.autoHwmAppliedSndHwmBytes - readyStatus.minimumCoreMessageChargeBytes
  ));
  const failures = [];
  let completed = 0;
  const sends = Array.from({ length: 256 }, () => socket.submit(routingId, payload)
    .then(() => { completed++; return 'submitted'; }, error => { failures.push(error); return error; }));
  let admissionStatus;
  let saturatedAt;
  let saturatedCompleted;
  while (completed < sends.length) {
    admissionStatus = monitor.status();
    if (admissionStatus.sndPendingBytes >= admissionStatus.autoHwmAppliedSndHwmBytes) {
      // A first full snapshot can still drain into the kernel socket buffer.
      if (saturatedCompleted !== completed) {
        saturatedAt = process.hrtime.bigint();
        saturatedCompleted = completed;
      } else if (process.hrtime.bigint() - saturatedAt >= 25_000_000n) {
        break;
      }
    } else {
      saturatedAt = undefined;
      saturatedCompleted = undefined;
    }
    await new Promise(resolve => setImmediate(resolve));
  }
  assert.ok(completed < sends.length, 'the non-reading peer must leave native admission pending');
  assert.ok(admissionStatus.sndPendingBytes >= admissionStatus.autoHwmAppliedSndHwmBytes,
    'the Core send queue must leave no byte-HWM budget for the heartbeat');
  assert.deepEqual(failures, []);
  monitor.close();
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
  process.once('message', message => {
    if (message?.type === 'raise-sigint') process.emit('SIGINT');
  });
  process.send({ type: 'pending', completed });
}
main().catch(error => { console.error(error); process.exitCode = 1; process.disconnect(); });
