const assert = require('node:assert/strict');
const zlink = require('@zlink-systems/zlink');
const framework = require('../../../packages/framework/dist/internal');
const {
  ZLinkNodeBackendAdapterFactory
} = require('../../../packages/framework/dist/runtime/backend');

const sendHwmBytes = 65536n;
const pendingObservationMs = 25;

async function main() {
  let context;
  let socket;
  let monitor;
  let peer;
  let sends = [];
  let heartbeat;
  let heartbeatCompleted = false;
  let onInterrupt;
  let onMessage;

  async function cleanup() {
    try {
      monitor?.close();
    } finally {
      try {
        await socket?.dispose();
      } finally {
        try {
          peer?.close();
          await Promise.all(sends);
          await heartbeat;
        } finally {
          await context?.dispose();
        }
      }
    }
  }

  try {
    const factory = new ZLinkNodeBackendAdapterFactory();
    context = factory.createChannelAdapter().createContext();
    socket = factory.createStreamAdapter().createStreamSocket(context);
    socket.nativeInstance.options.sendHwm = sendHwmBytes;
    monitor = socket.nativeInstance.monitorOpen();
    socket.nativeInstance.options.recvMode = zlink.StreamRecvMode.Raw;
    const endpoint = 'inproc://heartbeat-shutdown';
    socket.bind(endpoint);
    peer = zlink.createPairSocket(context.nativeInstance);
    peer.options.linger = 0;
    peer.options.recvHwm = sendHwmBytes;
    peer.connect(endpoint);
    await peer.send().message(Buffer.from('ready')).submit().admitted;
    const received = new zlink.Received();
    let routingId;
    try {
      while (!socket.nativeInstance.recv(received, zlink.RecvFlags.DontWait)) {
        await new Promise((resolve) => setImmediate(resolve));
      }
      routingId = received.routingId;
    } finally {
      received.close();
    }
    const readyStatus = monitor.status();
    assert.equal(readyStatus.autoHwmAppliedSndHwmBytes, sendHwmBytes);
    const payload = Buffer.alloc(
      Number(readyStatus.autoHwmAppliedSndHwmBytes - readyStatus.minimumCoreMessageChargeBytes)
    );
    const failures = [];
    let completed = 0;
    sends = Array.from({ length: 256 }, () =>
      socket.submit(routingId, payload).then(
        () => {
          completed++;
          return 'submitted';
        },
        (error) => {
          failures.push(error);
          return error;
        }
      )
    );
    // The peer never receives after readiness, so its inproc queue retains
    // its charge through SIGINT until the sockets close.
    const admissionStatus = monitor.status();
    assert.ok(completed < sends.length, 'the non-reading peer must leave native admission pending');
    assert.ok(
      admissionStatus.sndPendingBytes >= admissionStatus.autoHwmAppliedSndHwmBytes,
      'the Core send queue must leave no byte-HWM budget for the heartbeat'
    );
    assert.deepEqual(failures, []);
    const stream = new framework.ZLinkManagedStream(socket, routingId);
    heartbeat = stream.writeControl('$zlink.heartbeat.ping').then(
      () => {
        heartbeatCompleted = true;
        return 'submitted';
      },
      (error) => {
        heartbeatCompleted = true;
        return error;
      }
    );
    await new Promise((resolve) => setTimeout(resolve, pendingObservationMs));
    assert.equal(
      heartbeatCompleted,
      false,
      heartbeatCompleted
        ? String(await heartbeat)
        : 'heartbeat must still await real binding admission'
    );
    await new Promise((resolve) => {
      onInterrupt = resolve;
      onMessage = (message) => {
        if (message?.type === 'raise-sigint') process.emit('SIGINT');
      };
      process.once('SIGINT', onInterrupt);
      process.once('message', onMessage);
      process.send({ type: 'pending', completed });
    });
  } finally {
    if (onInterrupt !== undefined) process.removeListener('SIGINT', onInterrupt);
    if (onMessage !== undefined) process.removeListener('message', onMessage);
    await cleanup();
  }
  process.send({ type: 'closed', heartbeatCompleted });
}
main()
  .catch((error) => {
    console.error(error);
    process.exitCode = 1;
  })
  .finally(() => {
    if (process.connected) process.disconnect();
  });
