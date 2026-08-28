// SPDX-License-Identifier: MPL-2.0

'use strict';

const zlink = require('@zlink-systems/zlink');
const readline = require('node:readline');
const { MonitorEventType, RecvFlags, RecvResult } = zlink;
const {
  applyAutoHwmProfile,
  integerEnv,
  manualSocketOverridesEnabled,
  sleepImmediate
} = require('../common/perf_metrics');
const POLLIN = 1;
const POLLOUT = 2;
const { emitMultiSocketHwmDetail } = require('./perf_multi_auto_hwm');
const { resolveMultiMonitorHwm } = require('./perf_multi_common');

function measurementPartCount() {
  return process.env.PERF_PART_COUNT === '1' ? 1 : 2;
}

function measurementParts(payload) {
  return measurementPartCount() === 1 ? [payload] : [payload, Buffer.alloc(0)];
}

function measurementPayload(parts) {
  if (!Array.isArray(parts) || parts.length !== measurementPartCount()) return null;
  if (measurementPartCount() === 2 && parts[1].data().length !== 0) return null;
  return parts[0];
}

function integerEnvPair(primary, fallbackName, fallback) {
  return integerEnv(primary, integerEnv(fallbackName, fallback));
}

function pollEvents(mask) {
  const events = [];
  if ((mask & POLLIN) !== 0) {
    events.push(zlink.PollEventFlag.PollIn);
  }
  if ((mask & POLLOUT) !== 0) {
    events.push(zlink.PollEventFlag.PollOut);
  }
  return events;
}

function pollEventHas(event, mask) {
  return ((event?.revents ?? event?.events ?? 0) & mask) !== 0;
}

function waitPollerOne(poller, events, timeoutMs) {
  let count;
  try {
    count = poller.wait(events, timeoutMs);
  } catch (error) {
    // C perf retries an interrupted poll. A signal must not turn a waiting
    // benchmark endpoint into a failed measurement.
    if (error instanceof zlink.RecvError && error.nativeErrno === 4) {
      return null;
    }
    throw error;
  }
  if (count <= 0) return null;
  return {
    sourceKind: events.sourceKind(0),
    slot: events.slot(0),
    revents: events.revents(0),
    fd: events.fd(0)
  };
}

function applySocketPolicy(
  socket,
  options: { transport?: string; recvTimeout?: number; noDrop?: boolean } = {}
) {
  const linger = integerEnv('PERF_MULTI_LINGER_MS', 0);
  // C parity: bindings/c/perf/multi/common/perf_multi_runtime.hpp
  // apply_debug_timeouts (~986-997) sets ZLINK_OPT_SNDTIMEO/RCVTIMEO to
  // the 200ms default on every benchmark socket UNCONDITIONALLY, and
  // returns early (no timeouts) only for the inproc transport. Direct
  // Receive-only roles use public poller readiness; HWM-managed routed sends
  // await the canonical Promise completion terminal. Match C's timeout policy:
  // skip for inproc, otherwise apply the C default.
  const transport = String(
    options.transport || process.env.PERF_MULTI_TRANSPORT || ''
  ).trim().toLowerCase();
  const isInproc = transport === 'inproc';
  const sendTimeout = integerEnv('PERF_MULTI_SNDTIMEO_MS', 200);
  const recvTimeout = integerEnv('PERF_MULTI_RCVTIMEO_MS', 200);

  if (socket.options) {
    if (manualSocketOverridesEnabled('multi')) {
      const hwm = integerEnv('PERF_MULTI_HWM', 1000);
      const sendHwm = integerEnv('PERF_MULTI_SNDHWM', hwm);
      const recvHwm = integerEnv('PERF_MULTI_RCVHWM', hwm);
      socket.options.sendHwm = BigInt(sendHwm);
      socket.options.recvHwm = BigInt(recvHwm);
    }
    if (!isInproc) {
      socket.options.sendTimeout = sendTimeout;
      socket.options.recvTimeout = options.recvTimeout ?? recvTimeout;
    }
    socket.options.linger = linger;
    if ('noDrop' in socket.options && options.noDrop !== undefined) {
      socket.options.noDrop = Boolean(options.noDrop);
    }
  }
}

function resolveMultiIoThreads(role, pattern) {
  const normalizedRole = String(role || '').trim().toLowerCase();
  const roleKey = normalizedRole === 'server' ? 'SERVER' : 'CLIENT';
  const isStream = pattern === 'MULTI_STREAM';
  const envNames = isStream
    ? [`PERF_MULTI_STREAM_${roleKey}_IO_THREADS`, `PERF_MULTI_${roleKey}_IO_THREADS`]
    : [`PERF_MULTI_${roleKey}_IO_THREADS`];

  for (const name of envNames) {
    const value = integerEnv(name, NaN);
    if (Number.isFinite(value) && value >= 0) {
      return value;
    }
  }

  const shared = integerEnv('PERF_IO_THREADS', NaN);
  if (Number.isFinite(shared) && shared >= 0) {
    return shared;
  }

  const fallback = integerEnv('PERF_MULTI_DEFAULT_IO_THREADS', integerEnv('PERF_DEFAULT_IO_THREADS', NaN));
  if (Number.isFinite(fallback) && fallback >= 0) {
    return fallback;
  }

  return 4;
}

function applyContextPolicy(ctx, role, pattern) {
  ctx.options.ioThreads = resolveMultiIoThreads(role, pattern);
  const maxSockets = integerEnv('PERF_MAX_SOCKETS', NaN);
  if (Number.isFinite(maxSockets) && maxSockets > 0) {
    ctx.options.maxSockets = maxSockets;
  }
  ctx.options.blocky = integerEnv('PERF_CTX_BLOCKY', 0) !== 0;
  ctx.options.autoHwmEnabled = true;
  applyAutoHwmProfile(ctx, zlink);
}

