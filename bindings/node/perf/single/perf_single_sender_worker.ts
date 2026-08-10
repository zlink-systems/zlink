// SPDX-License-Identifier: MPL-2.0

'use strict';

const { parentPort, workerData } = require('node:worker_threads');
const zlink = require('@zlink-systems/zlink');
const {
  createPayload,
  stampPayload
} = require('../common/perf_metrics');
const {
  applyContextPolicy,
  applyAutoHwmMsgUnit,
  applySocketPolicy,
  configureTlsClient,
  configureTlsServer,
  emitSingleSocketHwmDetail,
  waitForConnectionReady,
} = require('./perf_single_common');
const { STOP_TOKEN_BYTES } = require('../perf_stop_token');

const DEFAULT_TOPIC = 'perf.topic';

function ensureParentPort() {
  if (!parentPort) {
    throw new Error('sender worker requires parentPort');
  }
  return parentPort;
}

function trace(message) {
  if (process.env.PERF_NODE_TRACE === '1') {
    console.error(`[sender-worker] ${message}`);
  }
}

function waitForCommand(port, type) {
  return new Promise((resolve) => {
    const handler = (message) => {
      if (!message || message.type !== type) {
        return;
      }
      port.off('message', handler);
      resolve(message);
    };
    port.on('message', handler);
  });
}

async function connectSender(kind, socket, endpoint, transport) {
  configureTlsClient(socket, transport);
  await waitForConnectionReady(socket, () => socket.connect(endpoint));
}

async function handshakeRouterSender(port, sender, receiverRoutingId) {
  port.postMessage({ type: 'connected' });
  await waitForCommand(port, 'handshake');
  sender.send(receiverRoutingId).message(Buffer.from('PING')).submit();
  const reply = new zlink.Received();
  sender.recv(reply);
  try {
    const text = reply.parts.map((part) => part.data().toString()).join(',');
    if (text !== 'PONG') {
      throw new Error('router-router handshake reply failed');
    }
    if (reply.routingId) {
      return reply.routingId;
    }
    return receiverRoutingId;
  } finally {
    reply.close();
  }
}

// C parity: generic one-way patterns use ZLINK_DONTWAIT + retry-on-EAGAIN
// through perf_single_one_way.hpp, while routed one-way patterns use
// ZLINK_SEND_FLAGS_NONE in their active send loops and still treat transient
// backpressure as retry. In both cases a transient submit must never become
// a thrown failure or a silent drop. PERF_SINGLE_TEST_POLICY § 1.4.
function isTransientSubmit(error) {
  const text = String(error && error.message ? error.message : error);
  return (error instanceof zlink.SubmitError
      && (error.result === zlink.SubmitResult.Backpressured
        || error.result === zlink.SubmitResult.NotConnected
        || error.result === zlink.SubmitResult.NotFound))
    || (error && error.code === 'EAGAIN')
    || /Resource temporarily unavailable|temporarily unavailable|would block|timed out|Host unreachable|not connected/i.test(text);
}

function submitOnce(kind, socket, body, receiverRoutingId, topic) {
  if (kind === 'pubsub') {
    return socket.publish(topic).message(body)
      .flags(zlink.SendFlags.DontWait).submit();
  }
  if (kind === 'router_router') {
    return socket.send(receiverRoutingId).message(body)
      .flags(zlink.SendFlags.None).submit();
  }
  if (kind === 'dealer_router') {
    return socket.send().message(body).flags(zlink.SendFlags.None).submit();
  }
  return socket.send().message(body).flags(zlink.SendFlags.DontWait).submit();
}

// Retry through transient backpressure until accepted (C send_step_retry
// loop). Returns when the message is on the wire; throws only on a real
// fatal error. `deadlineNs` (optional) bounds the active-sample retry the
// same way C's send_active_samples is bounded by the duration deadline.
function submitWithRetry(kind, socket, body, receiverRoutingId, topic, deadlineNs) {
  for (;;) {
    try {
      if (submitOnce(kind, socket, body, receiverRoutingId, topic)) {
        return true;
      }
    } catch (error) {
      if (!isTransientSubmit(error)) {
        throw error;
      }
    }
    if (deadlineNs !== undefined && process.hrtime.bigint() >= deadlineNs) {
      return false;
    }
  }
}

