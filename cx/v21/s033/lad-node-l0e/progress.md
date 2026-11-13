# #4350 Node L0 receive parity and re-measurement

- Previous L0 raw remains at `/root/jobs/fx/cx/v21/s033/lad-node/raw` and was not modified.
- Change: move L0 onto the raw server's single-threaded `DontWait`/`setImmediate` batch receive loop and reuse `BenchServerMetrics`; remove the worker implementation and per-message atomics.
- Measurement output is isolated under this directory's `raw/`.

## Before l0e-six-cells
commit=8cbbf6387b5b7ae42a6e5e6038d7f4911b77fc88 dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
Node sources are executed directly from this worktree; framework build outputs and native binding hashes:
framework/languages/node/packages/framework/dist/runtime/channels/channel-dispatchers.js sha256=2eb493e44a2738fe318252cfd07fc0861ecadb6dc335f8104f0d6bcbb3a82f0f
framework/languages/node/packages/nestjs/dist/index.js sha256=4909eed4600b8222caa46a18bb55c05d9b05b7fdb1b22bdee311b91143d8cab8
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae
.artifacts/wsl/npm/zlink-systems-zlink-1.25.0.tgz sha256=f15954789c6e6e893f704e90dabfa1d9903d32d786370d23c5ef98f7e695e0d3

