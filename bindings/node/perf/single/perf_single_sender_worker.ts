// SPDX-License-Identifier: MPL-2.0

'use strict';

const { parentPort, workerData } = require('node:worker_threads');
const zlink = require('@zlink-systems/zlink');
const {
  createPayload,
  integerEnv,
  stampPayload
} = require('../common/perf_metrics');
const {
  applyContextPolicy,
  applySocketPolicy,
  configureTlsClient,
  configureTlsServer,
  emitSingleSocketHwmDetail,
  waitForConnectionReady,
  waitForMonitorConnectionReady,
} = require('./perf_single_common');
const { STOP_TOKEN_BYTES } = require('../perf_stop_token');
const { isStopTokenParts } = require('../perf_stop_token');

const DEFAULT_TOPIC = 'perf.topic';

function appendMeasurement(op, payload) {
  op = op.message(payload);
  if (process.env.PERF_PART_COUNT !== '1') {
    op = op.message(Buffer.alloc(0));
  }
  return op;
}

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

function signalStatus(port, status: Int32Array, type, code, extra = {}) {
  Atomics.store(status, 0, code);
  Atomics.notify(status, 0);
  port.postMessage({ type, ...extra });
}

function waitForRelease(control: Int32Array) {
  while (Atomics.load(control, 0) === 0) {
    Atomics.wait(control, 0, 0);
  }
}

function connectSender(kind, socket, endpoint, transport) {
  configureTlsClient(socket, transport);
  waitForConnectionReady(socket, () => socket.connect(endpoint));
}

function handshakeRouterSender(port, control, status, sender, receiverRoutingId) {
  signalStatus(port, status, 'connected', 2);
  waitForRelease(control);
  const configuredMs = integerEnv('PERF_ROUTER_HANDSHAKE_TIMEOUT_MS', 3000);
  const timeoutMs = configuredMs > 0 ? configuredMs : 3000;
  const deadlineNs = process.hrtime.bigint() + BigInt(timeoutMs) * 1_000_000n;
  let pingSent = false;
  while (!pingSent && process.hrtime.bigint() < deadlineNs) {
    try {
      sender.send(receiverRoutingId)
        .message(Buffer.from('PING')).submit_sync(zlink.SendFlags.DontWait);
      pingSent = true;
    } catch (error) {
      if (!isTransientSubmit(error)) {
        throw error;
      }
    }
    if (!pingSent) {
      sleepMillis(1);
    }
  }
  if (!pingSent) {
    throw new Error('router-router handshake ping timeout');
  }
  const reply = new zlink.Received();
  try {
    let received = false;
    while (!received && process.hrtime.bigint() < deadlineNs) {
      try {
        received = sender.recv(reply, zlink.RecvFlags.DontWait);
      } catch (error) {
        if (!(error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData)) {
          throw error;
        }
      }
      if (!received) {
        sleepMillis(1);
      }
    }
    if (!received) {
      throw new Error('router-router handshake pong timeout');
    }
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

// Active raw sends use the synchronous blocking public terminal. Core owns
// HWM admission; the worker does not recreate DONTWAIT retry behavior.
// PERF_SINGLE_TEST_POLICY § 1.4.
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
  const message = process.env.PERF_NODE_MESSAGE_PAYLOAD === '1'
    ? zlink.Message.from(body)
    : body;
  if (kind === 'pubsub') {
    appendMeasurement(socket.publish(topic), message).submit();
    return true;
  }
  if (kind === 'router_router') {
    appendMeasurement(socket.send(receiverRoutingId), message)
      .submit_sync(zlink.SendFlags.None);
    return true;
  }
  if (kind === 'dealer_router') {
    appendMeasurement(socket.send(), message).submit_sync(zlink.SendFlags.None);
    return true;
  }
  if (kind === 'dealer_dealer') {
    appendMeasurement(socket.send(), message).submit_sync(zlink.SendFlags.None);
    return true;
  }
  appendMeasurement(socket.send(), message).submit_sync(zlink.SendFlags.None);
  return true;
}

const sleepBuffer = new Int32Array(new SharedArrayBuffer(4));

function sleepMillis(ms) {
  Atomics.wait(sleepBuffer, 0, 0, ms);
}

function submitStopOnce(kind, socket, receiverRoutingId, topic) {
  if (kind === 'pubsub') {
    socket.publish(topic).message(STOP_TOKEN_BYTES).submit();
    return;
  }
  if (kind === 'router_router') {
    socket.send(receiverRoutingId).message(STOP_TOKEN_BYTES)
      .submit_sync(zlink.SendFlags.None);
    return;
  }
  if (kind === 'dealer_router' || kind === 'dealer_dealer') {
    socket.send().message(STOP_TOKEN_BYTES).submit_sync(zlink.SendFlags.None);
    return;
  }
  socket.send().message(STOP_TOKEN_BYTES).submit_sync(zlink.SendFlags.None);
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
    try {
      submitOnce(kind, socket, payload, receiverRoutingId, topic);
    } catch (error) {
      // The terminal is still the synchronous blocking public call. A socket
      // SNDTIMEO can return transient backpressure before the active deadline,
      // notably while WSS PUB/NODROP flow control catches up. Retry that same
      // logical sample; never advance seq or substitute a DONTWAIT/POLLOUT path.
      if (!isTransientSubmit(error)) {
        throw error;
      }
      continue;
    }
    seq += 1n;
  }
  // C single sends active samples until the deadline and then sends only
  // the wire stop token. There is no post-active phase-2 payload.
  trace(`sendLoop active done kind=${kind} seq=${seq.toString()}`);
  sendStopToken(kind, socket, receiverRoutingId, topic);
}

