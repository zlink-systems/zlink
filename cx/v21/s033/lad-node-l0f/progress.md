# Progress

## Before paired CPU profiles
commit=48655c21ca22789ca21acc741e138ac2802b8feb dirty=true base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970

HEAD=48655c21ca22789ca21acc741e138ac2802b8feb dirty=true base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=fe35d09d5d7ea83c65bf86af46d10ecd65ed1d80f8b62c99530fef54e4abafa8
framework/languages/node/packages/framework/dist/runtime/channels/channel-dispatchers.js sha256=2eb493e44a2738fe318252cfd07fc0861ecadb6dc335f8104f0d6bcbb3a82f0f
framework/languages/node/packages/framework-codec-protobuf/dist/server/framework.cjs sha256=942e942e8e04fe89f7e63d117ba6f0a96b16e82fa0e26309344e8a119b799e53
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae
.artifacts/wsl/npm/zlink-systems-zlink-1.25.0.tgz sha256=f15954789c6e6e893f704e90dabfa1d9903d32d786370d23c5ef98f7e695e0d3

Binding receive/reply `zlink-raw-server/main.js:64-90,121-142`:
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

L0 receive/reply `zlink-framework-server/main.js:64-90,119-132`:
    64	  const IDLE_SPIN_LIMIT = 20000;
    65	  let idleSpins = 0;
    66	
    67	  function frameworkResponseHeader(bytes) {
    68	    const header = JSON.parse(bytes.subarray(1).toString('utf8'));
    69	    header.kind = 2;
    70	    return Buffer.concat([Buffer.from([0xf2]), Buffer.from(JSON.stringify(header), 'utf8')]);
    71	  }
    72	
    73	  function pump() {
    74	    let handled = 0;
    75	    while (handled < batch) {
    76	      let hasMessage;
    77	      try {
    78	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    79	      } catch (error) {
    80	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
    81	        metrics.recordError();
    82	        console.error(`ladder L0 recv failed: ${error.message}`);
    83	        break;
    84	      }
    85	      if (!hasMessage) break;
    86	      handled += 1;
    87	      const parts = received.parts;
    88	      try {
    89	        const header = frameworkWire ? parts[0].data() : null;
    90	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   119	        console.error(`ladder L0 command recv failed: ${error.message}`);
   120	        break;
   121	      }
   122	      if (!hasMessage) break;
   123	      handled += 1;
   124	      try {
   125	        const parts = commandReceived.parts;
   126	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   127	        if (payload === null) throw new Error('invalid benchmark payload');
   128	        commandHandler.handleL0(payload);
   129	      } catch (error) {
   130	        metrics.recordError();
   131	        console.error(`ladder L0 command handler failed: ${error.message}`);
   132	      }

Per message: both reuse one Received; no blocking wait, atomics, or request-part copy; both drain up to 256 then setImmediate; both decode, record ordinary JS metrics, encode a fresh protobuf body and submit a new envelope plus body. Current L0 directly calls handleL0. Request-serial has no command frames.

## Paired profile retry pre-measure record
HEAD=48655c21ca22789ca21acc741e138ac2802b8feb dirty=true base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=db56855d7650f90ce06d12bf99d55bc5119efca885916568dfcb8a2e652b584b
framework/languages/node/packages/framework/dist/runtime/channels/channel-dispatchers.js sha256=2eb493e44a2738fe318252cfd07fc0861ecadb6dc335f8104f0d6bcbb3a82f0f
framework/languages/node/packages/framework-codec-protobuf/dist/server/framework.cjs sha256=942e942e8e04fe89f7e63d117ba6f0a96b16e82fa0e26309344e8a119b799e53
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae
.artifacts/wsl/npm/zlink-systems-zlink-1.25.0.tgz sha256=f15954789c6e6e893f704e90dabfa1d9903d32d786370d23c5ef98f7e695e0d3

Binding receive: `zlink-raw-server/main.js:64-90,121-142`
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

L0 receive: `zlink-framework-server/main.js:73-99,103-110`
    73	  function pump() {
    74	    let handled = 0;
    75	    while (handled < batch) {
    76	      let hasMessage;
    77	      try {
    78	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    79	      } catch (error) {
    80	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
    81	        metrics.recordError();
    82	        console.error(`ladder L0 recv failed: ${error.message}`);
    83	        break;
    84	      }
    85	      if (!hasMessage) break;
    86	      handled += 1;
    87	      const parts = received.parts;
    88	      try {
    89	        const header = frameworkWire ? parts[0].data() : null;
    90	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
    91	        if (payload === null) throw new Error('invalid benchmark payload');
    92	        if (received.replyToken !== null) {
    93	          echoHandler.handleL0(payload);
    94	          received.reply()
    95	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
    96	            .message(encodeBenchPayloadMessage(payload))
    97	            .submit();
    98	        } else {
    99	          commandHandler.handleL0(payload);
   103	        console.error(`ladder L0 handler failed: ${error.message}`);
   104	      }
   105	    }
   106	    return handled;
   107	  }
   108	
   109	  function pumpCommands() {
   110	    if (commandRouter === null) return 0;

Binding reply `zlink-raw-server/main.js:77-90`; L0 reply `zlink-framework-server/main.js:89-99`: both make a new response operation and protobuf body, then submit a new two-part envelope; neither reuses request parts. Both use one reusable Received for this request-only cell, no blocking wait, atomics, or copy, drain batch=256 and yield with setImmediate; L0 calls handleL0 directly.

## Before paired CPU profiles
HEAD=48655c21ca22789ca21acc741e138ac2802b8feb dirty=true base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=db56855d7650f90ce06d12bf99d55bc5119efca885916568dfcb8a2e652b584b
framework/bench/grpc/node/run_local.sh sha256=6bf323f1f0de49e45bb92dd193e1f222083b14e41199f517cfb04c3ff2cb3a74
framework/languages/node/packages/framework/dist/runtime/channels/channel-dispatchers.js sha256=2eb493e44a2738fe318252cfd07fc0861ecadb6dc335f8104f0d6bcbb3a82f0f
framework/languages/node/packages/framework-codec-protobuf/dist/server/framework.cjs sha256=942e942e8e04fe89f7e63d117ba6f0a96b16e82fa0e26309344e8a119b799e53
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae
.artifacts/wsl/npm/zlink-systems-zlink-1.25.0.tgz sha256=f15954789c6e6e893f704e90dabfa1d9903d32d786370d23c5ef98f7e695e0d3

Binding receive `zlink-raw-server/main.js:64-90,121-142`:
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

Current L0 receive `zlink-framework-server/main.js:73-101`:
    73	  function pump() {
    74	    let handled = 0;
    75	    while (handled < batch) {
    76	      let hasMessage;
    77	      try {
    78	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    79	      } catch (error) {
    80	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
    81	        metrics.recordError();
    82	        console.error(`ladder L0 recv failed: ${error.message}`);
    83	        break;
    84	      }
    85	      if (!hasMessage) break;
    86	      handled += 1;
    87	      const parts = received.parts;
    88	      try {
    89	        const header = frameworkWire ? parts[0].data() : null;
    90	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
    91	        if (payload === null) throw new Error('invalid benchmark payload');
    92	        if (received.replyToken !== null) {
    93	          echoHandler.handleL0(payload);
    94	          received.reply()
    95	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
    96	            .message(encodeBenchPayloadMessage(payload))
    97	            .submit();
    98	        } else {
    99	          commandHandler.handleL0(payload);
   100	        }
   101	      } catch (error) {

Binding reply `zlink-raw-server/main.js:77-90`; current L0 reply `zlink-framework-server/main.js:89-99`: both decode the existing body, record ordinary JS counters, encode a new protobuf body, create a new reply operation and submit two parts. Both use one reusable Received for request-serial, no blocking wait, atomics, or request-part copy, batch 256 and setImmediate. L0 directly calls handleL0. Diagnostic switches are OFF.

## Before paired CPU profiles (final source)
HEAD=48655c21ca22789ca21acc741e138ac2802b8feb dirty=true base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=800c3e9bd884d4e73c57bd3cafbc8a706c39b5de248c9505a07839101643e3fb
framework/bench/grpc/node/run_local.sh sha256=6bf323f1f0de49e45bb92dd193e1f222083b14e41199f517cfb04c3ff2cb3a74
framework/languages/node/packages/framework/dist/runtime/channels/channel-dispatchers.js sha256=2eb493e44a2738fe318252cfd07fc0861ecadb6dc335f8104f0d6bcbb3a82f0f
framework/languages/node/packages/framework-codec-protobuf/dist/server/framework.cjs sha256=942e942e8e04fe89f7e63d117ba6f0a96b16e82fa0e26309344e8a119b799e53
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae
.artifacts/wsl/npm/zlink-systems-zlink-1.25.0.tgz sha256=f15954789c6e6e893f704e90dabfa1d9903d32d786370d23c5ef98f7e695e0d3

Binding receive `zlink-raw-server/main.js:64-90,121-142`:
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

Current L0 receive/reply `zlink-framework-server/main.js:76-105`:
    76	    let handled = 0;
    77	    while (handled < batch) {
    78	      let hasMessage;
    79	      try {
    80	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    81	      } catch (error) {
    82	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
    83	        metrics.recordError();
    84	        console.error(`ladder L0 recv failed: ${error.message}`);
    85	        break;
    86	      }
    87	      if (!hasMessage) break;
    88	      handled += 1;
    89	      const parts = received.parts;
    90	      try {
    91	        const header = frameworkWire ? parts[0].data() : null;
    92	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
    93	        if (payload === null) throw new Error('invalid benchmark payload');
    94	        if (received.replyToken !== null) {
    95	          echoHandler.handleL0(payload);
    96	          received.reply()
    97	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
    98	            .message(encodeBenchPayloadMessage(payload))
    99	            .submit();
   100	        } else {
   101	          commandHandler.handleL0(payload);
   102	        }
   103	      } catch (error) {
   104	        metrics.recordError();
   105	        console.error(`ladder L0 handler failed: ${error.message}`);

Both use one reusable Received for request-serial, no blocking wait, atomics, or request-part copy, batch 256 plus setImmediate, ordinary JS metrics, new encoded protobuf body, reply operation, and two-part submit. L0 directly invokes handleL0. Diagnostic switches OFF; Nest/Framework/codec imports remain eager.

Profile raw checkpoint: binding client-source profile is not a target-server CPU profile. Throughput checkpoint: raw/profile/bench/node/run1/zlink-node-request-serial-4096/results.json
First number: binding source→raw target request-serial 4K throughput=8181.4 msg/s, mean latency=0.118831 ms, errors=0, submitted=completed=40908; raw=`raw/profile/bench/node/run1/zlink-node-request-serial-4096/results.json`.

## Before complete paired CPU profile retry
HEAD=48655c21ca22789ca21acc741e138ac2802b8feb dirty=true base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
profile-exit-hook.cjs sha256=9bd4e8cad12add7f2e9077a6ba1a712bd33771c7dbbafcf8ff83b8b142b9e46e
framework/bench/grpc/node/zlink-framework-server/main.js sha256=800c3e9bd884d4e73c57bd3cafbc8a706c39b5de248c9505a07839101643e3fb
framework/bench/grpc/node/run_local.sh sha256=6bf323f1f0de49e45bb92dd193e1f222083b14e41199f517cfb04c3ff2cb3a74
framework/languages/node/packages/framework/dist/runtime/channels/channel-dispatchers.js sha256=2eb493e44a2738fe318252cfd07fc0861ecadb6dc335f8104f0d6bcbb3a82f0f
framework/languages/node/packages/framework-codec-protobuf/dist/server/framework.cjs sha256=942e942e8e04fe89f7e63d117ba6f0a96b16e82fa0e26309344e8a119b799e53
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae
.artifacts/wsl/npm/zlink-systems-zlink-1.25.0.tgz sha256=f15954789c6e6e893f704e90dabfa1d9903d32d786370d23c5ef98f7e695e0d3
Binding receive/reply excerpts `zlink-raw-server/main.js:64-90,121-142,77-90`:
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
L0 receive/reply excerpts `zlink-framework-server/main.js:76-105`:
    76	    let handled = 0;
    77	    while (handled < batch) {
    78	      let hasMessage;
    79	      try {
    80	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    81	      } catch (error) {
    82	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
    83	        metrics.recordError();
    84	        console.error(`ladder L0 recv failed: ${error.message}`);
    85	        break;
    86	      }
    87	      if (!hasMessage) break;
    88	      handled += 1;
    89	      const parts = received.parts;
    90	      try {
    91	        const header = frameworkWire ? parts[0].data() : null;
    92	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
    93	        if (payload === null) throw new Error('invalid benchmark payload');
    94	        if (received.replyToken !== null) {
    95	          echoHandler.handleL0(payload);
    96	          received.reply()
    97	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
    98	            .message(encodeBenchPayloadMessage(payload))
    99	            .submit();
   100	        } else {
   101	          commandHandler.handleL0(payload);
   102	        }
   103	      } catch (error) {
   104	        metrics.recordError();
   105	        console.error(`ladder L0 handler failed: ${error.message}`);
Both reuse one Received for request-serial; no waits, atomics, or request-part copy; batch 256, setImmediate, ordinary metrics. Reply encodes a fresh protobuf body and submits a new envelope plus body. L0 direct handler only. Profile target shutdown: preload hook calls process.exit on SIGTERM only for raw server to flush --cpu-prof output.
Complete-pair retry first result: binding/raw request-serial 4K throughput and counters are in `raw/profile-complete/bench/node/run1/zlink-node-request-serial-4096/results.json`.
Completed pair raw first result: throughput=7696.8 msg/s, mean latency=0.126522 ms, errors=0, submitted=completed=target received=38485; `raw/profile-complete/bench/node/run1/zlink-node-request-serial-4096/results.json`.

## Paired profile result
- Binding/raw target profile: `raw/profile/cpu/CPU.20261113.192849.1888388.0.001.cpuprofile`; L0 target profile: `raw/profile/cpu/CPU.20261113.193048.1903686.0.001.cpuprofile`.
- Paired cell outputs: binding/raw `raw/profile-complete/bench/node/run1/zlink-node-request-serial-4096/results.json` (7696.8 msg/s, mean 0.126522 ms, errors=0); L0 `raw/profile-complete/bench/node/run1/zlink-framework-node-request-serial-4096/results.json` (7552.6 msg/s, mean 0.129061 ms, errors=0). In both results submitted=completed=target received.
- Self sample differences >1 percentage point: idle 77.51%/51.85%; `recv` socket_operations.js:156 8.29%/14.40%; `(program)` 6.17%/13.52%; `processImmediate` node:internal/timers:436 0.77%/2.73%; `routedReceivedPrefersManagedBuffer` message_materializer.js:26 1.31%/2.87%; `replyDirect` router_socket.js:56 0.94%/2.22%; garbage collector 0.62%/1.80%. Full frame comparison from `tools/compare-profiles.py` output; both target profiles are preserved above.

## Before binding
HEAD=48655c21ca22789ca21acc741e138ac2802b8feb dirty=true base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=800c3e9bd884d4e73c57bd3cafbc8a706c39b5de248c9505a07839101643e3fb
framework/bench/grpc/node/run_local.sh sha256=6bf323f1f0de49e45bb92dd193e1f222083b14e41199f517cfb04c3ff2cb3a74
framework/languages/node/packages/framework/dist/runtime/channels/channel-dispatchers.js sha256=2eb493e44a2738fe318252cfd07fc0861ecadb6dc335f8104f0d6bcbb3a82f0f
framework/languages/node/packages/framework-codec-protobuf/dist/server/framework.cjs sha256=942e942e8e04fe89f7e63d117ba6f0a96b16e82fa0e26309344e8a119b799e53
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae
.artifacts/wsl/npm/zlink-systems-zlink-1.25.0.tgz sha256=f15954789c6e6e893f704e90dabfa1d9903d32d786370d23c5ef98f7e695e0d3
Binding receive `zlink-raw-server/main.js:64-90,121-142`:
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
L0 receive/reply `zlink-framework-server/main.js:76-105`:
    76	    let handled = 0;
    77	    while (handled < batch) {
    78	      let hasMessage;
    79	      try {
    80	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    81	      } catch (error) {
    82	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
    83	        metrics.recordError();
    84	        console.error(`ladder L0 recv failed: ${error.message}`);
    85	        break;
    86	      }
    87	      if (!hasMessage) break;
    88	      handled += 1;
    89	      const parts = received.parts;
    90	      try {
    91	        const header = frameworkWire ? parts[0].data() : null;
    92	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
    93	        if (payload === null) throw new Error('invalid benchmark payload');
    94	        if (received.replyToken !== null) {
    95	          echoHandler.handleL0(payload);
    96	          received.reply()
    97	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
    98	            .message(encodeBenchPayloadMessage(payload))
    99	            .submit();
   100	        } else {
   101	          commandHandler.handleL0(payload);
   102	        }
   103	      } catch (error) {
   104	        metrics.recordError();
   105	        console.error(`ladder L0 handler failed: ${error.message}`);
Reply: each path decodes the request body, records ordinary JS metrics, makes a new protobuf body and response operation, and submits a fresh envelope plus body. No per-message wait, atomics, or request-part copy; batch=256, setImmediate pump. L0 directly calls handleL0. Variant=binding.

## Before current
HEAD=48655c21ca22789ca21acc741e138ac2802b8feb dirty=true base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=800c3e9bd884d4e73c57bd3cafbc8a706c39b5de248c9505a07839101643e3fb
framework/bench/grpc/node/run_local.sh sha256=6bf323f1f0de49e45bb92dd193e1f222083b14e41199f517cfb04c3ff2cb3a74
framework/languages/node/packages/framework/dist/runtime/channels/channel-dispatchers.js sha256=2eb493e44a2738fe318252cfd07fc0861ecadb6dc335f8104f0d6bcbb3a82f0f
framework/languages/node/packages/framework-codec-protobuf/dist/server/framework.cjs sha256=942e942e8e04fe89f7e63d117ba6f0a96b16e82fa0e26309344e8a119b799e53
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae
.artifacts/wsl/npm/zlink-systems-zlink-1.25.0.tgz sha256=f15954789c6e6e893f704e90dabfa1d9903d32d786370d23c5ef98f7e695e0d3
Binding receive `zlink-raw-server/main.js:64-90,121-142`:
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
L0 receive/reply `zlink-framework-server/main.js:76-105`:
    76	    let handled = 0;
    77	    while (handled < batch) {
    78	      let hasMessage;
    79	      try {
    80	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    81	      } catch (error) {
    82	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
    83	        metrics.recordError();
    84	        console.error(`ladder L0 recv failed: ${error.message}`);
    85	        break;
    86	      }
    87	      if (!hasMessage) break;
    88	      handled += 1;
    89	      const parts = received.parts;
    90	      try {
    91	        const header = frameworkWire ? parts[0].data() : null;
    92	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
    93	        if (payload === null) throw new Error('invalid benchmark payload');
    94	        if (received.replyToken !== null) {
    95	          echoHandler.handleL0(payload);
    96	          received.reply()
    97	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
    98	            .message(encodeBenchPayloadMessage(payload))
    99	            .submit();
   100	        } else {
   101	          commandHandler.handleL0(payload);
   102	        }
   103	      } catch (error) {
   104	        metrics.recordError();
   105	        console.error(`ladder L0 handler failed: ${error.message}`);
Reply: each path decodes the request body, records ordinary JS metrics, makes a new protobuf body and response operation, and submits a fresh envelope plus body. No per-message wait, atomics, or request-part copy; batch=256, setImmediate pump. L0 directly calls handleL0. Variant=current.

## Before l0-a
HEAD=48655c21ca22789ca21acc741e138ac2802b8feb dirty=true base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=800c3e9bd884d4e73c57bd3cafbc8a706c39b5de248c9505a07839101643e3fb
framework/bench/grpc/node/run_local.sh sha256=6bf323f1f0de49e45bb92dd193e1f222083b14e41199f517cfb04c3ff2cb3a74
framework/languages/node/packages/framework/dist/runtime/channels/channel-dispatchers.js sha256=2eb493e44a2738fe318252cfd07fc0861ecadb6dc335f8104f0d6bcbb3a82f0f
framework/languages/node/packages/framework-codec-protobuf/dist/server/framework.cjs sha256=942e942e8e04fe89f7e63d117ba6f0a96b16e82fa0e26309344e8a119b799e53
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae
.artifacts/wsl/npm/zlink-systems-zlink-1.25.0.tgz sha256=f15954789c6e6e893f704e90dabfa1d9903d32d786370d23c5ef98f7e695e0d3
Binding receive `zlink-raw-server/main.js:64-90,121-142`:
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
L0 receive/reply `zlink-framework-server/main.js:76-105`:
    76	    let handled = 0;
    77	    while (handled < batch) {
    78	      let hasMessage;
    79	      try {
    80	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    81	      } catch (error) {
    82	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
    83	        metrics.recordError();
    84	        console.error(`ladder L0 recv failed: ${error.message}`);
    85	        break;
    86	      }
    87	      if (!hasMessage) break;
    88	      handled += 1;
    89	      const parts = received.parts;
    90	      try {
    91	        const header = frameworkWire ? parts[0].data() : null;
    92	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
    93	        if (payload === null) throw new Error('invalid benchmark payload');
    94	        if (received.replyToken !== null) {
    95	          echoHandler.handleL0(payload);
    96	          received.reply()
    97	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
    98	            .message(encodeBenchPayloadMessage(payload))
    99	            .submit();
   100	        } else {
   101	          commandHandler.handleL0(payload);
   102	        }
   103	      } catch (error) {
   104	        metrics.recordError();
   105	        console.error(`ladder L0 handler failed: ${error.message}`);
Reply: each path decodes the request body, records ordinary JS metrics, makes a new protobuf body and response operation, and submits a fresh envelope plus body. No per-message wait, atomics, or request-part copy; batch=256, setImmediate pump. L0 directly calls handleL0. Variant=l0-a.

## Before l0-b
HEAD=48655c21ca22789ca21acc741e138ac2802b8feb dirty=true base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=800c3e9bd884d4e73c57bd3cafbc8a706c39b5de248c9505a07839101643e3fb
framework/bench/grpc/node/run_local.sh sha256=6bf323f1f0de49e45bb92dd193e1f222083b14e41199f517cfb04c3ff2cb3a74
framework/languages/node/packages/framework/dist/runtime/channels/channel-dispatchers.js sha256=2eb493e44a2738fe318252cfd07fc0861ecadb6dc335f8104f0d6bcbb3a82f0f
framework/languages/node/packages/framework-codec-protobuf/dist/server/framework.cjs sha256=942e942e8e04fe89f7e63d117ba6f0a96b16e82fa0e26309344e8a119b799e53
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae
.artifacts/wsl/npm/zlink-systems-zlink-1.25.0.tgz sha256=f15954789c6e6e893f704e90dabfa1d9903d32d786370d23c5ef98f7e695e0d3
Binding receive `zlink-raw-server/main.js:64-90,121-142`:
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
L0 receive/reply `zlink-framework-server/main.js:76-105`:
    76	    let handled = 0;
    77	    while (handled < batch) {
    78	      let hasMessage;
    79	      try {
    80	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    81	      } catch (error) {
    82	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
    83	        metrics.recordError();
    84	        console.error(`ladder L0 recv failed: ${error.message}`);
    85	        break;
    86	      }
    87	      if (!hasMessage) break;
    88	      handled += 1;
    89	      const parts = received.parts;
    90	      try {
    91	        const header = frameworkWire ? parts[0].data() : null;
    92	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
    93	        if (payload === null) throw new Error('invalid benchmark payload');
    94	        if (received.replyToken !== null) {
    95	          echoHandler.handleL0(payload);
    96	          received.reply()
    97	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
    98	            .message(encodeBenchPayloadMessage(payload))
    99	            .submit();
   100	        } else {
   101	          commandHandler.handleL0(payload);
   102	        }
   103	      } catch (error) {
   104	        metrics.recordError();
   105	        console.error(`ladder L0 handler failed: ${error.message}`);
Reply: each path decodes the request body, records ordinary JS metrics, makes a new protobuf body and response operation, and submits a fresh envelope plus body. No per-message wait, atomics, or request-part copy; batch=256, setImmediate pump. L0 directly calls handleL0. Variant=l0-b.

## Before l0-ab
HEAD=48655c21ca22789ca21acc741e138ac2802b8feb dirty=true base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=800c3e9bd884d4e73c57bd3cafbc8a706c39b5de248c9505a07839101643e3fb
framework/bench/grpc/node/run_local.sh sha256=6bf323f1f0de49e45bb92dd193e1f222083b14e41199f517cfb04c3ff2cb3a74
framework/languages/node/packages/framework/dist/runtime/channels/channel-dispatchers.js sha256=2eb493e44a2738fe318252cfd07fc0861ecadb6dc335f8104f0d6bcbb3a82f0f
framework/languages/node/packages/framework-codec-protobuf/dist/server/framework.cjs sha256=942e942e8e04fe89f7e63d117ba6f0a96b16e82fa0e26309344e8a119b799e53
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae
.artifacts/wsl/npm/zlink-systems-zlink-1.25.0.tgz sha256=f15954789c6e6e893f704e90dabfa1d9903d32d786370d23c5ef98f7e695e0d3
Binding receive `zlink-raw-server/main.js:64-90,121-142`:
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
L0 receive/reply `zlink-framework-server/main.js:76-105`:
    76	    let handled = 0;
    77	    while (handled < batch) {
    78	      let hasMessage;
    79	      try {
    80	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    81	      } catch (error) {
    82	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
    83	        metrics.recordError();
    84	        console.error(`ladder L0 recv failed: ${error.message}`);
    85	        break;
    86	      }
    87	      if (!hasMessage) break;
    88	      handled += 1;
    89	      const parts = received.parts;
    90	      try {
    91	        const header = frameworkWire ? parts[0].data() : null;
    92	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
    93	        if (payload === null) throw new Error('invalid benchmark payload');
    94	        if (received.replyToken !== null) {
    95	          echoHandler.handleL0(payload);
    96	          received.reply()
    97	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
    98	            .message(encodeBenchPayloadMessage(payload))
    99	            .submit();
   100	        } else {
   101	          commandHandler.handleL0(payload);
   102	        }
   103	      } catch (error) {
   104	        metrics.recordError();
   105	        console.error(`ladder L0 handler failed: ${error.message}`);
Reply: each path decodes the request body, records ordinary JS metrics, makes a new protobuf body and response operation, and submits a fresh envelope plus body. No per-message wait, atomics, or request-part copy; batch=256, setImmediate pump. L0 directly calls handleL0. Variant=l0-ab.