Binding receive excerpt `zlink-raw-server/main.js:64-90`:
    64	function pumpRequests() {
    65	  let handled = 0;
    66	  while (handled < batch) {
    67	    let hasMessage;
    68	    try {
    69	      hasMessage = tryRecv(requestRouter, requestReceived);
    70	    } catch (error) {
    71	      metrics.recordError();
    72	      console.error(`raw request loop recv failed: ${error.message}`);
    73	      break;
    74	    }
    75	    if (!hasMessage) break;
    76	    handled += 1;
    77	    try {
    78	      const part = bodyPart(requestReceived);
    79	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
    80	      if (body === null) throw new Error('invalid raw protobuf payload');
    81	      metrics.record(body);
    82	      const reply = encodeBenchPayloadMessage(body);
    83	      const operation = requestReceived.replyToken !== null
    84	        ? requestReceived.reply()
    85	        : requestReceived.send();
    86	      operation.message(RESPONSE_ENVELOPE).message(reply).submit();
    87	    } catch (error) {
    88	      metrics.recordError();
    89	      console.error(`raw request loop failed: ${error.message}`);
    90	    }

L0 receive excerpt `zlink-framework-server/main.js:59-105`:
    57	  function pump() {
    58	    let handled = 0;
    59	    while (handled < batch) {
    60	      let hasMessage;
    61	      try {
    62	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    63	      } catch (error) {
    64	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
    65	        metrics.recordError();
    66	        console.error(`ladder L0 recv failed: ${error.message}`);
    67	        break;
    68	      }
    69	      if (!hasMessage) break;
    70	      handled += 1;
    71	      const parts = received.parts;
    72	      try {
    73	        const header = frameworkWire ? parts[0].data() : null;
    74	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
    75	        if (payload === null) throw new Error('invalid benchmark payload');
    76	        if (received.replyToken !== null) {
    77	          echoHandler.handleL0(payload);
    78	          received.reply()
    79	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
    80	            .message(encodeBenchPayloadMessage(payload))
    81	            .submit();
    82	        } else {
    83	          commandHandler.handleL0(payload);
    84	        }
    85	      } catch (error) {
    86	        metrics.recordError();
    87	        console.error(`ladder L0 handler failed: ${error.message}`);
    88	      }
    89	    }
    90	    setImmediate(pump);

Binding reply excerpt `zlink-raw-server/main.js:77-90` and L0 reply excerpt `zlink-framework-server/main.js:74-86`:
    77	    try {
    78	      const part = bodyPart(requestReceived);
    79	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
    80	      if (body === null) throw new Error('invalid raw protobuf payload');
    81	      metrics.record(body);
    82	      const reply = encodeBenchPayloadMessage(body);
    83	      const operation = requestReceived.replyToken !== null
    84	        ? requestReceived.reply()
    85	        : requestReceived.send();
    86	      operation.message(RESPONSE_ENVELOPE).message(reply).submit();
    87	    } catch (error) {
    88	      metrics.recordError();
    89	      console.error(`raw request loop failed: ${error.message}`);
    90	    }
    72	      try {
    73	        const header = frameworkWire ? parts[0].data() : null;
    74	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
    75	        if (payload === null) throw new Error('invalid benchmark payload');
    76	        if (received.replyToken !== null) {
    77	          echoHandler.handleL0(payload);
    78	          received.reply()
    79	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
    80	            .message(encodeBenchPayloadMessage(payload))
    81	            .submit();
    82	        } else {
    83	          commandHandler.handleL0(payload);
    84	        }

Per message: both receive into one reusable `Received`, no wait, and drain up to 256 messages before `setImmediate`; both call the same ordinary JS metrics counters at `shared/bench-server-metrics.js:33-46` (0 atomics). Each request decodes the existing body, allocates one new protobuf-encoded body Buffer and one response operation/message, submits a new envelope plus body as two parts; no request parts are copied/reused. L0 adds no per-message work besides its direct `handleL0` call.

- Initial six-cell launcher was cancelled while queued for the exclusive measurement lock, before the bench ran and before any result file existed. Its lock request is not included in reported measurements. The receive loop then adopted the raw server idle policy; commit and build verification are repeated before measurement.

## Before l0e-six-cells
commit=48655c21ca22789ca21acc741e138ac2802b8feb dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
Node sources are executed directly from this worktree; framework build outputs and native binding hashes:
framework/languages/node/packages/framework/dist/runtime/channels/channel-dispatchers.js sha256=2eb493e44a2738fe318252cfd07fc0861ecadb6dc335f8104f0d6bcbb3a82f0f
framework/languages/node/packages/nestjs/dist/index.js sha256=4909eed4600b8222caa46a18bb55c05d9b05b7fdb1b22bdee311b91143d8cab8
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae
.artifacts/wsl/npm/zlink-systems-zlink-1.25.0.tgz sha256=f15954789c6e6e893f704e90dabfa1d9903d32d786370d23c5ef98f7e695e0d3

Binding receive excerpt `zlink-raw-server/main.js:64-90`:
    64	function pumpRequests() {
    65	  let handled = 0;
    66	  while (handled < batch) {
    67	    let hasMessage;
    68	    try {
    69	      hasMessage = tryRecv(requestRouter, requestReceived);
    70	    } catch (error) {
    71	      metrics.recordError();
    72	      console.error(`raw request loop recv failed: ${error.message}`);
    73	      break;
    74	    }
    75	    if (!hasMessage) break;
    76	    handled += 1;
    77	    try {
    78	      const part = bodyPart(requestReceived);
    79	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
    80	      if (body === null) throw new Error('invalid raw protobuf payload');
    81	      metrics.record(body);
    82	      const reply = encodeBenchPayloadMessage(body);
    83	      const operation = requestReceived.replyToken !== null
    84	        ? requestReceived.reply()
    85	        : requestReceived.send();
    86	      operation.message(RESPONSE_ENVELOPE).message(reply).submit();
    87	    } catch (error) {
    88	      metrics.recordError();
    89	      console.error(`raw request loop failed: ${error.message}`);
    90	    }
   121	// Idle policy. `setTimeout(pump, 0)` clamps to about a millisecond, which would
   122	// have put a poll delay into every `request-serial` round trip and reported the
   123	// harness's backoff as the stack's latency. So the loop stays hot on
   124	// setImmediate while a cell is running (traffic keeps `handled` non-zero, and
   125	// even an idle gap inside a serial cell is far shorter than the spin budget) and
   126	// only falls back to the timer once the socket has been quiet for a long stretch,
   127	// which happens between cells. setImmediate still yields to I/O callbacks, so
   128	// /bench/stats keeps answering during the drain.
   129	const IDLE_SPIN_LIMIT = 20000;
   130	let idleSpins = 0;
   131	
   132	function pump() {
   133	  const handled = pumpRequests() + pumpCommands();
   134	  if (handled > 0) {
   135	    idleSpins = 0;
   136	    setImmediate(pump);
   137	    return;
   138	  }
   139	  idleSpins += 1;
   140	  if (idleSpins < IDLE_SPIN_LIMIT) setImmediate(pump);
   141	  else setTimeout(pump, 1);
   142	}

L0 receive excerpt `zlink-framework-server/main.js:59-105`:
    59	  function pump() {
    60	    let handled = 0;
    61	    while (handled < batch) {
    62	      let hasMessage;
    63	      try {
    64	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    65	      } catch (error) {
    66	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
    67	        metrics.recordError();
    68	        console.error(`ladder L0 recv failed: ${error.message}`);
    69	        break;
    70	      }
    71	      if (!hasMessage) break;
    72	      handled += 1;
    73	      const parts = received.parts;
    74	      try {
    75	        const header = frameworkWire ? parts[0].data() : null;
    76	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
    77	        if (payload === null) throw new Error('invalid benchmark payload');
    78	        if (received.replyToken !== null) {
    79	          echoHandler.handleL0(payload);
    80	          received.reply()
    81	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
    82	            .message(encodeBenchPayloadMessage(payload))
    83	            .submit();
    84	        } else {
    85	          commandHandler.handleL0(payload);
    86	        }
    87	      } catch (error) {
    88	        metrics.recordError();
    89	        console.error(`ladder L0 handler failed: ${error.message}`);
    90	      }
    91	    }
    92	    return handled;
    93	  }
    94	
    95	  function schedulePump() {
    96	    const handled = pump();
    97	    if (handled > 0) {
    98	      idleSpins = 0;
    99	      setImmediate(schedulePump);
   100	      return;
   101	    }
   102	    idleSpins += 1;
   103	    if (idleSpins < IDLE_SPIN_LIMIT) setImmediate(schedulePump);
   104	    else setTimeout(schedulePump, 1);
   105	  }

Binding reply excerpt `zlink-raw-server/main.js:77-90` and L0 reply excerpt `zlink-framework-server/main.js:74-86`:
    77	    try {
    78	      const part = bodyPart(requestReceived);
    79	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
    80	      if (body === null) throw new Error('invalid raw protobuf payload');
    81	      metrics.record(body);
    82	      const reply = encodeBenchPayloadMessage(body);
    83	      const operation = requestReceived.replyToken !== null
    84	        ? requestReceived.reply()
    85	        : requestReceived.send();
    86	      operation.message(RESPONSE_ENVELOPE).message(reply).submit();
    87	    } catch (error) {
    88	      metrics.recordError();
    89	      console.error(`raw request loop failed: ${error.message}`);
    90	    }
    74	      try {
    75	        const header = frameworkWire ? parts[0].data() : null;
    76	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
    77	        if (payload === null) throw new Error('invalid benchmark payload');
    78	        if (received.replyToken !== null) {
    79	          echoHandler.handleL0(payload);
    80	          received.reply()
    81	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
    82	            .message(encodeBenchPayloadMessage(payload))
    83	            .submit();
    84	        } else {
    85	          commandHandler.handleL0(payload);
    86	        }

Per message: both receive into one reusable `Received`, no wait, and drain up to 256 messages before `setImmediate`; both call the same ordinary JS metrics counters at `shared/bench-server-metrics.js:33-46` (0 atomics). Each request decodes the existing body, allocates one new protobuf-encoded body Buffer and one response operation/message, submits a new envelope plus body as two parts; no request parts are copied/reused. L0 adds no per-message work besides its direct `handleL0` call in the measured raw-wire cells.

First result: binding send-saturation 1K run1 = 116554.4 msg/s; errors=0 and submitted=completed=received=582772. Raw: raw/six-cells/bench/node/run1/zlink-node-send-saturation-1024/results.json.
L0 first result: send-saturation 1K run1 = 119421 msg/s; errors=0 and submitted=completed=received=603326. Raw: raw/six-cells/bench/node/run1/zlink-framework-node-send-saturation-1024/results.json.
Run1 request-serial 1K: binding 9297 msg/s and L0 9758.6 msg/s (1.0497x); both errors=0 and counts agree. Raw: raw/six-cells/bench/node/run1/zlink-node-request-serial-1024/results.json and raw/six-cells/bench/node/run1/zlink-framework-node-request-serial-1024/results.json.

## Before profile-request-serial-4k
commit=48655c21ca22789ca21acc741e138ac2802b8feb dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/languages/node/packages/framework/dist/runtime/channels/channel-dispatchers.js sha256=2eb493e44a2738fe318252cfd07fc0861ecadb6dc335f8104f0d6bcbb3a82f0f
framework/languages/node/packages/nestjs/dist/index.js sha256=4909eed4600b8222caa46a18bb55c05d9b05b7fdb1b22bdee311b91143d8cab8
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae
.artifacts/wsl/npm/zlink-systems-zlink-1.25.0.tgz sha256=f15954789c6e6e893f704e90dabfa1d9903d32d786370d23c5ef98f7e695e0d3

Binding receive and idle excerpts `zlink-raw-server/main.js:64-90,121-142`:
    64	function pumpRequests() {
    65	  let handled = 0;
    66	  while (handled < batch) {
    67	    let hasMessage;
    68	    try {
    69	      hasMessage = tryRecv(requestRouter, requestReceived);
    70	    } catch (error) {
    71	      metrics.recordError();
    72	      console.error(`raw request loop recv failed: ${error.message}`);
    73	      break;
    74	    }
    75	    if (!hasMessage) break;
    76	    handled += 1;
    77	    try {
    78	      const part = bodyPart(requestReceived);
    79	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
    80	      if (body === null) throw new Error('invalid raw protobuf payload');
    81	      metrics.record(body);
    82	      const reply = encodeBenchPayloadMessage(body);
    83	      const operation = requestReceived.replyToken !== null
    84	        ? requestReceived.reply()
    85	        : requestReceived.send();
    86	      operation.message(RESPONSE_ENVELOPE).message(reply).submit();
    87	    } catch (error) {
    88	      metrics.recordError();
    89	      console.error(`raw request loop failed: ${error.message}`);
    90	    }
   121	// Idle policy. `setTimeout(pump, 0)` clamps to about a millisecond, which would
   122	// have put a poll delay into every `request-serial` round trip and reported the
   123	// harness's backoff as the stack's latency. So the loop stays hot on
   124	// setImmediate while a cell is running (traffic keeps `handled` non-zero, and
   125	// even an idle gap inside a serial cell is far shorter than the spin budget) and
   126	// only falls back to the timer once the socket has been quiet for a long stretch,
   127	// which happens between cells. setImmediate still yields to I/O callbacks, so
   128	// /bench/stats keeps answering during the drain.
   129	const IDLE_SPIN_LIMIT = 20000;
   130	let idleSpins = 0;
   131	
   132	function pump() {
   133	  const handled = pumpRequests() + pumpCommands();
   134	  if (handled > 0) {
   135	    idleSpins = 0;
   136	    setImmediate(pump);
   137	    return;
   138	  }
   139	  idleSpins += 1;
   140	  if (idleSpins < IDLE_SPIN_LIMIT) setImmediate(pump);
   141	  else setTimeout(pump, 1);
   142	}

L0 receive and idle excerpt `zlink-framework-server/main.js:59-105`:
    59	  function pump() {
    60	    let handled = 0;
    61	    while (handled < batch) {
    62	      let hasMessage;
    63	      try {
    64	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    65	      } catch (error) {
    66	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
    67	        metrics.recordError();
    68	        console.error(`ladder L0 recv failed: ${error.message}`);
    69	        break;
    70	      }
    71	      if (!hasMessage) break;
    72	      handled += 1;
    73	      const parts = received.parts;
    74	      try {
    75	        const header = frameworkWire ? parts[0].data() : null;
    76	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
    77	        if (payload === null) throw new Error('invalid benchmark payload');
    78	        if (received.replyToken !== null) {
    79	          echoHandler.handleL0(payload);
    80	          received.reply()
    81	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
    82	            .message(encodeBenchPayloadMessage(payload))
    83	            .submit();
    84	        } else {
    85	          commandHandler.handleL0(payload);
    86	        }
    87	      } catch (error) {
    88	        metrics.recordError();
    89	        console.error(`ladder L0 handler failed: ${error.message}`);
    90	      }
    91	    }
    92	    return handled;
    93	  }
    94	
    95	  function schedulePump() {
    96	    const handled = pump();
    97	    if (handled > 0) {
    98	      idleSpins = 0;
    99	      setImmediate(schedulePump);
   100	      return;
   101	    }
   102	    idleSpins += 1;
   103	    if (idleSpins < IDLE_SPIN_LIMIT) setImmediate(schedulePump);
   104	    else setTimeout(schedulePump, 1);
   105	  }

Binding and L0 reply excerpts `zlink-raw-server/main.js:77-90`; `zlink-framework-server/main.js:74-86`:
    77	    try {
    78	      const part = bodyPart(requestReceived);
    79	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
    80	      if (body === null) throw new Error('invalid raw protobuf payload');
    81	      metrics.record(body);
    82	      const reply = encodeBenchPayloadMessage(body);
    83	      const operation = requestReceived.replyToken !== null
    84	        ? requestReceived.reply()
    85	        : requestReceived.send();
    86	      operation.message(RESPONSE_ENVELOPE).message(reply).submit();
    87	    } catch (error) {
    88	      metrics.recordError();
    89	      console.error(`raw request loop failed: ${error.message}`);
    90	    }
    74	      try {
    75	        const header = frameworkWire ? parts[0].data() : null;
    76	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
    77	        if (payload === null) throw new Error('invalid benchmark payload');
    78	        if (received.replyToken !== null) {
    79	          echoHandler.handleL0(payload);
    80	          received.reply()
    81	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
    82	            .message(encodeBenchPayloadMessage(payload))
    83	            .submit();
    84	        } else {
    85	          commandHandler.handleL0(payload);
    86	        }

Per message: no waits or atomics; batch limit 256; same BenchServerMetrics counters; one decoded body, one new protobuf body Buffer, new response operation, fixed envelope and two-part submit. L0 has one direct handleL0 call. This is a profiled diagnostic run; its throughput is not used in the six-cell comparison.

## Before l0e-cross-raw-raw
commit=48655c21ca22789ca21acc741e138ac2802b8feb dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/languages/node/packages/framework/dist/runtime/channels/channel-dispatchers.js sha256=2eb493e44a2738fe318252cfd07fc0861ecadb6dc335f8104f0d6bcbb3a82f0f
framework/languages/node/packages/nestjs/dist/index.js sha256=4909eed4600b8222caa46a18bb55c05d9b05b7fdb1b22bdee311b91143d8cab8
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae
.artifacts/wsl/npm/zlink-systems-zlink-1.25.0.tgz sha256=f15954789c6e6e893f704e90dabfa1d9903d32d786370d23c5ef98f7e695e0d3

Binding receive `zlink-raw-server/main.js:64-90` and L0 receive `zlink-framework-server/main.js:59-105` excerpts:
    64	function pumpRequests() {
    65	  let handled = 0;
    66	  while (handled < batch) {
    67	    let hasMessage;
    68	    try {
    69	      hasMessage = tryRecv(requestRouter, requestReceived);
    70	    } catch (error) {
    71	      metrics.recordError();
    72	      console.error(`raw request loop recv failed: ${error.message}`);
    73	      break;
    74	    }
    75	    if (!hasMessage) break;
    76	    handled += 1;
    77	    try {
    78	      const part = bodyPart(requestReceived);
    79	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
    80	      if (body === null) throw new Error('invalid raw protobuf payload');
    81	      metrics.record(body);
    82	      const reply = encodeBenchPayloadMessage(body);
    83	      const operation = requestReceived.replyToken !== null
    84	        ? requestReceived.reply()
    85	        : requestReceived.send();
    86	      operation.message(RESPONSE_ENVELOPE).message(reply).submit();
    87	    } catch (error) {
    88	      metrics.recordError();
    89	      console.error(`raw request loop failed: ${error.message}`);
    90	    }
   121	// Idle policy. `setTimeout(pump, 0)` clamps to about a millisecond, which would
   122	// have put a poll delay into every `request-serial` round trip and reported the
   123	// harness's backoff as the stack's latency. So the loop stays hot on
   124	// setImmediate while a cell is running (traffic keeps `handled` non-zero, and
   125	// even an idle gap inside a serial cell is far shorter than the spin budget) and
   126	// only falls back to the timer once the socket has been quiet for a long stretch,
   127	// which happens between cells. setImmediate still yields to I/O callbacks, so
   128	// /bench/stats keeps answering during the drain.
   129	const IDLE_SPIN_LIMIT = 20000;
   130	let idleSpins = 0;
   131	
   132	function pump() {
   133	  const handled = pumpRequests() + pumpCommands();
   134	  if (handled > 0) {
   135	    idleSpins = 0;
   136	    setImmediate(pump);
   137	    return;
   138	  }
   139	  idleSpins += 1;
   140	  if (idleSpins < IDLE_SPIN_LIMIT) setImmediate(pump);
   141	  else setTimeout(pump, 1);
   142	}
    59	  function pump() {
    60	    let handled = 0;
    61	    while (handled < batch) {
    62	      let hasMessage;
    63	      try {
    64	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    65	      } catch (error) {
    66	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
    67	        metrics.recordError();
    68	        console.error(`ladder L0 recv failed: ${error.message}`);
    69	        break;
    70	      }
    71	      if (!hasMessage) break;
    72	      handled += 1;
    73	      const parts = received.parts;
    74	      try {
    75	        const header = frameworkWire ? parts[0].data() : null;
    76	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
    77	        if (payload === null) throw new Error('invalid benchmark payload');
    78	        if (received.replyToken !== null) {
    79	          echoHandler.handleL0(payload);
    80	          received.reply()
    81	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
    82	            .message(encodeBenchPayloadMessage(payload))
    83	            .submit();
    84	        } else {
    85	          commandHandler.handleL0(payload);
    86	        }
    87	      } catch (error) {
    88	        metrics.recordError();
    89	        console.error(`ladder L0 handler failed: ${error.message}`);
    90	      }
    91	    }
    92	    return handled;
    93	  }
    94	
    95	  function schedulePump() {
    96	    const handled = pump();
    97	    if (handled > 0) {
    98	      idleSpins = 0;
    99	      setImmediate(schedulePump);
   100	      return;
   101	    }
   102	    idleSpins += 1;
   103	    if (idleSpins < IDLE_SPIN_LIMIT) setImmediate(schedulePump);
   104	    else setTimeout(schedulePump, 1);
   105	  }

Binding reply `zlink-raw-server/main.js:77-90` and L0 reply `zlink-framework-server/main.js:74-86` excerpts:
    77	    try {
    78	      const part = bodyPart(requestReceived);
    79	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
    80	      if (body === null) throw new Error('invalid raw protobuf payload');
    81	      metrics.record(body);
    82	      const reply = encodeBenchPayloadMessage(body);
    83	      const operation = requestReceived.replyToken !== null
    84	        ? requestReceived.reply()
    85	        : requestReceived.send();
    86	      operation.message(RESPONSE_ENVELOPE).message(reply).submit();
    87	    } catch (error) {
    88	      metrics.recordError();
    89	      console.error(`raw request loop failed: ${error.message}`);
    90	    }
    74	      try {
    75	        const header = frameworkWire ? parts[0].data() : null;
    76	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
    77	        if (payload === null) throw new Error('invalid benchmark payload');
    78	        if (received.replyToken !== null) {
    79	          echoHandler.handleL0(payload);
    80	          received.reply()
    81	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
    82	            .message(encodeBenchPayloadMessage(payload))
    83	            .submit();
    84	        } else {
    85	          commandHandler.handleL0(payload);
    86	        }
    32	  /** `payload` is the stamped body, not the protobuf envelope. */
    33	  record(payload) {
    34	    this._allPhaseMessages += 1;
    35	    const decoded = header.decode(payload);
    36	    if (decoded === null || decoded.phase !== header.PHASE_ACTIVE) return;
    37	    this._activeMessages += 1;
    38	    if (this._latencyNs.length < this._latencyLimit) {
    39	      const latency = header.nowNs() - decoded.sentTimestampNs;
    40	      this._latencyNs.push(latency > 0n ? Number(latency) : 0);
    41	    }
    42	  }
    43	
    44	  recordError() {
    45	    this._errors += 1;
    46	  }

Per message: no waits; ordinary JS counters in `shared/bench-server-metrics.js:33-46` and no atomics; drain max 256; decode one body, allocate one new protobuf body, create a new response operation and submit a new envelope plus body as two parts. No request parts are copied/reused.

## Before l0e-cross-L0-raw-client
commit=48655c21ca22789ca21acc741e138ac2802b8feb dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/languages/node/packages/framework/dist/runtime/channels/channel-dispatchers.js sha256=2eb493e44a2738fe318252cfd07fc0861ecadb6dc335f8104f0d6bcbb3a82f0f
framework/languages/node/packages/nestjs/dist/index.js sha256=4909eed4600b8222caa46a18bb55c05d9b05b7fdb1b22bdee311b91143d8cab8
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae
.artifacts/wsl/npm/zlink-systems-zlink-1.25.0.tgz sha256=f15954789c6e6e893f704e90dabfa1d9903d32d786370d23c5ef98f7e695e0d3

Binding receive `zlink-raw-server/main.js:64-90` and L0 receive `zlink-framework-server/main.js:59-105` excerpts:
    64	function pumpRequests() {
    65	  let handled = 0;
    66	  while (handled < batch) {
    67	    let hasMessage;
    68	    try {
    69	      hasMessage = tryRecv(requestRouter, requestReceived);
    70	    } catch (error) {
    71	      metrics.recordError();
    72	      console.error(`raw request loop recv failed: ${error.message}`);
    73	      break;
    74	    }
    75	    if (!hasMessage) break;
    76	    handled += 1;
    77	    try {
    78	      const part = bodyPart(requestReceived);
    79	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
    80	      if (body === null) throw new Error('invalid raw protobuf payload');
    81	      metrics.record(body);
    82	      const reply = encodeBenchPayloadMessage(body);
    83	      const operation = requestReceived.replyToken !== null
    84	        ? requestReceived.reply()
    85	        : requestReceived.send();
    86	      operation.message(RESPONSE_ENVELOPE).message(reply).submit();
    87	    } catch (error) {
    88	      metrics.recordError();
    89	      console.error(`raw request loop failed: ${error.message}`);
    90	    }
   121	// Idle policy. `setTimeout(pump, 0)` clamps to about a millisecond, which would
   122	// have put a poll delay into every `request-serial` round trip and reported the
   123	// harness's backoff as the stack's latency. So the loop stays hot on
   124	// setImmediate while a cell is running (traffic keeps `handled` non-zero, and
   125	// even an idle gap inside a serial cell is far shorter than the spin budget) and
   126	// only falls back to the timer once the socket has been quiet for a long stretch,
   127	// which happens between cells. setImmediate still yields to I/O callbacks, so
   128	// /bench/stats keeps answering during the drain.
   129	const IDLE_SPIN_LIMIT = 20000;
   130	let idleSpins = 0;
   131	
   132	function pump() {
   133	  const handled = pumpRequests() + pumpCommands();
   134	  if (handled > 0) {
   135	    idleSpins = 0;
   136	    setImmediate(pump);
   137	    return;
   138	  }
   139	  idleSpins += 1;
   140	  if (idleSpins < IDLE_SPIN_LIMIT) setImmediate(pump);
   141	  else setTimeout(pump, 1);
   142	}
    59	  function pump() {
    60	    let handled = 0;
    61	    while (handled < batch) {
    62	      let hasMessage;
    63	      try {
    64	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    65	      } catch (error) {
    66	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
    67	        metrics.recordError();
    68	        console.error(`ladder L0 recv failed: ${error.message}`);
    69	        break;
    70	      }
    71	      if (!hasMessage) break;
    72	      handled += 1;
    73	      const parts = received.parts;
    74	      try {
    75	        const header = frameworkWire ? parts[0].data() : null;
    76	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
    77	        if (payload === null) throw new Error('invalid benchmark payload');
    78	        if (received.replyToken !== null) {
    79	          echoHandler.handleL0(payload);
    80	          received.reply()
    81	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
    82	            .message(encodeBenchPayloadMessage(payload))
    83	            .submit();
    84	        } else {
    85	          commandHandler.handleL0(payload);
    86	        }
    87	      } catch (error) {
    88	        metrics.recordError();
    89	        console.error(`ladder L0 handler failed: ${error.message}`);
    90	      }
    91	    }
    92	    return handled;
    93	  }
    94	
    95	  function schedulePump() {
    96	    const handled = pump();
    97	    if (handled > 0) {
    98	      idleSpins = 0;
    99	      setImmediate(schedulePump);
   100	      return;
   101	    }
   102	    idleSpins += 1;
   103	    if (idleSpins < IDLE_SPIN_LIMIT) setImmediate(schedulePump);
   104	    else setTimeout(schedulePump, 1);
   105	  }

Binding reply `zlink-raw-server/main.js:77-90` and L0 reply `zlink-framework-server/main.js:74-86` excerpts:
    77	    try {
    78	      const part = bodyPart(requestReceived);
    79	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
    80	      if (body === null) throw new Error('invalid raw protobuf payload');
    81	      metrics.record(body);
    82	      const reply = encodeBenchPayloadMessage(body);
    83	      const operation = requestReceived.replyToken !== null
    84	        ? requestReceived.reply()
    85	        : requestReceived.send();
    86	      operation.message(RESPONSE_ENVELOPE).message(reply).submit();
    87	    } catch (error) {
    88	      metrics.recordError();
    89	      console.error(`raw request loop failed: ${error.message}`);
    90	    }
    74	      try {
    75	        const header = frameworkWire ? parts[0].data() : null;
    76	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
    77	        if (payload === null) throw new Error('invalid benchmark payload');
    78	        if (received.replyToken !== null) {
    79	          echoHandler.handleL0(payload);
    80	          received.reply()
    81	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
    82	            .message(encodeBenchPayloadMessage(payload))
    83	            .submit();
    84	        } else {
    85	          commandHandler.handleL0(payload);
    86	        }
    32	  /** `payload` is the stamped body, not the protobuf envelope. */
    33	  record(payload) {
    34	    this._allPhaseMessages += 1;
    35	    const decoded = header.decode(payload);
    36	    if (decoded === null || decoded.phase !== header.PHASE_ACTIVE) return;
    37	    this._activeMessages += 1;
    38	    if (this._latencyNs.length < this._latencyLimit) {
    39	      const latency = header.nowNs() - decoded.sentTimestampNs;
    40	      this._latencyNs.push(latency > 0n ? Number(latency) : 0);
    41	    }
    42	  }
    43	
    44	  recordError() {
    45	    this._errors += 1;
    46	  }

Per message: no waits; ordinary JS counters in `shared/bench-server-metrics.js:33-46` and no atomics; drain max 256; decode one body, allocate one new protobuf body, create a new response operation and submit a new envelope plus body as two parts. No request parts are copied/reused.

## Before l0e-cross-L0-client
commit=48655c21ca22789ca21acc741e138ac2802b8feb dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/languages/node/packages/framework/dist/runtime/channels/channel-dispatchers.js sha256=2eb493e44a2738fe318252cfd07fc0861ecadb6dc335f8104f0d6bcbb3a82f0f
framework/languages/node/packages/nestjs/dist/index.js sha256=4909eed4600b8222caa46a18bb55c05d9b05b7fdb1b22bdee311b91143d8cab8
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae
.artifacts/wsl/npm/zlink-systems-zlink-1.25.0.tgz sha256=f15954789c6e6e893f704e90dabfa1d9903d32d786370d23c5ef98f7e695e0d3

Binding receive `zlink-raw-server/main.js:64-90` and L0 receive `zlink-framework-server/main.js:59-105` excerpts:
    64	function pumpRequests() {
    65	  let handled = 0;
    66	  while (handled < batch) {
    67	    let hasMessage;
    68	    try {
    69	      hasMessage = tryRecv(requestRouter, requestReceived);
    70	    } catch (error) {
    71	      metrics.recordError();
    72	      console.error(`raw request loop recv failed: ${error.message}`);
    73	      break;
    74	    }
    75	    if (!hasMessage) break;
    76	    handled += 1;
    77	    try {
    78	      const part = bodyPart(requestReceived);
    79	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
    80	      if (body === null) throw new Error('invalid raw protobuf payload');
    81	      metrics.record(body);
    82	      const reply = encodeBenchPayloadMessage(body);
    83	      const operation = requestReceived.replyToken !== null
    84	        ? requestReceived.reply()
    85	        : requestReceived.send();
    86	      operation.message(RESPONSE_ENVELOPE).message(reply).submit();
    87	    } catch (error) {
    88	      metrics.recordError();
    89	      console.error(`raw request loop failed: ${error.message}`);
    90	    }
   121	// Idle policy. `setTimeout(pump, 0)` clamps to about a millisecond, which would
   122	// have put a poll delay into every `request-serial` round trip and reported the
   123	// harness's backoff as the stack's latency. So the loop stays hot on
   124	// setImmediate while a cell is running (traffic keeps `handled` non-zero, and
   125	// even an idle gap inside a serial cell is far shorter than the spin budget) and
   126	// only falls back to the timer once the socket has been quiet for a long stretch,
   127	// which happens between cells. setImmediate still yields to I/O callbacks, so
   128	// /bench/stats keeps answering during the drain.
   129	const IDLE_SPIN_LIMIT = 20000;
   130	let idleSpins = 0;
   131	
   132	function pump() {
   133	  const handled = pumpRequests() + pumpCommands();
   134	  if (handled > 0) {
   135	    idleSpins = 0;
   136	    setImmediate(pump);
   137	    return;
   138	  }
   139	  idleSpins += 1;
   140	  if (idleSpins < IDLE_SPIN_LIMIT) setImmediate(pump);
   141	  else setTimeout(pump, 1);
   142	}
    59	  function pump() {
    60	    let handled = 0;
    61	    while (handled < batch) {
    62	      let hasMessage;
    63	      try {
    64	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    65	      } catch (error) {
    66	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
    67	        metrics.recordError();
    68	        console.error(`ladder L0 recv failed: ${error.message}`);
    69	        break;
    70	      }
    71	      if (!hasMessage) break;
    72	      handled += 1;
    73	      const parts = received.parts;
    74	      try {
    75	        const header = frameworkWire ? parts[0].data() : null;
    76	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
    77	        if (payload === null) throw new Error('invalid benchmark payload');
    78	        if (received.replyToken !== null) {
    79	          echoHandler.handleL0(payload);
    80	          received.reply()
    81	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
    82	            .message(encodeBenchPayloadMessage(payload))
    83	            .submit();
    84	        } else {
    85	          commandHandler.handleL0(payload);
    86	        }
    87	      } catch (error) {
    88	        metrics.recordError();
    89	        console.error(`ladder L0 handler failed: ${error.message}`);
    90	      }
    91	    }
    92	    return handled;
    93	  }
    94	
    95	  function schedulePump() {
    96	    const handled = pump();
    97	    if (handled > 0) {
    98	      idleSpins = 0;
    99	      setImmediate(schedulePump);
   100	      return;
   101	    }
   102	    idleSpins += 1;
   103	    if (idleSpins < IDLE_SPIN_LIMIT) setImmediate(schedulePump);
   104	    else setTimeout(schedulePump, 1);
   105	  }

Binding reply `zlink-raw-server/main.js:77-90` and L0 reply `zlink-framework-server/main.js:74-86` excerpts:
    77	    try {
    78	      const part = bodyPart(requestReceived);
    79	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
    80	      if (body === null) throw new Error('invalid raw protobuf payload');
    81	      metrics.record(body);
    82	      const reply = encodeBenchPayloadMessage(body);
    83	      const operation = requestReceived.replyToken !== null
    84	        ? requestReceived.reply()
    85	        : requestReceived.send();
    86	      operation.message(RESPONSE_ENVELOPE).message(reply).submit();
    87	    } catch (error) {
    88	      metrics.recordError();
    89	      console.error(`raw request loop failed: ${error.message}`);
    90	    }
    74	      try {
    75	        const header = frameworkWire ? parts[0].data() : null;
    76	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
    77	        if (payload === null) throw new Error('invalid benchmark payload');
    78	        if (received.replyToken !== null) {
    79	          echoHandler.handleL0(payload);
    80	          received.reply()
    81	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
    82	            .message(encodeBenchPayloadMessage(payload))
    83	            .submit();
    84	        } else {
    85	          commandHandler.handleL0(payload);
    86	        }
    32	  /** `payload` is the stamped body, not the protobuf envelope. */
    33	  record(payload) {
    34	    this._allPhaseMessages += 1;
    35	    const decoded = header.decode(payload);
    36	    if (decoded === null || decoded.phase !== header.PHASE_ACTIVE) return;
    37	    this._activeMessages += 1;
    38	    if (this._latencyNs.length < this._latencyLimit) {
    39	      const latency = header.nowNs() - decoded.sentTimestampNs;
    40	      this._latencyNs.push(latency > 0n ? Number(latency) : 0);
    41	    }
    42	  }
    43	
    44	  recordError() {
    45	    this._errors += 1;
    46	  }

Per message: no waits; ordinary JS counters in `shared/bench-server-metrics.js:33-46` and no atomics; drain max 256; decode one body, allocate one new protobuf body, create a new response operation and submit a new envelope plus body as two parts. No request parts are copied/reused.

## Before l0e-cross-L0-raw-client-G5-repeat2
commit=48655c21ca22789ca21acc741e138ac2802b8feb dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
L0 server: zlink-framework-node; client: zlink-node; send-saturation 1K; repeats 4–5.
Receive/pump excerpt `zlink-framework-server/main.js:59-105`:
    59	  function pump() {
    60	    let handled = 0;
    61	    while (handled < batch) {
    62	      let hasMessage;
    63	      try {
    64	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    65	      } catch (error) {
    66	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
    67	        metrics.recordError();
    68	        console.error(`ladder L0 recv failed: ${error.message}`);
    69	        break;
    70	      }
    71	      if (!hasMessage) break;
    72	      handled += 1;
    73	      const parts = received.parts;
    74	      try {
    75	        const header = frameworkWire ? parts[0].data() : null;
    76	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
    77	        if (payload === null) throw new Error('invalid benchmark payload');
    78	        if (received.replyToken !== null) {
    79	          echoHandler.handleL0(payload);
    80	          received.reply()
    81	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
    82	            .message(encodeBenchPayloadMessage(payload))
    83	            .submit();
    84	        } else {
    85	          commandHandler.handleL0(payload);
    86	        }
    87	      } catch (error) {
    88	        metrics.recordError();
    89	        console.error(`ladder L0 handler failed: ${error.message}`);
    90	      }
    91	    }
    92	    return handled;
    93	  }
    94	
    95	  function schedulePump() {
    96	    const handled = pump();
    97	    if (handled > 0) {
    98	      idleSpins = 0;
    99	      setImmediate(schedulePump);
   100	      return;
   101	    }
   102	    idleSpins += 1;
   103	    if (idleSpins < IDLE_SPIN_LIMIT) setImmediate(schedulePump);
   104	    else setTimeout(schedulePump, 1);
   105	  }
Metrics excerpt `shared/bench-server-metrics.js:33-46`:
    33	  record(payload) {
    34	    this._allPhaseMessages += 1;
    35	    const decoded = header.decode(payload);
    36	    if (decoded === null || decoded.phase !== header.PHASE_ACTIVE) return;
    37	    this._activeMessages += 1;
    38	    if (this._latencyNs.length < this._latencyLimit) {
    39	      const latency = header.nowNs() - decoded.sentTimestampNs;
    40	      this._latencyNs.push(latency > 0n ? Number(latency) : 0);
    41	    }
    42	  }
    43	
    44	  recordError() {
    45	    this._errors += 1;
    46	  }
## Cross measurement final

- send-saturation 1K raw/raw: 3 runs, median 118762.0 msg/s, G5 spread 8.38% pass.
- send-saturation 1K Framework L0 server/raw client: 3-run spread 17.85% triggered the common 5-run rule; after two extra runs the five-run median is 118945.8 msg/s, spread 15.19% still fails G5.
- send-saturation 1K Framework L0 server/Framework L0 client: 3 runs, median 113787.0 msg/s, G5 spread 6.35% pass; relative to raw/raw it is 0.958.
- All cross result files have errors=0, server errors=0, and submitted=completed=target received. The repeat run used its own exclusive lock window and is in `raw/lock-windows/l0e-cross-L0-raw-client-G5-repeat2-{before,after}.txt`.
