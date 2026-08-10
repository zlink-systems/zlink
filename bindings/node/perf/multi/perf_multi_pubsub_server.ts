// SPDX-License-Identifier: MPL-2.0

'use strict';

const readline = require('node:readline');
const zlink = require('@zlink-systems/zlink');
const {
  createPayload,
  createRunId,
  stampPayload
} = require('../common/perf_metrics');
const { configureTlsServer } = require('../common/perf_tls');
const { parseMultiArgs } = require('./perf_multi_common');
const {
  POLLOUT,
  applyAutoHwmMsgUnit,
  applyContextPolicy,
  applySocketPolicy,
  emitMultiSocketHwmDetail,
  pollEvents,
  trySocketPublish
} = require('./perf_multi_runtime');
const { STOP_TOKEN_BYTES } = require('../perf_stop_token');
const TOPIC = 'bench';

async function main() {
  const options = parseMultiArgs(process.argv.slice(2));
  const ctx = zlink.createContext();
  applyContextPolicy(ctx, 'server', 'MULTI_PUBSUB');
  const pub = zlink.createPubSocket(ctx);
  const payload = createPayload(options.msgSize);
  const poller = zlink.createPoller();
  const pollBuffer = zlink.createPollEvents(1);
  let rl = null;

  try {
    applySocketPolicy(pub, {
      noDrop: Number(process.env.PERF_MULTI_PUBSUB_XPUB_NODROP ?? 1) !== 0
    });
    configureTlsServer(pub, options.transport);
    pub.bind(options.endpoint);
    applyAutoHwmMsgUnit(ctx, options.msgSize);
    ctx.recalculateAutoHwm();
    emitMultiSocketHwmDetail(pub, 'endpoint', options.transport, options.msgSize);
    poller.add(pub, pollEvents(POLLOUT), 0);
    console.log(`READY,${options.endpoint}`);

    let activeDone = false;
    rl = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });
    for await (const line of rl) {
      if (line !== `START,${options.msgSize}` || activeDone) {
        if (line === 'STOP' || line === 'QUIT') {
          break;
        }
        continue;
      }

      const runId = createRunId(1);
      const activeStopNs = process.hrtime.bigint() + BigInt(Math.floor(options.duration * 1_000_000_000));
      let seq = 1n;
      while (process.hrtime.bigint() < activeStopNs) {
        stampPayload(payload, { phase: 1, runId, msgSize: options.msgSize, seq });
        while (!trySocketPublish(pub, TOPIC, payload)) {
          // C recreates the consumed outbound message after a POLLOUT wakeup
          // and retries the same stamped payload. The public Node publish
          // builder already creates a fresh native message for each call.
          poller.wait(pollBuffer, process.platform === 'win32' ? 50 : 100);
        }
        seq += 1n;
      }
      // PERF_MULTI_TEST_POLICY § 1.3.1 (stop token) / C
      // bindings/c/perf/multi/src/perf_multi_pubsub_server.cpp:265-268 and
      // publish_stop_token (~113-144): signal active-phase end with ONE
      // wire-level stop token on the active topic (blocking publish,
      // deadline ignored, retry only through transient backpressure — C
      // loops on EAGAIN/EWOULDBLOCK without polling). No phase=2 burst —
      // in-flight payloads naturally precede the token, which wakes the
      // subscriber's `-1` poller wait. Mirrors the already-fixed cpp
      // perf_pubsub_server.cpp publish_stop_token.
      while (!trySocketPublish(pub, TOPIC, STOP_TOKEN_BYTES)) {
        poller.wait(pollBuffer, process.platform === 'win32' ? 50 : 100);
      }
      // Keep the PUB socket open until the runner sends STOP (after the
      // measuring client has exited). Closing here with linger=0 would
      // drop the in-flight stop token before the 100 subscribers receive
      // it, so the C-faithful wire-stop-token contract requires we stay
      // alive for delivery (the runner's STOP is the shutdown edge).
      activeDone = true;
    }
  } finally {
    rl?.close();
    pollBuffer.close();
    poller.close();
    pub.close();
    ctx.close();
  }
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