function recvNoWait(socket) {
  const received = new zlink.Received();
  return recvNoWaitInto(socket, received) ? received : null;
}

function recvNoWaitInto(socket, received) {
  try {
    return socket.recv(received, RecvFlags.DontWait);
  } catch (error) {
    if (error instanceof zlink.RecvError && error.result === RecvResult.NoData) {
      return false;
    }
    throw error;
  }
}

async function waitForConnectionReady(
  socket,
  connectFn = null,
  timeoutMs = integerEnvPair('PERF_MULTI_CONNECT_READY_TIMEOUT_MS', 'PERF_CONNECT_READY_TIMEOUT_MS', 10000)
) {
  return waitForConnectionReadyCount(socket, 1, connectFn, timeoutMs);
}

async function waitForConnectionReadyCount(
  socket,
  expectedCount,
  connectFn = null,
  timeoutMs = integerEnvPair('PERF_MULTI_CONNECT_READY_TIMEOUT_MS', 'PERF_CONNECT_READY_TIMEOUT_MS', 10000)
) {
  const monitor = socket.monitorOpen(
    [MonitorEventType.ConnectionReady],
    BigInt(resolveMultiMonitorHwm())
  );
  try {
    if (typeof connectFn === 'function') {
      await connectFn();
    }
    const targetCount = Math.max(1, Math.trunc(expectedCount || 1));
    let readyCount = 0;
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      let drained = false;
      try {
        while (true) {
          const event = monitor.recv(RecvFlags.DontWait);
          if (!event) {
            break;
          }
          drained = true;
          if (event.event === MonitorEventType.ConnectionReady) {
            readyCount += 1;
            if (readyCount >= targetCount) {
              return;
            }
          }
        }
      } catch (error) {
        if (!(error instanceof zlink.RecvError && error.result === RecvResult.NoData)) {
          throw error;
        }
      }
      if (!drained) {
        await sleepMs(1);
      }
    }
    throw new Error(
      `connection ready timeout after ${timeoutMs}ms (${readyCount}/${targetCount})`
    );
  } finally {
    monitor.close();
  }
}

async function sendRouted(socket, ...args) {
  const routed = args.length >= 2 && args[0] instanceof zlink.RoutingId;
  const payload = routed ? args[1] : args[0];
  let op = routed ? socket.send(args[0]) : socket.send();
  const parts = Array.isArray(payload) ? payload : measurementParts(payload);
  for (const part of parts) op = op.message(part);
  await op.submit();
}

// PERF_MULTI_TEST_POLICY § 1.3.1: emit the wire-level stop token once at
// phase end. Callers pass a closure that performs the actual send (e.g.
// router.send(routingId, ...)); a failed sentinel is a benchmark failure.
async function sendStopTokenOnce(_socket, sendFn) {
  const stopBytes = require('../perf_stop_token').STOP_TOKEN_BYTES;
  if (!(await sendFn(stopBytes))) {
    throw new Error('stop token send failed');
  }
}

function trySocketPublish(socket, topic, payload) {
  try {
    let op = socket.publish(topic);
    const parts = Array.isArray(payload) ? payload : measurementParts(payload);
    for (const part of parts) {
      op = op.message(part);
    }
    op.submit();
    return true;
  } catch (error) {
    if (error instanceof zlink.SubmitError && error.result === zlink.SubmitResult.Backpressured) {
      return false;
    }
    const text = String(error && error.message ? error.message : error);
    if ((error && error.code === 'EAGAIN') || text.includes('Resource temporarily unavailable')) {
      return false;
    }
    throw error;
  }
}

function sleepMs(ms) {
  return new Promise((resolve) => setTimeout(resolve, Math.max(0, ms)));
}

async function waitForRunnerStart(msgSize) {
  const rl = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });
  try {
    for await (const line of rl) {
      if (line === `START,${msgSize}` || line === 'STOP' || line === 'QUIT') {
        return line;
      }
    }
    return null;
  } finally {
    rl.close();
  }
}

function createSocketEventWaiter(socket, events) {
  const poller = zlink.createPoller();
  poller.add(socket, pollEvents(events), 0);
  const eventBuffer = zlink.createPollEvents(1);

  return {
    // PERF_MULTI_TEST_POLICY § 1.3.1: signal-driven `-1` wait. The core
    // emits a wakeup on every relevant event, so timer fallbacks are not
    // needed. The leading `sleepImmediate()` lets queued microtasks run
    // before we descend into the synchronous N-API wait.
    async wait(mask = events) {
      while (true) {
        await sleepImmediate();
        let ready = null;
        try {
          ready = waitPollerOne(poller, eventBuffer, -1);
        } catch (error) {
          const text = String(error && error.message ? error.message : error);
          if ((error && error.code === 'EAGAIN') || text.includes('Resource temporarily unavailable')) {
            continue;
          }
          throw error;
        }
        if (ready && pollEventHas(ready, mask)) {
          return ready;
        }
      }
    },
    close() {
      eventBuffer.close();
      poller.close();
    }
  };
}

module.exports = {
  POLLIN,
  POLLOUT,
  applyContextPolicy,
  applySocketPolicy,
  createSocketEventWaiter,
  emitMultiSocketHwmDetail,
  pollEvents,
  pollEventHas,
  waitPollerOne,
  recvNoWait,
  recvNoWaitInto,
  resolveMultiMonitorHwm,
  sendStopTokenOnce,
  sendRouted,
  trySocketPublish,
  measurementParts,
  measurementPayload,
  waitForRunnerStart,
  waitForConnectionReadyCount,
  waitForConnectionReady
};