const sleepBuffer = new Int32Array(new SharedArrayBuffer(4));

function sleepMillis(ms) {
  Atomics.wait(sleepBuffer, 0, 0, ms);
}

function submitStopOnce(kind, socket, receiverRoutingId, topic) {
  if (kind === 'pubsub') {
    socket.publish(topic).message(STOP_TOKEN_BYTES)
      .flags(zlink.SendFlags.None).submit();
    return;
  }
  if (kind === 'router_router') {
    socket.send(receiverRoutingId).message(STOP_TOKEN_BYTES)
      .flags(zlink.SendFlags.None).submit();
    return;
  }
  socket.send().message(STOP_TOKEN_BYTES).flags(zlink.SendFlags.None).submit();
}

function sendStopToken(kind, socket, receiverRoutingId, topic) {
  // PERF_SINGLE_TEST_POLICY § 1.4 / C send_stop_token_with_retry
  // (~202-215): emit the wire-level stop token once, retrying through
  // transient backpressure so the terminator always reaches the peer.
  trace(`sendStopToken begin kind=${kind}`);
  for (let retry = 0; retry < 100; retry += 1) {
    try {
      submitStopOnce(kind, socket, receiverRoutingId, topic);
      trace(`sendStopToken sent kind=${kind}`);
      return;
    } catch (error) {
      if (!isTransientSubmit(error)) {
        throw error;
      }
      sleepMillis(1);
    }
  }
  throw new Error('stop token send retry budget exhausted');
}

function sendLoop(kind, socket, payload, duration, runId, msgSize, seqStart, receiverRoutingId, topic) {
  const activeStopNs = process.hrtime.bigint() + BigInt(Math.floor(duration * 1_000_000_000));
  let seq = seqStart;
  while (process.hrtime.bigint() < activeStopNs) {
    stampPayload(payload, { phase: 1, runId, msgSize, seq });
    // C send_active_samples: a retried (backpressured) send does not
    // advance seq until it is actually accepted; the duration deadline
    // bounds the retry so we never block past the active window.
    if (submitWithRetry(kind, socket, payload, receiverRoutingId, topic, activeStopNs)) {
      seq += 1n;
    }
  }
  // C single sends active samples until the deadline and then sends only
  // the wire stop token. There is no post-active phase-2 payload.
  trace(`sendLoop active done kind=${kind} seq=${seq.toString()}`);
  sendStopToken(kind, socket, receiverRoutingId, topic);
}

