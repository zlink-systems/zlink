// SPDX-License-Identifier: MPL-2.0

'use strict';

const readline = require('node:readline');
const zlink = require('@zlink-systems/zlink');
const {
  currentEpochNs,
  createPayload,
  stampPayload,
} = require('../common/perf_metrics');
const { configureTlsClient } = require('../common/perf_tls');
const { parseMultiArgs } = require('./perf_multi_common');
const {
  applyContextPolicy,
  applySocketPolicy,
  emitMultiSocketHwmDetail,
  sendRouted,
  waitForConnectionReady
} = require('./perf_multi_runtime');
const { STOP_TOKEN_BYTES } = require('../perf_stop_token');

// MULTI_DEALER_DEALER client == SENDER (one DEALER socket per client).
//
// C parity: bindings/c/perf/multi/src/perf_multi_dealer_dealer_client.cpp
// is the SENDER. It creates one DEALER socket per client (connect),
// prints CLIENT_READY,<size>, waits START,<size> from stdin, runs the
// per-socket bounded send window (run_send_window ~142-265). This binding
// awaits the Core send-completion terminal instead of using the removed DONTWAIT terminal,
// then sends exactly ONE wire stop token per socket
// (run_single_size_case ~290-293 / send_stop_token ~114-140). The
// matching RECEIVER/MEASURER is perf_multi_dealer_dealer_server.cpp.
// Cross-checked against the already-fixed cpp
// bindings/cpp/perf/multi/src/perf_dealer_dealer_client.cpp.
// Handshake (PERF_MULTI § 1.5 / line 201): server READY,<endpoint> then
// client spawn; client prints CLIENT_READY,<size>; runner sends
// START,<size> to BOTH; sender runs the send window after START.
async function main() {
  const options = parseMultiArgs(process.argv.slice(2));
  const ctx = zlink.createContext();
  applyContextPolicy(ctx, 'client', 'MULTI_DEALER_DEALER');
  const dealers = [];
  let rl = null;

  try {
    for (let i = 0; i < options.clients; i += 1) {
      const dealer = zlink.createDealerSocket(ctx);
      applySocketPolicy(dealer, { transport: options.transport });
      configureTlsClient(dealer, options.transport);
      dealers.push(dealer);
    }
    for (let i = 0; i < dealers.length; i += 1) {
      const dealer = dealers[i];
      await waitForConnectionReady(dealer, () => dealer.connect(options.endpoint));
    }
    ctx.recalculateAutoHwm();
    for (const dealer of dealers) {
      emitMultiSocketHwmDetail(dealer, 'endpoint', options.transport, options.msgSize);
    }

    console.log(`CLIENT_READY,${options.msgSize}`);
    rl = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });
    for await (const line of rl) {
      if (line === `START,${options.msgSize}`) {
        break;
      }
      if (line === 'STOP' || line === 'QUIT') {
        return;
      }
    }

    const payloads = dealers.map(() => createPayload(options.msgSize));
    const activeStopNs = currentEpochNs() + BigInt(Math.floor(options.duration * 1_000_000_000));
    let seq = 1n;
    await Promise.all(dealers.map(async (dealer, index) => {
      while (currentEpochNs() < activeStopNs) {
        const currentSeq = seq;
        seq += 1n;
        stampPayload(payloads[index], {
          phase: 1, runId: 1, msgSize: options.msgSize, seq: currentSeq
        });
        await sendRouted(dealer, payloads[index]);
      }
      await sendRouted(dealer, [STOP_TOKEN_BYTES]);
    }));
    console.log(`CLIENT_DONE,${options.msgSize}`);
  } finally {
    rl?.close();
    for (const dealer of dealers) {
      dealer.close();
    }
    ctx.close();
  }
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
