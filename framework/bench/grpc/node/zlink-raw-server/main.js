// SPDX-License-Identifier: MPL-2.0
'use strict';

// `zlink-<lang>` server, node row: raw binding, ROUTER<->ROUTER (FB-001, spec section 1.3).
//
// Two ROUTERs, not one. spec section 3 separates the request echo endpoint from the
// command endpoint so that an echo reply can never be counted in the
// `send-saturation` receive total.
//
// The receive loop is DontWait plus setImmediate rather than a blocking recv:
// the settle contract (spec section 3 / FB-008) polls /bench/stats while this server is
// still draining, so the HTTP endpoint has to stay answerable throughout. A
// blocking recv on the single JS thread would stall it exactly when the client
// needs it. Messages are taken in batches so the yield cost is amortized.

const zlink = require('@zlink-systems/zlink');
const { argValue, argInt } = require('../shared/args');
const { BenchServerMetrics, startStatsServer } = require('../shared/bench-server-metrics');
const { RESPONSE_ENVELOPE, ROUTING_IDS, encodeBenchPayload, decodeBenchPayloadBody } =
  require('../shared/raw-wire');

const argv = process.argv.slice(2);
const endpoint = argValue(argv, '--endpoint', 'tcp://127.0.0.1:5085');
const commandEndpoint = argValue(argv, '--command-endpoint', 'tcp://127.0.0.1:5087');
const metricsUrl = argValue(argv, '--metrics-url', 'http://127.0.0.1:5086');
const batch = argInt(argv, '--recv-batch', 256);

const metrics = new BenchServerMetrics();
const ctx = zlink.createContext();
const requestRouter = zlink.createRouterSocket(ctx);
const commandRouter = zlink.createRouterSocket(ctx);

requestRouter.setRoutingId(zlink.RoutingId.from(Buffer.from(ROUTING_IDS.rawRequestServer, 'ascii')));
commandRouter.setRoutingId(zlink.RoutingId.from(Buffer.from(ROUTING_IDS.rawCommandServer, 'ascii')));
requestRouter.options.mandatory = true;
commandRouter.options.mandatory = true;
requestRouter.bind(endpoint);
commandRouter.bind(commandEndpoint);

startStatsServer(metricsUrl, metrics, {
  implementation: 'zlink-node',
  socket: 'ROUTER<->ROUTER',
  endpoint,
  commandEndpoint
});

function tryRecv(socket, received) {
  try {
    return socket.recv(received, zlink.RecvFlags.DontWait);
  } catch (error) {
    if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) return false;
    throw error;
  }
}

function bodyPart(received) {
  const parts = received.parts;
  return parts.length === 0 ? null : parts[parts.length - 1];
}

const requestReceived = new zlink.Received();
const commandReceived = new zlink.Received();

function pumpRequests() {
  let handled = 0;
  while (handled < batch) {
    let hasMessage;
    try {
      hasMessage = tryRecv(requestRouter, requestReceived);
    } catch (error) {
      metrics.recordError();
      console.error(`raw request loop recv failed: ${error.message}`);
      break;
    }
    if (!hasMessage) break;
    handled += 1;
    try {
      const part = bodyPart(requestReceived);
      const body = part === null ? null : decodeBenchPayloadBody(part.data());
      if (body === null) throw new Error('invalid raw protobuf payload');
      const reply = encodeBenchPayload(body);
      const operation = requestReceived.replyToken !== null
        ? requestReceived.reply()
        : requestReceived.send();
      operation.message(RESPONSE_ENVELOPE).message(reply).submit();
    } catch (error) {
      metrics.recordError();
      console.error(`raw request loop failed: ${error.message}`);
    } finally {
      requestReceived.close();
    }
  }
  return handled;
}

function pumpCommands() {
  let handled = 0;
  while (handled < batch) {
    let hasMessage;
    try {
      hasMessage = tryRecv(commandRouter, commandReceived);
    } catch (error) {
      metrics.recordError();
      console.error(`raw command loop recv failed: ${error.message}`);
      break;
    }
    if (!hasMessage) break;
    handled += 1;
    try {
      const part = bodyPart(commandReceived);
      const body = part === null ? null : decodeBenchPayloadBody(part.data());
      if (body === null) throw new Error('invalid raw protobuf payload');
      metrics.record(body);
    } catch (error) {
      metrics.recordError();
      console.error(`raw command loop failed: ${error.message}`);
    } finally {
      commandReceived.close();
    }
  }
  return handled;
}

// Idle policy. `setTimeout(pump, 0)` clamps to about a millisecond, which would
// have put a poll delay into every `request-serial` round trip and reported the
// harness's backoff as the stack's latency. So the loop stays hot on
// setImmediate while a cell is running (traffic keeps `handled` non-zero, and
// even an idle gap inside a serial cell is far shorter than the spin budget) and
// only falls back to the timer once the socket has been quiet for a long stretch,
// which happens between cells. setImmediate still yields to I/O callbacks, so
// /bench/stats keeps answering during the drain.
const IDLE_SPIN_LIMIT = 20000;
let idleSpins = 0;

function pump() {
  const handled = pumpRequests() + pumpCommands();
  if (handled > 0) {
    idleSpins = 0;
    setImmediate(pump);
    return;
  }
  idleSpins += 1;
  if (idleSpins < IDLE_SPIN_LIMIT) setImmediate(pump);
  else setTimeout(pump, 1);
}

console.error(`[raw-server] request=${endpoint} command=${commandEndpoint} stats=${metricsUrl}`);
setImmediate(pump);