function runReqRepReplier(router) {
  const received = new zlink.Received();
  try {
    for (;;) {
      let hasMessage = false;
      try {
        hasMessage = router.recv(received, zlink.RecvFlags.None);
      } catch (error) {
        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) {
          continue;
        }
        throw error;
      }
      if (!hasMessage) {
        continue;
      }
      if (isStopTokenParts(received.parts)) {
        return;
      }
      if (received.requestSeq === null) {
        throw new Error('request is missing request correlation metadata');
      }
      const count = process.env.PERF_PART_COUNT === '1' ? 1 : 2;
      if (received.parts.length !== count
          || (count === 2 && received.parts[1].data().length !== 0)) {
        throw new Error('request has an invalid measurement part layout');
      }
      appendMeasurement(received.reply(), Buffer.from(received.parts[0].data())).submit();
      received.close();
    }
  } finally {
    received.close();
  }
}

function main() {
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
  const control = new Int32Array(workerData.controlBuffer);
  const status = new Int32Array(workerData.statusBuffer);
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
        ctx.recalculateAutoHwm();
        connectSender(kind, socket, endpoint, transport);
        break;
      case 'dealer_dealer':
        socket = zlink.createDealerSocket(ctx);
        applySocketPolicy(socket, options);
        ctx.recalculateAutoHwm();
        connectSender(kind, socket, endpoint, transport);
        break;
      case 'dealer_router':
        socket = zlink.createDealerSocket(ctx);
        applySocketPolicy(socket, options);
        ctx.recalculateAutoHwm();
        connectSender(kind, socket, endpoint, transport);
        break;
      case 'pubsub':
        socket = zlink.createPubSocket(ctx);
        applySocketPolicy(socket, options);
        ctx.recalculateAutoHwm();
        configureTlsServer(socket, transport);
        {
          const publisherMonitor = socket.monitorOpen([
            zlink.MonitorEventType.ConnectionReady
          ]);
          try {
            socket.bind(endpoint);
            trace('pubsub bound');
            signalStatus(port, status, 'bound', 1);
            // Match C setup_connected_pubsub_pair: both the connecting SUB
            // and the binding PUB must report CONNECTION_READY before the
            // post-ready settle and active publish window begin. This is
            // load-bearing for WSS, where the SUB-side event can precede the
            // server-side ready route used by publish and the wire stop token.
            waitForMonitorConnectionReady(publisherMonitor);
            trace('pubsub connection ready');
            signalStatus(port, status, 'connected', 2);
          } finally {
            publisherMonitor.close();
          }
        }
        break;
      case 'router_router': {
        socket = zlink.createRouterSocket(ctx);
        applySocketPolicy(socket, options);
        socket.setRoutingId(zlink.RoutingId.from(Buffer.from(senderRoutingIdBytes)));
        ctx.recalculateAutoHwm();
        configureTlsClient(socket, transport);
        socket.connect(endpoint);
        activeReceiverRoutingId = handshakeRouterSender(
          port,
          control,
          status,
          socket,
          activeReceiverRoutingId
        );
        break;
      }
      case 'socket_reqrep_replier':
        socket = zlink.createRouterSocket(ctx);
        applySocketPolicy(socket, options);
        socket.setRoutingId(zlink.RoutingId.from(Buffer.from('SERVER', 'ascii')));
        socket.options.mandatory = true;
        ctx.recalculateAutoHwm();
        configureTlsServer(socket, transport);
        socket.bind(endpoint);
        signalStatus(port, status, 'bound', 1);
        waitForRelease(control);
        runReqRepReplier(socket);
        signalStatus(port, status, 'done', 4);
        if (Atomics.load(control, 0) !== 2) {
          Atomics.wait(control, 0, Atomics.load(control, 0));
        }
        return;
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
      waitForRelease(control);
      trace('ready received');
    } else {
      signalStatus(port, status, 'ready', 3);
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
    signalStatus(port, status, 'done', 4);
    const expected = Atomics.load(control, 0);
    if (expected !== 2) {
      Atomics.wait(control, 0, expected);
    }
  } catch (error) {
    signalStatus(port, status, 'error', -1, {
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
