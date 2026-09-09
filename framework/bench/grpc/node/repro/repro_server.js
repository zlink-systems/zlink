// SPDX-License-Identifier: MPL-2.0
'use strict';
// Minimal ROUTER echo server. Uses bindings/node directly; shares no code with
// the with-grpc harness. Two receive modes so that the harness's own
// setImmediate pump can be ruled in or out as the cause.
//
//   --mode blocking  a tight blocking recv loop (the shape .NET's server uses)
//   --mode spin      DontWait + setImmediate batching (the shape the bench uses)

const zlink = require('@zlink-systems/zlink');

const argv = process.argv.slice(2);
const arg = (name, fallback) => {
  const i = argv.indexOf(name);
  return i >= 0 && i + 1 < argv.length ? argv[i + 1] : fallback;
};

const endpoint = arg('--endpoint', 'tcp://127.0.0.1:5185');
const mode = arg('--mode', 'blocking');
const batch = Number.parseInt(arg('--batch', '256'), 10);

const ctx = zlink.createContext();
const router = zlink.createRouterSocket(ctx);
router.setRoutingId(zlink.RoutingId.from(Buffer.from('repro-server', 'ascii')));
router.options.mandatory = true;
router.bind(endpoint);
process.stderr.write(`[repro-server] mode=${mode} endpoint=${endpoint}\n`);

const received = new zlink.Received();

function handleOne(hasMessage) {
  if (!hasMessage) return false;
  try {
    const parts = received.parts;
    const echoed = parts.map((part) => Buffer.from(part.data()));
    const op = received.replyToken !== null ? received.reply() : received.send();
    let builder = op;
    for (const part of echoed) builder = builder.message(part);
    builder.submit();
  } catch (error) {
    process.stderr.write(`[repro-server] reply failed: ${error.message}\n`);
  } finally {
    received.close();
  }
  return true;
}

if (mode === 'blocking') {
  for (;;) {
    let hasMessage = false;
    try {
      hasMessage = router.recv(received, zlink.RecvFlags.None);
    } catch (error) {
      if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) continue;
      process.stderr.write(`[repro-server] recv failed: ${error.message}\n`);
      continue;
    }
    handleOne(hasMessage);
  }
} else {
  const IDLE_SPIN_LIMIT = 20000;
  let idle = 0;
  const pump = () => {
    let handled = 0;
    while (handled < batch) {
      let hasMessage = false;
      try {
        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
      } catch (error) {
        if (!(error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData)) {
          process.stderr.write(`[repro-server] recv failed: ${error.message}\n`);
        }
        break;
      }
      if (!hasMessage) break;
      handled += 1;
      handleOne(true);
    }
    if (handled > 0) { idle = 0; setImmediate(pump); return; }
    idle += 1;
    if (idle < IDLE_SPIN_LIMIT) setImmediate(pump);
    else setTimeout(pump, 1);
  };
  setImmediate(pump);
}