async function main() {
  const port = ensureParentPort();
  const {
    kind,
    transport,
    endpoint,
    duration,
    msgSize,
    runId,
    receiverRoutingIdBytes,
    senderRoutingIdBytes,
    options
  } = workerData;
  const topic = typeof workerData.topic === 'string' && workerData.topic.length > 0
    ? workerData.topic
    : DEFAULT_TOPIC;
  const ctx = zlink.createContext();
  applyContextPolicy(ctx);
  const payload = createPayload(msgSize);
  let socket = null;
  let activeReceiverRoutingId = receiverRoutingIdBytes
    ? zlink.RoutingId.from(Buffer.from(receiverRoutingIdBytes))
    : null;

  try {
    switch (kind) {
      case 'pair':
        socket = zlink.createPairSocket(ctx);
        applySocketPolicy(socket, options);
        applyAutoHwmMsgUnit(ctx, msgSize);
        ctx.recalculateAutoHwm();
        await connectSender(kind, socket, endpoint, transport);
        break;
      case 'dealer_dealer':
        socket = zlink.createDealerSocket(ctx);
        applySocketPolicy(socket, options);
        applyAutoHwmMsgUnit(ctx, msgSize);
        ctx.recalculateAutoHwm();
        await connectSender(kind, socket, endpoint, transport);
        break;
      case 'dealer_router':
        socket = zlink.createDealerSocket(ctx);
        applySocketPolicy(socket, options);
        applyAutoHwmMsgUnit(ctx, msgSize);
        ctx.recalculateAutoHwm();
        await connectSender(kind, socket, endpoint, transport);
        break;
      case 'pubsub':
        socket = zlink.createPubSocket(ctx);
        applySocketPolicy(socket, options);
        applyAutoHwmMsgUnit(ctx, msgSize);
        ctx.recalculateAutoHwm();
        configureTlsServer(socket, transport);
        socket.bind(endpoint);
        trace('pubsub bound');
        port.postMessage({ type: 'bound' });
        break;
      case 'router_router': {
        socket = zlink.createRouterSocket(ctx);
        applySocketPolicy(socket, options);
        applyAutoHwmMsgUnit(ctx, msgSize);
        socket.setRoutingId(zlink.RoutingId.from(Buffer.from(senderRoutingIdBytes)));
        ctx.recalculateAutoHwm();
        // C progresses ROUTER readiness through its PING/PONG handshake.
        // Waiting on the sender monitor first deadlocks because neither
        // ROUTER is receiving yet, so connect and let the handshake below
        // drive both peers.
        configureTlsClient(socket, transport);
        socket.connect(endpoint);
        activeReceiverRoutingId = await handshakeRouterSender(
          port,
          socket,
          activeReceiverRoutingId
        );
        break;
      }
      default:
        throw new Error(`unsupported sender worker kind: ${kind}`);
    }

    // PERF_SINGLE_TEST_POLICY § 2.0.1 / C perf_single_one_way.hpp
    // run_active_phase: single must not add a start/stop control channel.
    // The connection-ready gate is the only cross-thread sync before the
    // active window; phase end is the wire stop token alone.
    //
    // PUBSUB binds in this worker, so the subscriber must connect before
    // we publish: the main thread replies `ready` once CONNECTION_READY +
    // post-ready settle have completed (mirrors C `setup_connected_pubsub_
    // pair` ordering). All other patterns connect from this worker, so the
    // connection-ready gate is satisfied here and we proceed directly.
    if (kind === 'pubsub') {
      trace('waiting ready');
      await waitForCommand(port, 'ready');
      trace('ready received');
    } else {
      port.postMessage({ type: 'ready' });
    }

    trace(`sendLoop begin kind=${kind} duration=${duration} msgSize=${msgSize}`);
    sendLoop(
      kind,
      socket,
      payload,
      duration,
      runId,
      msgSize,
      1n,
      activeReceiverRoutingId,
      topic
    );
    trace('send loop done');
    if (kind === 'pair') {
      emitSingleSocketHwmDetail(socket, 'PAIR', transport, 'sender', msgSize);
    } else if (kind === 'dealer_dealer') {
      emitSingleSocketHwmDetail(socket, 'DEALER_DEALER', transport, 'sender', msgSize);
    } else if (kind === 'dealer_router') {
      emitSingleSocketHwmDetail(socket, 'DEALER_ROUTER', transport, 'sender', msgSize);
    } else if (kind === 'pubsub') {
      emitSingleSocketHwmDetail(socket, 'PUBSUB', transport, 'publisher', msgSize);
    }
    // C keeps the sender socket alive until the receiver has observed the
    // wire stop token and the sender thread is joined. The parent sends this
    // shutdown command only after its recv loop has completed, so this is a
    // lifetime match, not a phase-control channel.
    trace('sender done');
    port.postMessage({ type: 'done' });
    await waitForCommand(port, 'stop');
  } catch (error) {
    port.postMessage({
      type: 'error',
      message: String(error && error.stack ? error.stack : error)
    });
    process.exitCode = 1;
  } finally {
    try {
      socket?.close();
    } catch (err) {
      console.error(`[perf] close failed: ${err}`);
    }
    try {
      ctx.close();
    } catch (err) {
      console.error(`[perf] close failed: ${err}`);
    }
  }
}

main();
