# Node L1 확인 측정 진행

기준: HEAD `c512887606aeb686eccc53162d9f02c3c4db63ed`; 측정 방법은 `common-rules.md`의 11-13 15:29 규칙.


- 2026-11-13T16:29:55Z kairos: measurement processes inherited SCHED_IDLE from their agent until the guard fix at 2026-11-13T16:29:25Z (now the guard sets the lock holder tree SCHED_OTHER every second). Any run whose active window began before 16:29:25Z ran at idle priority: mark it invalid in the report and re-run that cell in this hold if time allows (lad-java-m1: the first cell of this hold, L0 send-saturation 1K).

- 2026-11-13T16:31:39Z kairos (PM 16:32): common-rules.md has two new rules — SCHEDULING CHECK (record chrt -p of every bench thread at run start and mid-active; any thread not SCHED_OTHER = invalid run, re-run it) and PROVISIONAL WINDOW (all 12:05-16:29 UTC results are 잠정, pass and fail alike). Apply both from your next run.
- 2026-11-13T16:31:39Z kairos: PM wants all 6 cells re-measured for the Node L1 step (lad-node-m1 ran 15:40-15:58 UTC, inside the provisional window). In your hold also run L0, L0R and L1 once each for send-saturation 1K, request-backpressure 1K and request-backpressure 4K (in addition to the 3-run cells).

## Before confirm-L0-send-saturation-4096-r1
built commit=c512887606aeb686eccc53162d9f02c3c4db63ed dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=d6db0481dc0af4f77e2fc19b8227b478e1a2539cca55d353eb7118106b1139e3
framework/languages/node/packages/framework/dist/runtime/host/application-job-queue.js sha256=07e9734c0f3466cdd3d174518f2b1d9fa357bef66b169a6d699aa6270493fa27
framework/languages/node/packages/framework/dist/index.js sha256=4aedc33b29681b11d37221bf122fd9fe882eb3b005a9bdb173676ec8dbe85ba9
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae

Binding receive/reply: zlink-raw-server/main.js:64-117
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
    91	  }
    92	  return handled;
    93	}
    94	
    95	function pumpCommands() {
    96	  let handled = 0;
    97	  while (handled < batch) {
    98	    let hasMessage;
    99	    try {
   100	      hasMessage = tryRecv(commandRouter, commandReceived);
   101	    } catch (error) {
   102	      metrics.recordError();
   103	      console.error(`raw command loop recv failed: ${error.message}`);
   104	      break;
   105	    }
   106	    if (!hasMessage) break;
   107	    handled += 1;
   108	    try {
   109	      const part = bodyPart(commandReceived);
   110	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
   111	      if (body === null) throw new Error('invalid raw protobuf payload');
   112	      metrics.record(body);
   113	    } catch (error) {
   114	      metrics.recordError();
   115	      console.error(`raw command loop failed: ${error.message}`);
   116	    }
   117	  }

L0 receive/reply: zlink-framework-server/main.js:93-155
    93	  function pump() {
    94	    let handled = 0;
    95	    while (handled < batch) {
    96	      let hasMessage;
    97	      try {
    98	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    99	      } catch (error) {
   100	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   101	        metrics.recordError();
   102	        console.error(`ladder L0 recv failed: ${error.message}`);
   103	        break;
   104	      }
   105	      if (!hasMessage) break;
   106	      handled += 1;
   107	      const parts = received.parts;
   108	      try {
   109	        const header = frameworkWire ? parts[0].data() : null;
   110	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   111	        if (payload === null) throw new Error('invalid benchmark payload');
   112	        if (received.replyToken !== null) {
   113	          echoHandler.handleL0(payload);
   114	          received.reply()
   115	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
   116	            .message(encodeBenchPayloadMessage(payload))
   117	            .submit();
   118	        } else {
   119	          commandHandler.handleL0(payload);
   120	        }
   121	      } catch (error) {
   122	        metrics.recordError();
   123	        console.error(`ladder L0 handler failed: ${error.message}`);
   124	      }
   125	    }
   126	    return handled;
   127	  }
   128	
   129	  function pumpCommands() {
   130	    if (commandRouter === null) return 0;
   131	    let handled = 0;
   132	    while (handled < batch) {
   133	      let hasMessage;
   134	      try {
   135	        hasMessage = commandRouter.recv(commandReceived, zlink.RecvFlags.DontWait);
   136	      } catch (error) {
   137	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   138	        metrics.recordError();
   139	        console.error(`ladder L0 command recv failed: ${error.message}`);
   140	        break;
   141	      }
   142	      if (!hasMessage) break;
   143	      handled += 1;
   144	      try {
   145	        const parts = commandReceived.parts;
   146	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   147	        if (payload === null) throw new Error('invalid benchmark payload');
   148	        commandHandler.handleL0(payload);
   149	      } catch (error) {
   150	        metrics.recordError();
   151	        console.error(`ladder L0 command handler failed: ${error.message}`);
   152	      }
   153	    }
   154	    return handled;
   155	  }

Per-message waits/allocations/atomics/copies: binding and L0/L0R use one reusable Received, DontWait, batch 256, setImmediate; no queue wait, no JS atomics. L1 awaits ApplicationJobQueue.acquire(remote) before each receive and releases at handler entry. All paths decode the protobuf body, allocate a new encoded body and reply operation, and submit two parts; they do not copy or reuse request parts.

Run start L0 send-saturation-4096 r1: 2026-11-13T16:34:58Z

First result L0 send-saturation-4096 r1: 77054 msg/s; errors=0 submitted=completed=received=385270. Raw: raw/session/confirm/send-saturation-4096/L0/run1/run1/zlink-framework-node-send-saturation-4096/results.json.

## Before confirm-L0R-send-saturation-4096-r1
built commit=c512887606aeb686eccc53162d9f02c3c4db63ed dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=d6db0481dc0af4f77e2fc19b8227b478e1a2539cca55d353eb7118106b1139e3
framework/languages/node/packages/framework/dist/runtime/host/application-job-queue.js sha256=07e9734c0f3466cdd3d174518f2b1d9fa357bef66b169a6d699aa6270493fa27
framework/languages/node/packages/framework/dist/index.js sha256=4aedc33b29681b11d37221bf122fd9fe882eb3b005a9bdb173676ec8dbe85ba9
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae

Binding receive/reply: zlink-raw-server/main.js:64-117
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
    91	  }
    92	  return handled;
    93	}
    94	
    95	function pumpCommands() {
    96	  let handled = 0;
    97	  while (handled < batch) {
    98	    let hasMessage;
    99	    try {
   100	      hasMessage = tryRecv(commandRouter, commandReceived);
   101	    } catch (error) {
   102	      metrics.recordError();
   103	      console.error(`raw command loop recv failed: ${error.message}`);
   104	      break;
   105	    }
   106	    if (!hasMessage) break;
   107	    handled += 1;
   108	    try {
   109	      const part = bodyPart(commandReceived);
   110	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
   111	      if (body === null) throw new Error('invalid raw protobuf payload');
   112	      metrics.record(body);
   113	    } catch (error) {
   114	      metrics.recordError();
   115	      console.error(`raw command loop failed: ${error.message}`);
   116	    }
   117	  }

L0 receive/reply: zlink-framework-server/main.js:93-155
    93	  function pump() {
    94	    let handled = 0;
    95	    while (handled < batch) {
    96	      let hasMessage;
    97	      try {
    98	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    99	      } catch (error) {
   100	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   101	        metrics.recordError();
   102	        console.error(`ladder L0 recv failed: ${error.message}`);
   103	        break;
   104	      }
   105	      if (!hasMessage) break;
   106	      handled += 1;
   107	      const parts = received.parts;
   108	      try {
   109	        const header = frameworkWire ? parts[0].data() : null;
   110	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   111	        if (payload === null) throw new Error('invalid benchmark payload');
   112	        if (received.replyToken !== null) {
   113	          echoHandler.handleL0(payload);
   114	          received.reply()
   115	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
   116	            .message(encodeBenchPayloadMessage(payload))
   117	            .submit();
   118	        } else {
   119	          commandHandler.handleL0(payload);
   120	        }
   121	      } catch (error) {
   122	        metrics.recordError();
   123	        console.error(`ladder L0 handler failed: ${error.message}`);
   124	      }
   125	    }
   126	    return handled;
   127	  }
   128	
   129	  function pumpCommands() {
   130	    if (commandRouter === null) return 0;
   131	    let handled = 0;
   132	    while (handled < batch) {
   133	      let hasMessage;
   134	      try {
   135	        hasMessage = commandRouter.recv(commandReceived, zlink.RecvFlags.DontWait);
   136	      } catch (error) {
   137	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   138	        metrics.recordError();
   139	        console.error(`ladder L0 command recv failed: ${error.message}`);
   140	        break;
   141	      }
   142	      if (!hasMessage) break;
   143	      handled += 1;
   144	      try {
   145	        const parts = commandReceived.parts;
   146	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   147	        if (payload === null) throw new Error('invalid benchmark payload');
   148	        commandHandler.handleL0(payload);
   149	      } catch (error) {
   150	        metrics.recordError();
   151	        console.error(`ladder L0 command handler failed: ${error.message}`);
   152	      }
   153	    }
   154	    return handled;
   155	  }

Per-message waits/allocations/atomics/copies: binding and L0/L0R use one reusable Received, DontWait, batch 256, setImmediate; no queue wait, no JS atomics. L1 awaits ApplicationJobQueue.acquire(remote) before each receive and releases at handler entry. All paths decode the protobuf body, allocate a new encoded body and reply operation, and submit two parts; they do not copy or reuse request parts.

Run start L0R send-saturation-4096 r1: 2026-11-13T16:36:11Z

First result L0R send-saturation-4096 r1: 87241 msg/s; errors=0 submitted=completed=received=436586. Raw: raw/session/confirm/send-saturation-4096/L0R/run1/run1/zlink-framework-node-send-saturation-4096/results.json.

## Before confirm-L1-send-saturation-4096-r1
built commit=c512887606aeb686eccc53162d9f02c3c4db63ed dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=d6db0481dc0af4f77e2fc19b8227b478e1a2539cca55d353eb7118106b1139e3
framework/languages/node/packages/framework/dist/runtime/host/application-job-queue.js sha256=07e9734c0f3466cdd3d174518f2b1d9fa357bef66b169a6d699aa6270493fa27
framework/languages/node/packages/framework/dist/index.js sha256=4aedc33b29681b11d37221bf122fd9fe882eb3b005a9bdb173676ec8dbe85ba9
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae

Binding receive/reply: zlink-raw-server/main.js:64-117
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
    91	  }
    92	  return handled;
    93	}
    94	
    95	function pumpCommands() {
    96	  let handled = 0;
    97	  while (handled < batch) {
    98	    let hasMessage;
    99	    try {
   100	      hasMessage = tryRecv(commandRouter, commandReceived);
   101	    } catch (error) {
   102	      metrics.recordError();
   103	      console.error(`raw command loop recv failed: ${error.message}`);
   104	      break;
   105	    }
   106	    if (!hasMessage) break;
   107	    handled += 1;
   108	    try {
   109	      const part = bodyPart(commandReceived);
   110	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
   111	      if (body === null) throw new Error('invalid raw protobuf payload');
   112	      metrics.record(body);
   113	    } catch (error) {
   114	      metrics.recordError();
   115	      console.error(`raw command loop failed: ${error.message}`);
   116	    }
   117	  }

L0 receive/reply: zlink-framework-server/main.js:93-155
    93	  function pump() {
    94	    let handled = 0;
    95	    while (handled < batch) {
    96	      let hasMessage;
    97	      try {
    98	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    99	      } catch (error) {
   100	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   101	        metrics.recordError();
   102	        console.error(`ladder L0 recv failed: ${error.message}`);
   103	        break;
   104	      }
   105	      if (!hasMessage) break;
   106	      handled += 1;
   107	      const parts = received.parts;
   108	      try {
   109	        const header = frameworkWire ? parts[0].data() : null;
   110	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   111	        if (payload === null) throw new Error('invalid benchmark payload');
   112	        if (received.replyToken !== null) {
   113	          echoHandler.handleL0(payload);
   114	          received.reply()
   115	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
   116	            .message(encodeBenchPayloadMessage(payload))
   117	            .submit();
   118	        } else {
   119	          commandHandler.handleL0(payload);
   120	        }
   121	      } catch (error) {
   122	        metrics.recordError();
   123	        console.error(`ladder L0 handler failed: ${error.message}`);
   124	      }
   125	    }
   126	    return handled;
   127	  }
   128	
   129	  function pumpCommands() {
   130	    if (commandRouter === null) return 0;
   131	    let handled = 0;
   132	    while (handled < batch) {
   133	      let hasMessage;
   134	      try {
   135	        hasMessage = commandRouter.recv(commandReceived, zlink.RecvFlags.DontWait);
   136	      } catch (error) {
   137	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   138	        metrics.recordError();
   139	        console.error(`ladder L0 command recv failed: ${error.message}`);
   140	        break;
   141	      }
   142	      if (!hasMessage) break;
   143	      handled += 1;
   144	      try {
   145	        const parts = commandReceived.parts;
   146	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   147	        if (payload === null) throw new Error('invalid benchmark payload');
   148	        commandHandler.handleL0(payload);
   149	      } catch (error) {
   150	        metrics.recordError();
   151	        console.error(`ladder L0 command handler failed: ${error.message}`);
   152	      }
   153	    }
   154	    return handled;
   155	  }

L1 permit receive/reply: zlink-framework-server/main.js:157-214
   157	  async function pumpWithPermits(socket, targetReceived, processMessage, label) {
   158	    let handled = 0;
   159	    while (handled < batch) {
   160	      const permit = await applicationJobQueue.acquire(undefined, 'remote');
   161	      let hasMessage;
   162	      try {
   163	        hasMessage = socket.recv(targetReceived, zlink.RecvFlags.DontWait);
   164	      } catch (error) {
   165	        permit.releaseUnqueued();
   166	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   167	        metrics.recordError();
   168	        console.error(`ladder L1 ${label} recv failed: ${error.message}`);
   169	        break;
   170	      }
   171	      if (!hasMessage) {
   172	        permit.releaseUnqueued();
   173	        break;
   174	      }
   175	      handled += 1;
   176	      try {
   177	        processMessage(permit);
   178	      } catch (error) {
   179	        permit.releaseAfterInternalProcessing();
   180	        metrics.recordError();
   181	        console.error(`ladder L1 ${label} handler failed: ${error.message}`);
   182	      }
   183	    }
   184	    return handled;
   185	  }
   186	
   187	  async function pumpRequestsWithPermits() {
   188	    return pumpWithPermits(router, received, (permit) => {
   189	      const parts = received.parts;
   190	      const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   191	      if (payload === null) throw new Error('invalid benchmark payload');
   192	      if (received.replyToken !== null) {
   193	        permit.releaseBeforeHandler();
   194	        echoHandler.handleL0(payload);
   195	        received.reply()
   196	          .message(RESPONSE_ENVELOPE)
   197	          .message(encodeBenchPayloadMessage(payload))
   198	          .submit();
   199	      } else {
   200	        permit.releaseBeforeHandler();
   201	        commandHandler.handleL0(payload);
   202	      }
   203	    }, 'request');
   204	  }
   205	
   206	  async function pumpCommandsWithPermits() {
   207	    return pumpWithPermits(commandRouter, commandReceived, (permit) => {
   208	      const parts = commandReceived.parts;
   209	      const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   210	      if (payload === null) throw new Error('invalid benchmark payload');
   211	      permit.releaseBeforeHandler();
   212	      commandHandler.handleL0(payload);
   213	    }, 'command');
   214	  }

Per-message waits/allocations/atomics/copies: binding and L0/L0R use one reusable Received, DontWait, batch 256, setImmediate; no queue wait, no JS atomics. L1 awaits ApplicationJobQueue.acquire(remote) before each receive and releases at handler entry. All paths decode the protobuf body, allocate a new encoded body and reply operation, and submit two parts; they do not copy or reuse request parts.

Run start L1 send-saturation-4096 r1: 2026-11-13T16:36:26Z

First result L1 send-saturation-4096 r1: 75917.8 msg/s; errors=0 submitted=completed=received=379589. Raw: raw/session/confirm/send-saturation-4096/L1/run1/run1/zlink-framework-node-send-saturation-4096/results.json.

## Before confirm-L0-send-saturation-4096-r2
built commit=c512887606aeb686eccc53162d9f02c3c4db63ed dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=d6db0481dc0af4f77e2fc19b8227b478e1a2539cca55d353eb7118106b1139e3
framework/languages/node/packages/framework/dist/runtime/host/application-job-queue.js sha256=07e9734c0f3466cdd3d174518f2b1d9fa357bef66b169a6d699aa6270493fa27
framework/languages/node/packages/framework/dist/index.js sha256=4aedc33b29681b11d37221bf122fd9fe882eb3b005a9bdb173676ec8dbe85ba9
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae

Binding receive/reply: zlink-raw-server/main.js:64-117
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
    91	  }
    92	  return handled;
    93	}
    94	
    95	function pumpCommands() {
    96	  let handled = 0;
    97	  while (handled < batch) {
    98	    let hasMessage;
    99	    try {
   100	      hasMessage = tryRecv(commandRouter, commandReceived);
   101	    } catch (error) {
   102	      metrics.recordError();
   103	      console.error(`raw command loop recv failed: ${error.message}`);
   104	      break;
   105	    }
   106	    if (!hasMessage) break;
   107	    handled += 1;
   108	    try {
   109	      const part = bodyPart(commandReceived);
   110	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
   111	      if (body === null) throw new Error('invalid raw protobuf payload');
   112	      metrics.record(body);
   113	    } catch (error) {
   114	      metrics.recordError();
   115	      console.error(`raw command loop failed: ${error.message}`);
   116	    }
   117	  }

L0 receive/reply: zlink-framework-server/main.js:93-155
    93	  function pump() {
    94	    let handled = 0;
    95	    while (handled < batch) {
    96	      let hasMessage;
    97	      try {
    98	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    99	      } catch (error) {
   100	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   101	        metrics.recordError();
   102	        console.error(`ladder L0 recv failed: ${error.message}`);
   103	        break;
   104	      }
   105	      if (!hasMessage) break;
   106	      handled += 1;
   107	      const parts = received.parts;
   108	      try {
   109	        const header = frameworkWire ? parts[0].data() : null;
   110	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   111	        if (payload === null) throw new Error('invalid benchmark payload');
   112	        if (received.replyToken !== null) {
   113	          echoHandler.handleL0(payload);
   114	          received.reply()
   115	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
   116	            .message(encodeBenchPayloadMessage(payload))
   117	            .submit();
   118	        } else {
   119	          commandHandler.handleL0(payload);
   120	        }
   121	      } catch (error) {
   122	        metrics.recordError();
   123	        console.error(`ladder L0 handler failed: ${error.message}`);
   124	      }
   125	    }
   126	    return handled;
   127	  }
   128	
   129	  function pumpCommands() {
   130	    if (commandRouter === null) return 0;
   131	    let handled = 0;
   132	    while (handled < batch) {
   133	      let hasMessage;
   134	      try {
   135	        hasMessage = commandRouter.recv(commandReceived, zlink.RecvFlags.DontWait);
   136	      } catch (error) {
   137	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   138	        metrics.recordError();
   139	        console.error(`ladder L0 command recv failed: ${error.message}`);
   140	        break;
   141	      }
   142	      if (!hasMessage) break;
   143	      handled += 1;
   144	      try {
   145	        const parts = commandReceived.parts;
   146	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   147	        if (payload === null) throw new Error('invalid benchmark payload');
   148	        commandHandler.handleL0(payload);
   149	      } catch (error) {
   150	        metrics.recordError();
   151	        console.error(`ladder L0 command handler failed: ${error.message}`);
   152	      }
   153	    }
   154	    return handled;
   155	  }

Per-message waits/allocations/atomics/copies: binding and L0/L0R use one reusable Received, DontWait, batch 256, setImmediate; no queue wait, no JS atomics. L1 awaits ApplicationJobQueue.acquire(remote) before each receive and releases at handler entry. All paths decode the protobuf body, allocate a new encoded body and reply operation, and submit two parts; they do not copy or reuse request parts.

Run start L0 send-saturation-4096 r2: 2026-11-13T16:37:11Z

First result L0 send-saturation-4096 r2: 77428.4 msg/s; errors=0 submitted=completed=received=387142. Raw: raw/session/confirm/send-saturation-4096/L0/run2/run1/zlink-framework-node-send-saturation-4096/results.json.

## Before confirm-L0R-send-saturation-4096-r2
built commit=c512887606aeb686eccc53162d9f02c3c4db63ed dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=d6db0481dc0af4f77e2fc19b8227b478e1a2539cca55d353eb7118106b1139e3
framework/languages/node/packages/framework/dist/runtime/host/application-job-queue.js sha256=07e9734c0f3466cdd3d174518f2b1d9fa357bef66b169a6d699aa6270493fa27
framework/languages/node/packages/framework/dist/index.js sha256=4aedc33b29681b11d37221bf122fd9fe882eb3b005a9bdb173676ec8dbe85ba9
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae

Binding receive/reply: zlink-raw-server/main.js:64-117
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
    91	  }
    92	  return handled;
    93	}
    94	
    95	function pumpCommands() {
    96	  let handled = 0;
    97	  while (handled < batch) {
    98	    let hasMessage;
    99	    try {
   100	      hasMessage = tryRecv(commandRouter, commandReceived);
   101	    } catch (error) {
   102	      metrics.recordError();
   103	      console.error(`raw command loop recv failed: ${error.message}`);
   104	      break;
   105	    }
   106	    if (!hasMessage) break;
   107	    handled += 1;
   108	    try {
   109	      const part = bodyPart(commandReceived);
   110	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
   111	      if (body === null) throw new Error('invalid raw protobuf payload');
   112	      metrics.record(body);
   113	    } catch (error) {
   114	      metrics.recordError();
   115	      console.error(`raw command loop failed: ${error.message}`);
   116	    }
   117	  }

L0 receive/reply: zlink-framework-server/main.js:93-155
    93	  function pump() {
    94	    let handled = 0;
    95	    while (handled < batch) {
    96	      let hasMessage;
    97	      try {
    98	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    99	      } catch (error) {
   100	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   101	        metrics.recordError();
   102	        console.error(`ladder L0 recv failed: ${error.message}`);
   103	        break;
   104	      }
   105	      if (!hasMessage) break;
   106	      handled += 1;
   107	      const parts = received.parts;
   108	      try {
   109	        const header = frameworkWire ? parts[0].data() : null;
   110	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   111	        if (payload === null) throw new Error('invalid benchmark payload');
   112	        if (received.replyToken !== null) {
   113	          echoHandler.handleL0(payload);
   114	          received.reply()
   115	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
   116	            .message(encodeBenchPayloadMessage(payload))
   117	            .submit();
   118	        } else {
   119	          commandHandler.handleL0(payload);
   120	        }
   121	      } catch (error) {
   122	        metrics.recordError();
   123	        console.error(`ladder L0 handler failed: ${error.message}`);
   124	      }
   125	    }
   126	    return handled;
   127	  }
   128	
   129	  function pumpCommands() {
   130	    if (commandRouter === null) return 0;
   131	    let handled = 0;
   132	    while (handled < batch) {
   133	      let hasMessage;
   134	      try {
   135	        hasMessage = commandRouter.recv(commandReceived, zlink.RecvFlags.DontWait);
   136	      } catch (error) {
   137	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   138	        metrics.recordError();
   139	        console.error(`ladder L0 command recv failed: ${error.message}`);
   140	        break;
   141	      }
   142	      if (!hasMessage) break;
   143	      handled += 1;
   144	      try {
   145	        const parts = commandReceived.parts;
   146	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   147	        if (payload === null) throw new Error('invalid benchmark payload');
   148	        commandHandler.handleL0(payload);
   149	      } catch (error) {
   150	        metrics.recordError();
   151	        console.error(`ladder L0 command handler failed: ${error.message}`);
   152	      }
   153	    }
   154	    return handled;
   155	  }

Per-message waits/allocations/atomics/copies: binding and L0/L0R use one reusable Received, DontWait, batch 256, setImmediate; no queue wait, no JS atomics. L1 awaits ApplicationJobQueue.acquire(remote) before each receive and releases at handler entry. All paths decode the protobuf body, allocate a new encoded body and reply operation, and submit two parts; they do not copy or reuse request parts.

Run start L0R send-saturation-4096 r2: 2026-11-13T16:37:40Z

First result L0R send-saturation-4096 r2: 85901.4 msg/s; errors=0 submitted=completed=received=430418. Raw: raw/session/confirm/send-saturation-4096/L0R/run2/run1/zlink-framework-node-send-saturation-4096/results.json.

## Before confirm-L1-send-saturation-4096-r2
built commit=c512887606aeb686eccc53162d9f02c3c4db63ed dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=d6db0481dc0af4f77e2fc19b8227b478e1a2539cca55d353eb7118106b1139e3
framework/languages/node/packages/framework/dist/runtime/host/application-job-queue.js sha256=07e9734c0f3466cdd3d174518f2b1d9fa357bef66b169a6d699aa6270493fa27
framework/languages/node/packages/framework/dist/index.js sha256=4aedc33b29681b11d37221bf122fd9fe882eb3b005a9bdb173676ec8dbe85ba9
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae

Binding receive/reply: zlink-raw-server/main.js:64-117
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
    91	  }
    92	  return handled;
    93	}
    94	
    95	function pumpCommands() {
    96	  let handled = 0;
    97	  while (handled < batch) {
    98	    let hasMessage;
    99	    try {
   100	      hasMessage = tryRecv(commandRouter, commandReceived);
   101	    } catch (error) {
   102	      metrics.recordError();
   103	      console.error(`raw command loop recv failed: ${error.message}`);
   104	      break;
   105	    }
   106	    if (!hasMessage) break;
   107	    handled += 1;
   108	    try {
   109	      const part = bodyPart(commandReceived);
   110	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
   111	      if (body === null) throw new Error('invalid raw protobuf payload');
   112	      metrics.record(body);
   113	    } catch (error) {
   114	      metrics.recordError();
   115	      console.error(`raw command loop failed: ${error.message}`);
   116	    }
   117	  }

L0 receive/reply: zlink-framework-server/main.js:93-155
    93	  function pump() {
    94	    let handled = 0;
    95	    while (handled < batch) {
    96	      let hasMessage;
    97	      try {
    98	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    99	      } catch (error) {
   100	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   101	        metrics.recordError();
   102	        console.error(`ladder L0 recv failed: ${error.message}`);
   103	        break;
   104	      }
   105	      if (!hasMessage) break;
   106	      handled += 1;
   107	      const parts = received.parts;
   108	      try {
   109	        const header = frameworkWire ? parts[0].data() : null;
   110	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   111	        if (payload === null) throw new Error('invalid benchmark payload');
   112	        if (received.replyToken !== null) {
   113	          echoHandler.handleL0(payload);
   114	          received.reply()
   115	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
   116	            .message(encodeBenchPayloadMessage(payload))
   117	            .submit();
   118	        } else {
   119	          commandHandler.handleL0(payload);
   120	        }
   121	      } catch (error) {
   122	        metrics.recordError();
   123	        console.error(`ladder L0 handler failed: ${error.message}`);
   124	      }
   125	    }
   126	    return handled;
   127	  }
   128	
   129	  function pumpCommands() {
   130	    if (commandRouter === null) return 0;
   131	    let handled = 0;
   132	    while (handled < batch) {
   133	      let hasMessage;
   134	      try {
   135	        hasMessage = commandRouter.recv(commandReceived, zlink.RecvFlags.DontWait);
   136	      } catch (error) {
   137	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   138	        metrics.recordError();
   139	        console.error(`ladder L0 command recv failed: ${error.message}`);
   140	        break;
   141	      }
   142	      if (!hasMessage) break;
   143	      handled += 1;
   144	      try {
   145	        const parts = commandReceived.parts;
   146	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   147	        if (payload === null) throw new Error('invalid benchmark payload');
   148	        commandHandler.handleL0(payload);
   149	      } catch (error) {
   150	        metrics.recordError();
   151	        console.error(`ladder L0 command handler failed: ${error.message}`);
   152	      }
   153	    }
   154	    return handled;
   155	  }

L1 permit receive/reply: zlink-framework-server/main.js:157-214
   157	  async function pumpWithPermits(socket, targetReceived, processMessage, label) {
   158	    let handled = 0;
   159	    while (handled < batch) {
   160	      const permit = await applicationJobQueue.acquire(undefined, 'remote');
   161	      let hasMessage;
   162	      try {
   163	        hasMessage = socket.recv(targetReceived, zlink.RecvFlags.DontWait);
   164	      } catch (error) {
   165	        permit.releaseUnqueued();
   166	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   167	        metrics.recordError();
   168	        console.error(`ladder L1 ${label} recv failed: ${error.message}`);
   169	        break;
   170	      }
   171	      if (!hasMessage) {
   172	        permit.releaseUnqueued();
   173	        break;
   174	      }
   175	      handled += 1;
   176	      try {
   177	        processMessage(permit);
   178	      } catch (error) {
   179	        permit.releaseAfterInternalProcessing();
   180	        metrics.recordError();
   181	        console.error(`ladder L1 ${label} handler failed: ${error.message}`);
   182	      }
   183	    }
   184	    return handled;
   185	  }
   186	
   187	  async function pumpRequestsWithPermits() {
   188	    return pumpWithPermits(router, received, (permit) => {
   189	      const parts = received.parts;
   190	      const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   191	      if (payload === null) throw new Error('invalid benchmark payload');
   192	      if (received.replyToken !== null) {
   193	        permit.releaseBeforeHandler();
   194	        echoHandler.handleL0(payload);
   195	        received.reply()
   196	          .message(RESPONSE_ENVELOPE)
   197	          .message(encodeBenchPayloadMessage(payload))
   198	          .submit();
   199	      } else {
   200	        permit.releaseBeforeHandler();
   201	        commandHandler.handleL0(payload);
   202	      }
   203	    }, 'request');
   204	  }
   205	
   206	  async function pumpCommandsWithPermits() {
   207	    return pumpWithPermits(commandRouter, commandReceived, (permit) => {
   208	      const parts = commandReceived.parts;
   209	      const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   210	      if (payload === null) throw new Error('invalid benchmark payload');
   211	      permit.releaseBeforeHandler();
   212	      commandHandler.handleL0(payload);
   213	    }, 'command');
   214	  }

Per-message waits/allocations/atomics/copies: binding and L0/L0R use one reusable Received, DontWait, batch 256, setImmediate; no queue wait, no JS atomics. L1 awaits ApplicationJobQueue.acquire(remote) before each receive and releases at handler entry. All paths decode the protobuf body, allocate a new encoded body and reply operation, and submit two parts; they do not copy or reuse request parts.

Run start L1 send-saturation-4096 r2: 2026-11-13T16:37:55Z

First result L1 send-saturation-4096 r2: 74911 msg/s; errors=0 submitted=completed=received=374555. Raw: raw/session/confirm/send-saturation-4096/L1/run2/run1/zlink-framework-node-send-saturation-4096/results.json.

## Before confirm-L0-send-saturation-4096-r3
built commit=c512887606aeb686eccc53162d9f02c3c4db63ed dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=d6db0481dc0af4f77e2fc19b8227b478e1a2539cca55d353eb7118106b1139e3
framework/languages/node/packages/framework/dist/runtime/host/application-job-queue.js sha256=07e9734c0f3466cdd3d174518f2b1d9fa357bef66b169a6d699aa6270493fa27
framework/languages/node/packages/framework/dist/index.js sha256=4aedc33b29681b11d37221bf122fd9fe882eb3b005a9bdb173676ec8dbe85ba9
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae

Binding receive/reply: zlink-raw-server/main.js:64-117
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
    91	  }
    92	  return handled;
    93	}
    94	
    95	function pumpCommands() {
    96	  let handled = 0;
    97	  while (handled < batch) {
    98	    let hasMessage;
    99	    try {
   100	      hasMessage = tryRecv(commandRouter, commandReceived);
   101	    } catch (error) {
   102	      metrics.recordError();
   103	      console.error(`raw command loop recv failed: ${error.message}`);
   104	      break;
   105	    }
   106	    if (!hasMessage) break;
   107	    handled += 1;
   108	    try {
   109	      const part = bodyPart(commandReceived);
   110	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
   111	      if (body === null) throw new Error('invalid raw protobuf payload');
   112	      metrics.record(body);
   113	    } catch (error) {
   114	      metrics.recordError();
   115	      console.error(`raw command loop failed: ${error.message}`);
   116	    }
   117	  }

L0 receive/reply: zlink-framework-server/main.js:93-155
    93	  function pump() {
    94	    let handled = 0;
    95	    while (handled < batch) {
    96	      let hasMessage;
    97	      try {
    98	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    99	      } catch (error) {
   100	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   101	        metrics.recordError();
   102	        console.error(`ladder L0 recv failed: ${error.message}`);
   103	        break;
   104	      }
   105	      if (!hasMessage) break;
   106	      handled += 1;
   107	      const parts = received.parts;
   108	      try {
   109	        const header = frameworkWire ? parts[0].data() : null;
   110	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   111	        if (payload === null) throw new Error('invalid benchmark payload');
   112	        if (received.replyToken !== null) {
   113	          echoHandler.handleL0(payload);
   114	          received.reply()
   115	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
   116	            .message(encodeBenchPayloadMessage(payload))
   117	            .submit();
   118	        } else {
   119	          commandHandler.handleL0(payload);
   120	        }
   121	      } catch (error) {
   122	        metrics.recordError();
   123	        console.error(`ladder L0 handler failed: ${error.message}`);
   124	      }
   125	    }
   126	    return handled;
   127	  }
   128	
   129	  function pumpCommands() {
   130	    if (commandRouter === null) return 0;
   131	    let handled = 0;
   132	    while (handled < batch) {
   133	      let hasMessage;
   134	      try {
   135	        hasMessage = commandRouter.recv(commandReceived, zlink.RecvFlags.DontWait);
   136	      } catch (error) {
   137	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   138	        metrics.recordError();
   139	        console.error(`ladder L0 command recv failed: ${error.message}`);
   140	        break;
   141	      }
   142	      if (!hasMessage) break;
   143	      handled += 1;
   144	      try {
   145	        const parts = commandReceived.parts;
   146	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   147	        if (payload === null) throw new Error('invalid benchmark payload');
   148	        commandHandler.handleL0(payload);
   149	      } catch (error) {
   150	        metrics.recordError();
   151	        console.error(`ladder L0 command handler failed: ${error.message}`);
   152	      }
   153	    }
   154	    return handled;
   155	  }

Per-message waits/allocations/atomics/copies: binding and L0/L0R use one reusable Received, DontWait, batch 256, setImmediate; no queue wait, no JS atomics. L1 awaits ApplicationJobQueue.acquire(remote) before each receive and releases at handler entry. All paths decode the protobuf body, allocate a new encoded body and reply operation, and submit two parts; they do not copy or reuse request parts.

Run start L0 send-saturation-4096 r3: 2026-11-13T16:38:20Z

First result L0 send-saturation-4096 r3: 87327.6 msg/s; errors=0 submitted=completed=received=436638. Raw: raw/session/confirm/send-saturation-4096/L0/run3/run1/zlink-framework-node-send-saturation-4096/results.json.

## Before confirm-L0R-send-saturation-4096-r3
built commit=c512887606aeb686eccc53162d9f02c3c4db63ed dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=d6db0481dc0af4f77e2fc19b8227b478e1a2539cca55d353eb7118106b1139e3
framework/languages/node/packages/framework/dist/runtime/host/application-job-queue.js sha256=07e9734c0f3466cdd3d174518f2b1d9fa357bef66b169a6d699aa6270493fa27
framework/languages/node/packages/framework/dist/index.js sha256=4aedc33b29681b11d37221bf122fd9fe882eb3b005a9bdb173676ec8dbe85ba9
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae

Binding receive/reply: zlink-raw-server/main.js:64-117
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
    91	  }
    92	  return handled;
    93	}
    94	
    95	function pumpCommands() {
    96	  let handled = 0;
    97	  while (handled < batch) {
    98	    let hasMessage;
    99	    try {
   100	      hasMessage = tryRecv(commandRouter, commandReceived);
   101	    } catch (error) {
   102	      metrics.recordError();
   103	      console.error(`raw command loop recv failed: ${error.message}`);
   104	      break;
   105	    }
   106	    if (!hasMessage) break;
   107	    handled += 1;
   108	    try {
   109	      const part = bodyPart(commandReceived);
   110	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
   111	      if (body === null) throw new Error('invalid raw protobuf payload');
   112	      metrics.record(body);
   113	    } catch (error) {
   114	      metrics.recordError();
   115	      console.error(`raw command loop failed: ${error.message}`);
   116	    }
   117	  }

L0 receive/reply: zlink-framework-server/main.js:93-155
    93	  function pump() {
    94	    let handled = 0;
    95	    while (handled < batch) {
    96	      let hasMessage;
    97	      try {
    98	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    99	      } catch (error) {
   100	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   101	        metrics.recordError();
   102	        console.error(`ladder L0 recv failed: ${error.message}`);
   103	        break;
   104	      }
   105	      if (!hasMessage) break;
   106	      handled += 1;
   107	      const parts = received.parts;
   108	      try {
   109	        const header = frameworkWire ? parts[0].data() : null;
   110	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   111	        if (payload === null) throw new Error('invalid benchmark payload');
   112	        if (received.replyToken !== null) {
   113	          echoHandler.handleL0(payload);
   114	          received.reply()
   115	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
   116	            .message(encodeBenchPayloadMessage(payload))
   117	            .submit();
   118	        } else {
   119	          commandHandler.handleL0(payload);
   120	        }
   121	      } catch (error) {
   122	        metrics.recordError();
   123	        console.error(`ladder L0 handler failed: ${error.message}`);
   124	      }
   125	    }
   126	    return handled;
   127	  }
   128	
   129	  function pumpCommands() {
   130	    if (commandRouter === null) return 0;
   131	    let handled = 0;
   132	    while (handled < batch) {
   133	      let hasMessage;
   134	      try {
   135	        hasMessage = commandRouter.recv(commandReceived, zlink.RecvFlags.DontWait);
   136	      } catch (error) {
   137	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   138	        metrics.recordError();
   139	        console.error(`ladder L0 command recv failed: ${error.message}`);
   140	        break;
   141	      }
   142	      if (!hasMessage) break;
   143	      handled += 1;
   144	      try {
   145	        const parts = commandReceived.parts;
   146	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   147	        if (payload === null) throw new Error('invalid benchmark payload');
   148	        commandHandler.handleL0(payload);
   149	      } catch (error) {
   150	        metrics.recordError();
   151	        console.error(`ladder L0 command handler failed: ${error.message}`);
   152	      }
   153	    }
   154	    return handled;
   155	  }

Per-message waits/allocations/atomics/copies: binding and L0/L0R use one reusable Received, DontWait, batch 256, setImmediate; no queue wait, no JS atomics. L1 awaits ApplicationJobQueue.acquire(remote) before each receive and releases at handler entry. All paths decode the protobuf body, allocate a new encoded body and reply operation, and submit two parts; they do not copy or reuse request parts.

Run start L0R send-saturation-4096 r3: 2026-11-13T16:38:31Z

First result L0R send-saturation-4096 r3: 85963.6 msg/s; errors=0 submitted=completed=received=429818. Raw: raw/session/confirm/send-saturation-4096/L0R/run3/run1/zlink-framework-node-send-saturation-4096/results.json.

## Before confirm-L1-send-saturation-4096-r3
built commit=c512887606aeb686eccc53162d9f02c3c4db63ed dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=d6db0481dc0af4f77e2fc19b8227b478e1a2539cca55d353eb7118106b1139e3
framework/languages/node/packages/framework/dist/runtime/host/application-job-queue.js sha256=07e9734c0f3466cdd3d174518f2b1d9fa357bef66b169a6d699aa6270493fa27
framework/languages/node/packages/framework/dist/index.js sha256=4aedc33b29681b11d37221bf122fd9fe882eb3b005a9bdb173676ec8dbe85ba9
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae

Binding receive/reply: zlink-raw-server/main.js:64-117
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
    91	  }
    92	  return handled;
    93	}
    94	
    95	function pumpCommands() {
    96	  let handled = 0;
    97	  while (handled < batch) {
    98	    let hasMessage;
    99	    try {
   100	      hasMessage = tryRecv(commandRouter, commandReceived);
   101	    } catch (error) {
   102	      metrics.recordError();
   103	      console.error(`raw command loop recv failed: ${error.message}`);
   104	      break;
   105	    }
   106	    if (!hasMessage) break;
   107	    handled += 1;
   108	    try {
   109	      const part = bodyPart(commandReceived);
   110	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
   111	      if (body === null) throw new Error('invalid raw protobuf payload');
   112	      metrics.record(body);
   113	    } catch (error) {
   114	      metrics.recordError();
   115	      console.error(`raw command loop failed: ${error.message}`);
   116	    }
   117	  }

L0 receive/reply: zlink-framework-server/main.js:93-155
    93	  function pump() {
    94	    let handled = 0;
    95	    while (handled < batch) {
    96	      let hasMessage;
    97	      try {
    98	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    99	      } catch (error) {
   100	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   101	        metrics.recordError();
   102	        console.error(`ladder L0 recv failed: ${error.message}`);
   103	        break;
   104	      }
   105	      if (!hasMessage) break;
   106	      handled += 1;
   107	      const parts = received.parts;
   108	      try {
   109	        const header = frameworkWire ? parts[0].data() : null;
   110	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   111	        if (payload === null) throw new Error('invalid benchmark payload');
   112	        if (received.replyToken !== null) {
   113	          echoHandler.handleL0(payload);
   114	          received.reply()
   115	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
   116	            .message(encodeBenchPayloadMessage(payload))
   117	            .submit();
   118	        } else {
   119	          commandHandler.handleL0(payload);
   120	        }
   121	      } catch (error) {
   122	        metrics.recordError();
   123	        console.error(`ladder L0 handler failed: ${error.message}`);
   124	      }
   125	    }
   126	    return handled;
   127	  }
   128	
   129	  function pumpCommands() {
   130	    if (commandRouter === null) return 0;
   131	    let handled = 0;
   132	    while (handled < batch) {
   133	      let hasMessage;
   134	      try {
   135	        hasMessage = commandRouter.recv(commandReceived, zlink.RecvFlags.DontWait);
   136	      } catch (error) {
   137	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   138	        metrics.recordError();
   139	        console.error(`ladder L0 command recv failed: ${error.message}`);
   140	        break;
   141	      }
   142	      if (!hasMessage) break;
   143	      handled += 1;
   144	      try {
   145	        const parts = commandReceived.parts;
   146	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   147	        if (payload === null) throw new Error('invalid benchmark payload');
   148	        commandHandler.handleL0(payload);
   149	      } catch (error) {
   150	        metrics.recordError();
   151	        console.error(`ladder L0 command handler failed: ${error.message}`);
   152	      }
   153	    }
   154	    return handled;
   155	  }

L1 permit receive/reply: zlink-framework-server/main.js:157-214
   157	  async function pumpWithPermits(socket, targetReceived, processMessage, label) {
   158	    let handled = 0;
   159	    while (handled < batch) {
   160	      const permit = await applicationJobQueue.acquire(undefined, 'remote');
   161	      let hasMessage;
   162	      try {
   163	        hasMessage = socket.recv(targetReceived, zlink.RecvFlags.DontWait);
   164	      } catch (error) {
   165	        permit.releaseUnqueued();
   166	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   167	        metrics.recordError();
   168	        console.error(`ladder L1 ${label} recv failed: ${error.message}`);
   169	        break;
   170	      }
   171	      if (!hasMessage) {
   172	        permit.releaseUnqueued();
   173	        break;
   174	      }
   175	      handled += 1;
   176	      try {
   177	        processMessage(permit);
   178	      } catch (error) {
   179	        permit.releaseAfterInternalProcessing();
   180	        metrics.recordError();
   181	        console.error(`ladder L1 ${label} handler failed: ${error.message}`);
   182	      }
   183	    }
   184	    return handled;
   185	  }
   186	
   187	  async function pumpRequestsWithPermits() {
   188	    return pumpWithPermits(router, received, (permit) => {
   189	      const parts = received.parts;
   190	      const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   191	      if (payload === null) throw new Error('invalid benchmark payload');
   192	      if (received.replyToken !== null) {
   193	        permit.releaseBeforeHandler();
   194	        echoHandler.handleL0(payload);
   195	        received.reply()
   196	          .message(RESPONSE_ENVELOPE)
   197	          .message(encodeBenchPayloadMessage(payload))
   198	          .submit();
   199	      } else {
   200	        permit.releaseBeforeHandler();
   201	        commandHandler.handleL0(payload);
   202	      }
   203	    }, 'request');
   204	  }
   205	
   206	  async function pumpCommandsWithPermits() {
   207	    return pumpWithPermits(commandRouter, commandReceived, (permit) => {
   208	      const parts = commandReceived.parts;
   209	      const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   210	      if (payload === null) throw new Error('invalid benchmark payload');
   211	      permit.releaseBeforeHandler();
   212	      commandHandler.handleL0(payload);
   213	    }, 'command');
   214	  }

Per-message waits/allocations/atomics/copies: binding and L0/L0R use one reusable Received, DontWait, batch 256, setImmediate; no queue wait, no JS atomics. L1 awaits ApplicationJobQueue.acquire(remote) before each receive and releases at handler entry. All paths decode the protobuf body, allocate a new encoded body and reply operation, and submit two parts; they do not copy or reuse request parts.

Run start L1 send-saturation-4096 r3: 2026-11-13T16:38:46Z

First result L1 send-saturation-4096 r3: 88820.4 msg/s; errors=0 submitted=completed=received=444772. Raw: raw/session/confirm/send-saturation-4096/L1/run3/run1/zlink-framework-node-send-saturation-4096/results.json.

## Before confirm-L0-request-serial-1024-r1
built commit=c512887606aeb686eccc53162d9f02c3c4db63ed dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=d6db0481dc0af4f77e2fc19b8227b478e1a2539cca55d353eb7118106b1139e3
framework/languages/node/packages/framework/dist/runtime/host/application-job-queue.js sha256=07e9734c0f3466cdd3d174518f2b1d9fa357bef66b169a6d699aa6270493fa27
framework/languages/node/packages/framework/dist/index.js sha256=4aedc33b29681b11d37221bf122fd9fe882eb3b005a9bdb173676ec8dbe85ba9
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae

Binding receive/reply: zlink-raw-server/main.js:64-117
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
    91	  }
    92	  return handled;
    93	}
    94	
    95	function pumpCommands() {
    96	  let handled = 0;
    97	  while (handled < batch) {
    98	    let hasMessage;
    99	    try {
   100	      hasMessage = tryRecv(commandRouter, commandReceived);
   101	    } catch (error) {
   102	      metrics.recordError();
   103	      console.error(`raw command loop recv failed: ${error.message}`);
   104	      break;
   105	    }
   106	    if (!hasMessage) break;
   107	    handled += 1;
   108	    try {
   109	      const part = bodyPart(commandReceived);
   110	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
   111	      if (body === null) throw new Error('invalid raw protobuf payload');
   112	      metrics.record(body);
   113	    } catch (error) {
   114	      metrics.recordError();
   115	      console.error(`raw command loop failed: ${error.message}`);
   116	    }
   117	  }

L0 receive/reply: zlink-framework-server/main.js:93-155
    93	  function pump() {
    94	    let handled = 0;
    95	    while (handled < batch) {
    96	      let hasMessage;
    97	      try {
    98	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    99	      } catch (error) {
   100	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   101	        metrics.recordError();
   102	        console.error(`ladder L0 recv failed: ${error.message}`);
   103	        break;
   104	      }
   105	      if (!hasMessage) break;
   106	      handled += 1;
   107	      const parts = received.parts;
   108	      try {
   109	        const header = frameworkWire ? parts[0].data() : null;
   110	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   111	        if (payload === null) throw new Error('invalid benchmark payload');
   112	        if (received.replyToken !== null) {
   113	          echoHandler.handleL0(payload);
   114	          received.reply()
   115	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
   116	            .message(encodeBenchPayloadMessage(payload))
   117	            .submit();
   118	        } else {
   119	          commandHandler.handleL0(payload);
   120	        }
   121	      } catch (error) {
   122	        metrics.recordError();
   123	        console.error(`ladder L0 handler failed: ${error.message}`);
   124	      }
   125	    }
   126	    return handled;
   127	  }
   128	
   129	  function pumpCommands() {
   130	    if (commandRouter === null) return 0;
   131	    let handled = 0;
   132	    while (handled < batch) {
   133	      let hasMessage;
   134	      try {
   135	        hasMessage = commandRouter.recv(commandReceived, zlink.RecvFlags.DontWait);
   136	      } catch (error) {
   137	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   138	        metrics.recordError();
   139	        console.error(`ladder L0 command recv failed: ${error.message}`);
   140	        break;
   141	      }
   142	      if (!hasMessage) break;
   143	      handled += 1;
   144	      try {
   145	        const parts = commandReceived.parts;
   146	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   147	        if (payload === null) throw new Error('invalid benchmark payload');
   148	        commandHandler.handleL0(payload);
   149	      } catch (error) {
   150	        metrics.recordError();
   151	        console.error(`ladder L0 command handler failed: ${error.message}`);
   152	      }
   153	    }
   154	    return handled;
   155	  }

Per-message waits/allocations/atomics/copies: binding and L0/L0R use one reusable Received, DontWait, batch 256, setImmediate; no queue wait, no JS atomics. L1 awaits ApplicationJobQueue.acquire(remote) before each receive and releases at handler entry. All paths decode the protobuf body, allocate a new encoded body and reply operation, and submit two parts; they do not copy or reuse request parts.

Run start L0 request-serial-1024 r1: 2026-11-13T16:39:01Z

First result L0 request-serial-1024 r1: 7495.2 msg/s; errors=0 submitted=completed=received=37477. Raw: raw/session/confirm/request-serial-1024/L0/run1/run1/zlink-framework-node-request-serial-1024/results.json.

## Before confirm-L0R-request-serial-1024-r1
built commit=c512887606aeb686eccc53162d9f02c3c4db63ed dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=d6db0481dc0af4f77e2fc19b8227b478e1a2539cca55d353eb7118106b1139e3
framework/languages/node/packages/framework/dist/runtime/host/application-job-queue.js sha256=07e9734c0f3466cdd3d174518f2b1d9fa357bef66b169a6d699aa6270493fa27
framework/languages/node/packages/framework/dist/index.js sha256=4aedc33b29681b11d37221bf122fd9fe882eb3b005a9bdb173676ec8dbe85ba9
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae

Binding receive/reply: zlink-raw-server/main.js:64-117
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
    91	  }
    92	  return handled;
    93	}
    94	
    95	function pumpCommands() {
    96	  let handled = 0;
    97	  while (handled < batch) {
    98	    let hasMessage;
    99	    try {
   100	      hasMessage = tryRecv(commandRouter, commandReceived);
   101	    } catch (error) {
   102	      metrics.recordError();
   103	      console.error(`raw command loop recv failed: ${error.message}`);
   104	      break;
   105	    }
   106	    if (!hasMessage) break;
   107	    handled += 1;
   108	    try {
   109	      const part = bodyPart(commandReceived);
   110	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
   111	      if (body === null) throw new Error('invalid raw protobuf payload');
   112	      metrics.record(body);
   113	    } catch (error) {
   114	      metrics.recordError();
   115	      console.error(`raw command loop failed: ${error.message}`);
   116	    }
   117	  }

L0 receive/reply: zlink-framework-server/main.js:93-155
    93	  function pump() {
    94	    let handled = 0;
    95	    while (handled < batch) {
    96	      let hasMessage;
    97	      try {
    98	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    99	      } catch (error) {
   100	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   101	        metrics.recordError();
   102	        console.error(`ladder L0 recv failed: ${error.message}`);
   103	        break;
   104	      }
   105	      if (!hasMessage) break;
   106	      handled += 1;
   107	      const parts = received.parts;
   108	      try {
   109	        const header = frameworkWire ? parts[0].data() : null;
   110	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   111	        if (payload === null) throw new Error('invalid benchmark payload');
   112	        if (received.replyToken !== null) {
   113	          echoHandler.handleL0(payload);
   114	          received.reply()
   115	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
   116	            .message(encodeBenchPayloadMessage(payload))
   117	            .submit();
   118	        } else {
   119	          commandHandler.handleL0(payload);
   120	        }
   121	      } catch (error) {
   122	        metrics.recordError();
   123	        console.error(`ladder L0 handler failed: ${error.message}`);
   124	      }
   125	    }
   126	    return handled;
   127	  }
   128	
   129	  function pumpCommands() {
   130	    if (commandRouter === null) return 0;
   131	    let handled = 0;
   132	    while (handled < batch) {
   133	      let hasMessage;
   134	      try {
   135	        hasMessage = commandRouter.recv(commandReceived, zlink.RecvFlags.DontWait);
   136	      } catch (error) {
   137	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   138	        metrics.recordError();
   139	        console.error(`ladder L0 command recv failed: ${error.message}`);
   140	        break;
   141	      }
   142	      if (!hasMessage) break;
   143	      handled += 1;
   144	      try {
   145	        const parts = commandReceived.parts;
   146	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   147	        if (payload === null) throw new Error('invalid benchmark payload');
   148	        commandHandler.handleL0(payload);
   149	      } catch (error) {
   150	        metrics.recordError();
   151	        console.error(`ladder L0 command handler failed: ${error.message}`);
   152	      }
   153	    }
   154	    return handled;
   155	  }

Per-message waits/allocations/atomics/copies: binding and L0/L0R use one reusable Received, DontWait, batch 256, setImmediate; no queue wait, no JS atomics. L1 awaits ApplicationJobQueue.acquire(remote) before each receive and releases at handler entry. All paths decode the protobuf body, allocate a new encoded body and reply operation, and submit two parts; they do not copy or reuse request parts.

Run start L0R request-serial-1024 r1: 2026-11-13T16:39:29Z

First result L0R request-serial-1024 r1: 7827.6 msg/s; errors=0 submitted=completed=received=39139. Raw: raw/session/confirm/request-serial-1024/L0R/run1/run1/zlink-framework-node-request-serial-1024/results.json.

## Before confirm-L1-request-serial-1024-r1
built commit=c512887606aeb686eccc53162d9f02c3c4db63ed dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=d6db0481dc0af4f77e2fc19b8227b478e1a2539cca55d353eb7118106b1139e3
framework/languages/node/packages/framework/dist/runtime/host/application-job-queue.js sha256=07e9734c0f3466cdd3d174518f2b1d9fa357bef66b169a6d699aa6270493fa27
framework/languages/node/packages/framework/dist/index.js sha256=4aedc33b29681b11d37221bf122fd9fe882eb3b005a9bdb173676ec8dbe85ba9
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae

Binding receive/reply: zlink-raw-server/main.js:64-117
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
    91	  }
    92	  return handled;
    93	}
    94	
    95	function pumpCommands() {
    96	  let handled = 0;
    97	  while (handled < batch) {
    98	    let hasMessage;
    99	    try {
   100	      hasMessage = tryRecv(commandRouter, commandReceived);
   101	    } catch (error) {
   102	      metrics.recordError();
   103	      console.error(`raw command loop recv failed: ${error.message}`);
   104	      break;
   105	    }
   106	    if (!hasMessage) break;
   107	    handled += 1;
   108	    try {
   109	      const part = bodyPart(commandReceived);
   110	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
   111	      if (body === null) throw new Error('invalid raw protobuf payload');
   112	      metrics.record(body);
   113	    } catch (error) {
   114	      metrics.recordError();
   115	      console.error(`raw command loop failed: ${error.message}`);
   116	    }
   117	  }

L0 receive/reply: zlink-framework-server/main.js:93-155
    93	  function pump() {
    94	    let handled = 0;
    95	    while (handled < batch) {
    96	      let hasMessage;
    97	      try {
    98	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    99	      } catch (error) {
   100	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   101	        metrics.recordError();
   102	        console.error(`ladder L0 recv failed: ${error.message}`);
   103	        break;
   104	      }
   105	      if (!hasMessage) break;
   106	      handled += 1;
   107	      const parts = received.parts;
   108	      try {
   109	        const header = frameworkWire ? parts[0].data() : null;
   110	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   111	        if (payload === null) throw new Error('invalid benchmark payload');
   112	        if (received.replyToken !== null) {
   113	          echoHandler.handleL0(payload);
   114	          received.reply()
   115	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
   116	            .message(encodeBenchPayloadMessage(payload))
   117	            .submit();
   118	        } else {
   119	          commandHandler.handleL0(payload);
   120	        }
   121	      } catch (error) {
   122	        metrics.recordError();
   123	        console.error(`ladder L0 handler failed: ${error.message}`);
   124	      }
   125	    }
   126	    return handled;
   127	  }
   128	
   129	  function pumpCommands() {
   130	    if (commandRouter === null) return 0;
   131	    let handled = 0;
   132	    while (handled < batch) {
   133	      let hasMessage;
   134	      try {
   135	        hasMessage = commandRouter.recv(commandReceived, zlink.RecvFlags.DontWait);
   136	      } catch (error) {
   137	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   138	        metrics.recordError();
   139	        console.error(`ladder L0 command recv failed: ${error.message}`);
   140	        break;
   141	      }
   142	      if (!hasMessage) break;
   143	      handled += 1;
   144	      try {
   145	        const parts = commandReceived.parts;
   146	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   147	        if (payload === null) throw new Error('invalid benchmark payload');
   148	        commandHandler.handleL0(payload);
   149	      } catch (error) {
   150	        metrics.recordError();
   151	        console.error(`ladder L0 command handler failed: ${error.message}`);
   152	      }
   153	    }
   154	    return handled;
   155	  }

L1 permit receive/reply: zlink-framework-server/main.js:157-214
   157	  async function pumpWithPermits(socket, targetReceived, processMessage, label) {
   158	    let handled = 0;
   159	    while (handled < batch) {
   160	      const permit = await applicationJobQueue.acquire(undefined, 'remote');
   161	      let hasMessage;
   162	      try {
   163	        hasMessage = socket.recv(targetReceived, zlink.RecvFlags.DontWait);
   164	      } catch (error) {
   165	        permit.releaseUnqueued();
   166	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   167	        metrics.recordError();
   168	        console.error(`ladder L1 ${label} recv failed: ${error.message}`);
   169	        break;
   170	      }
   171	      if (!hasMessage) {
   172	        permit.releaseUnqueued();
   173	        break;
   174	      }
   175	      handled += 1;
   176	      try {
   177	        processMessage(permit);
   178	      } catch (error) {
   179	        permit.releaseAfterInternalProcessing();
   180	        metrics.recordError();
   181	        console.error(`ladder L1 ${label} handler failed: ${error.message}`);
   182	      }
   183	    }
   184	    return handled;
   185	  }
   186	
   187	  async function pumpRequestsWithPermits() {
   188	    return pumpWithPermits(router, received, (permit) => {
   189	      const parts = received.parts;
   190	      const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   191	      if (payload === null) throw new Error('invalid benchmark payload');
   192	      if (received.replyToken !== null) {
   193	        permit.releaseBeforeHandler();
   194	        echoHandler.handleL0(payload);
   195	        received.reply()
   196	          .message(RESPONSE_ENVELOPE)
   197	          .message(encodeBenchPayloadMessage(payload))
   198	          .submit();
   199	      } else {
   200	        permit.releaseBeforeHandler();
   201	        commandHandler.handleL0(payload);
   202	      }
   203	    }, 'request');
   204	  }
   205	
   206	  async function pumpCommandsWithPermits() {
   207	    return pumpWithPermits(commandRouter, commandReceived, (permit) => {
   208	      const parts = commandReceived.parts;
   209	      const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   210	      if (payload === null) throw new Error('invalid benchmark payload');
   211	      permit.releaseBeforeHandler();
   212	      commandHandler.handleL0(payload);
   213	    }, 'command');
   214	  }

Per-message waits/allocations/atomics/copies: binding and L0/L0R use one reusable Received, DontWait, batch 256, setImmediate; no queue wait, no JS atomics. L1 awaits ApplicationJobQueue.acquire(remote) before each receive and releases at handler entry. All paths decode the protobuf body, allocate a new encoded body and reply operation, and submit two parts; they do not copy or reuse request parts.

Run start L1 request-serial-1024 r1: 2026-11-13T16:39:40Z

First result L1 request-serial-1024 r1: 7842.4 msg/s; errors=0 submitted=completed=received=39213. Raw: raw/session/confirm/request-serial-1024/L1/run1/run1/zlink-framework-node-request-serial-1024/results.json.

## Before confirm-L0-request-serial-1024-r2
built commit=c512887606aeb686eccc53162d9f02c3c4db63ed dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=d6db0481dc0af4f77e2fc19b8227b478e1a2539cca55d353eb7118106b1139e3
framework/languages/node/packages/framework/dist/runtime/host/application-job-queue.js sha256=07e9734c0f3466cdd3d174518f2b1d9fa357bef66b169a6d699aa6270493fa27
framework/languages/node/packages/framework/dist/index.js sha256=4aedc33b29681b11d37221bf122fd9fe882eb3b005a9bdb173676ec8dbe85ba9
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae

Binding receive/reply: zlink-raw-server/main.js:64-117
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
    91	  }
    92	  return handled;
    93	}
    94	
    95	function pumpCommands() {
    96	  let handled = 0;
    97	  while (handled < batch) {
    98	    let hasMessage;
    99	    try {
   100	      hasMessage = tryRecv(commandRouter, commandReceived);
   101	    } catch (error) {
   102	      metrics.recordError();
   103	      console.error(`raw command loop recv failed: ${error.message}`);
   104	      break;
   105	    }
   106	    if (!hasMessage) break;
   107	    handled += 1;
   108	    try {
   109	      const part = bodyPart(commandReceived);
   110	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
   111	      if (body === null) throw new Error('invalid raw protobuf payload');
   112	      metrics.record(body);
   113	    } catch (error) {
   114	      metrics.recordError();
   115	      console.error(`raw command loop failed: ${error.message}`);
   116	    }
   117	  }

L0 receive/reply: zlink-framework-server/main.js:93-155
    93	  function pump() {
    94	    let handled = 0;
    95	    while (handled < batch) {
    96	      let hasMessage;
    97	      try {
    98	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    99	      } catch (error) {
   100	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   101	        metrics.recordError();
   102	        console.error(`ladder L0 recv failed: ${error.message}`);
   103	        break;
   104	      }
   105	      if (!hasMessage) break;
   106	      handled += 1;
   107	      const parts = received.parts;
   108	      try {
   109	        const header = frameworkWire ? parts[0].data() : null;
   110	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   111	        if (payload === null) throw new Error('invalid benchmark payload');
   112	        if (received.replyToken !== null) {
   113	          echoHandler.handleL0(payload);
   114	          received.reply()
   115	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
   116	            .message(encodeBenchPayloadMessage(payload))
   117	            .submit();
   118	        } else {
   119	          commandHandler.handleL0(payload);
   120	        }
   121	      } catch (error) {
   122	        metrics.recordError();
   123	        console.error(`ladder L0 handler failed: ${error.message}`);
   124	      }
   125	    }
   126	    return handled;
   127	  }
   128	
   129	  function pumpCommands() {
   130	    if (commandRouter === null) return 0;
   131	    let handled = 0;
   132	    while (handled < batch) {
   133	      let hasMessage;
   134	      try {
   135	        hasMessage = commandRouter.recv(commandReceived, zlink.RecvFlags.DontWait);
   136	      } catch (error) {
   137	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   138	        metrics.recordError();
   139	        console.error(`ladder L0 command recv failed: ${error.message}`);
   140	        break;
   141	      }
   142	      if (!hasMessage) break;
   143	      handled += 1;
   144	      try {
   145	        const parts = commandReceived.parts;
   146	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   147	        if (payload === null) throw new Error('invalid benchmark payload');
   148	        commandHandler.handleL0(payload);
   149	      } catch (error) {
   150	        metrics.recordError();
   151	        console.error(`ladder L0 command handler failed: ${error.message}`);
   152	      }
   153	    }
   154	    return handled;
   155	  }

Per-message waits/allocations/atomics/copies: binding and L0/L0R use one reusable Received, DontWait, batch 256, setImmediate; no queue wait, no JS atomics. L1 awaits ApplicationJobQueue.acquire(remote) before each receive and releases at handler entry. All paths decode the protobuf body, allocate a new encoded body and reply operation, and submit two parts; they do not copy or reuse request parts.

Run start L0 request-serial-1024 r2: 2026-11-13T16:39:55Z

First result L0 request-serial-1024 r2: 8458.2 msg/s; errors=0 submitted=completed=received=42292. Raw: raw/session/confirm/request-serial-1024/L0/run2/run1/zlink-framework-node-request-serial-1024/results.json.

## Before confirm-L0R-request-serial-1024-r2
built commit=c512887606aeb686eccc53162d9f02c3c4db63ed dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=d6db0481dc0af4f77e2fc19b8227b478e1a2539cca55d353eb7118106b1139e3
framework/languages/node/packages/framework/dist/runtime/host/application-job-queue.js sha256=07e9734c0f3466cdd3d174518f2b1d9fa357bef66b169a6d699aa6270493fa27
framework/languages/node/packages/framework/dist/index.js sha256=4aedc33b29681b11d37221bf122fd9fe882eb3b005a9bdb173676ec8dbe85ba9
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae

Binding receive/reply: zlink-raw-server/main.js:64-117
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
    91	  }
    92	  return handled;
    93	}
    94	
    95	function pumpCommands() {
    96	  let handled = 0;
    97	  while (handled < batch) {
    98	    let hasMessage;
    99	    try {
   100	      hasMessage = tryRecv(commandRouter, commandReceived);
   101	    } catch (error) {
   102	      metrics.recordError();
   103	      console.error(`raw command loop recv failed: ${error.message}`);
   104	      break;
   105	    }
   106	    if (!hasMessage) break;
   107	    handled += 1;
   108	    try {
   109	      const part = bodyPart(commandReceived);
   110	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
   111	      if (body === null) throw new Error('invalid raw protobuf payload');
   112	      metrics.record(body);
   113	    } catch (error) {
   114	      metrics.recordError();
   115	      console.error(`raw command loop failed: ${error.message}`);
   116	    }
   117	  }

L0 receive/reply: zlink-framework-server/main.js:93-155
    93	  function pump() {
    94	    let handled = 0;
    95	    while (handled < batch) {
    96	      let hasMessage;
    97	      try {
    98	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    99	      } catch (error) {
   100	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   101	        metrics.recordError();
   102	        console.error(`ladder L0 recv failed: ${error.message}`);
   103	        break;
   104	      }
   105	      if (!hasMessage) break;
   106	      handled += 1;
   107	      const parts = received.parts;
   108	      try {
   109	        const header = frameworkWire ? parts[0].data() : null;
   110	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   111	        if (payload === null) throw new Error('invalid benchmark payload');
   112	        if (received.replyToken !== null) {
   113	          echoHandler.handleL0(payload);
   114	          received.reply()
   115	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
   116	            .message(encodeBenchPayloadMessage(payload))
   117	            .submit();
   118	        } else {
   119	          commandHandler.handleL0(payload);
   120	        }
   121	      } catch (error) {
   122	        metrics.recordError();
   123	        console.error(`ladder L0 handler failed: ${error.message}`);
   124	      }
   125	    }
   126	    return handled;
   127	  }
   128	
   129	  function pumpCommands() {
   130	    if (commandRouter === null) return 0;
   131	    let handled = 0;
   132	    while (handled < batch) {
   133	      let hasMessage;
   134	      try {
   135	        hasMessage = commandRouter.recv(commandReceived, zlink.RecvFlags.DontWait);
   136	      } catch (error) {
   137	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   138	        metrics.recordError();
   139	        console.error(`ladder L0 command recv failed: ${error.message}`);
   140	        break;
   141	      }
   142	      if (!hasMessage) break;
   143	      handled += 1;
   144	      try {
   145	        const parts = commandReceived.parts;
   146	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   147	        if (payload === null) throw new Error('invalid benchmark payload');
   148	        commandHandler.handleL0(payload);
   149	      } catch (error) {
   150	        metrics.recordError();
   151	        console.error(`ladder L0 command handler failed: ${error.message}`);
   152	      }
   153	    }
   154	    return handled;
   155	  }

Per-message waits/allocations/atomics/copies: binding and L0/L0R use one reusable Received, DontWait, batch 256, setImmediate; no queue wait, no JS atomics. L1 awaits ApplicationJobQueue.acquire(remote) before each receive and releases at handler entry. All paths decode the protobuf body, allocate a new encoded body and reply operation, and submit two parts; they do not copy or reuse request parts.

Run start L0R request-serial-1024 r2: 2026-11-13T16:40:05Z

First result L0R request-serial-1024 r2: 4803.6 msg/s; errors=0 submitted=completed=received=24019. Raw: raw/session/confirm/request-serial-1024/L0R/run2/run1/zlink-framework-node-request-serial-1024/results.json.

## Before confirm-L1-request-serial-1024-r2
built commit=c512887606aeb686eccc53162d9f02c3c4db63ed dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=d6db0481dc0af4f77e2fc19b8227b478e1a2539cca55d353eb7118106b1139e3
framework/languages/node/packages/framework/dist/runtime/host/application-job-queue.js sha256=07e9734c0f3466cdd3d174518f2b1d9fa357bef66b169a6d699aa6270493fa27
framework/languages/node/packages/framework/dist/index.js sha256=4aedc33b29681b11d37221bf122fd9fe882eb3b005a9bdb173676ec8dbe85ba9
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae

Binding receive/reply: zlink-raw-server/main.js:64-117
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
    91	  }
    92	  return handled;
    93	}
    94	
    95	function pumpCommands() {
    96	  let handled = 0;
    97	  while (handled < batch) {
    98	    let hasMessage;
    99	    try {
   100	      hasMessage = tryRecv(commandRouter, commandReceived);
   101	    } catch (error) {
   102	      metrics.recordError();
   103	      console.error(`raw command loop recv failed: ${error.message}`);
   104	      break;
   105	    }
   106	    if (!hasMessage) break;
   107	    handled += 1;
   108	    try {
   109	      const part = bodyPart(commandReceived);
   110	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
   111	      if (body === null) throw new Error('invalid raw protobuf payload');
   112	      metrics.record(body);
   113	    } catch (error) {
   114	      metrics.recordError();
   115	      console.error(`raw command loop failed: ${error.message}`);
   116	    }
   117	  }

L0 receive/reply: zlink-framework-server/main.js:93-155
    93	  function pump() {
    94	    let handled = 0;
    95	    while (handled < batch) {
    96	      let hasMessage;
    97	      try {
    98	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    99	      } catch (error) {
   100	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   101	        metrics.recordError();
   102	        console.error(`ladder L0 recv failed: ${error.message}`);
   103	        break;
   104	      }
   105	      if (!hasMessage) break;
   106	      handled += 1;
   107	      const parts = received.parts;
   108	      try {
   109	        const header = frameworkWire ? parts[0].data() : null;
   110	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   111	        if (payload === null) throw new Error('invalid benchmark payload');
   112	        if (received.replyToken !== null) {
   113	          echoHandler.handleL0(payload);
   114	          received.reply()
   115	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
   116	            .message(encodeBenchPayloadMessage(payload))
   117	            .submit();
   118	        } else {
   119	          commandHandler.handleL0(payload);
   120	        }
   121	      } catch (error) {
   122	        metrics.recordError();
   123	        console.error(`ladder L0 handler failed: ${error.message}`);
   124	      }
   125	    }
   126	    return handled;
   127	  }
   128	
   129	  function pumpCommands() {
   130	    if (commandRouter === null) return 0;
   131	    let handled = 0;
   132	    while (handled < batch) {
   133	      let hasMessage;
   134	      try {
   135	        hasMessage = commandRouter.recv(commandReceived, zlink.RecvFlags.DontWait);
   136	      } catch (error) {
   137	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   138	        metrics.recordError();
   139	        console.error(`ladder L0 command recv failed: ${error.message}`);
   140	        break;
   141	      }
   142	      if (!hasMessage) break;
   143	      handled += 1;
   144	      try {
   145	        const parts = commandReceived.parts;
   146	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   147	        if (payload === null) throw new Error('invalid benchmark payload');
   148	        commandHandler.handleL0(payload);
   149	      } catch (error) {
   150	        metrics.recordError();
   151	        console.error(`ladder L0 command handler failed: ${error.message}`);
   152	      }
   153	    }
   154	    return handled;
   155	  }

L1 permit receive/reply: zlink-framework-server/main.js:157-214
   157	  async function pumpWithPermits(socket, targetReceived, processMessage, label) {
   158	    let handled = 0;
   159	    while (handled < batch) {
   160	      const permit = await applicationJobQueue.acquire(undefined, 'remote');
   161	      let hasMessage;
   162	      try {
   163	        hasMessage = socket.recv(targetReceived, zlink.RecvFlags.DontWait);
   164	      } catch (error) {
   165	        permit.releaseUnqueued();
   166	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   167	        metrics.recordError();
   168	        console.error(`ladder L1 ${label} recv failed: ${error.message}`);
   169	        break;
   170	      }
   171	      if (!hasMessage) {
   172	        permit.releaseUnqueued();
   173	        break;
   174	      }
   175	      handled += 1;
   176	      try {
   177	        processMessage(permit);
   178	      } catch (error) {
   179	        permit.releaseAfterInternalProcessing();
   180	        metrics.recordError();
   181	        console.error(`ladder L1 ${label} handler failed: ${error.message}`);
   182	      }
   183	    }
   184	    return handled;
   185	  }
   186	
   187	  async function pumpRequestsWithPermits() {
   188	    return pumpWithPermits(router, received, (permit) => {
   189	      const parts = received.parts;
   190	      const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   191	      if (payload === null) throw new Error('invalid benchmark payload');
   192	      if (received.replyToken !== null) {
   193	        permit.releaseBeforeHandler();
   194	        echoHandler.handleL0(payload);
   195	        received.reply()
   196	          .message(RESPONSE_ENVELOPE)
   197	          .message(encodeBenchPayloadMessage(payload))
   198	          .submit();
   199	      } else {
   200	        permit.releaseBeforeHandler();
   201	        commandHandler.handleL0(payload);
   202	      }
   203	    }, 'request');
   204	  }
   205	
   206	  async function pumpCommandsWithPermits() {
   207	    return pumpWithPermits(commandRouter, commandReceived, (permit) => {
   208	      const parts = commandReceived.parts;
   209	      const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   210	      if (payload === null) throw new Error('invalid benchmark payload');
   211	      permit.releaseBeforeHandler();
   212	      commandHandler.handleL0(payload);
   213	    }, 'command');
   214	  }

Per-message waits/allocations/atomics/copies: binding and L0/L0R use one reusable Received, DontWait, batch 256, setImmediate; no queue wait, no JS atomics. L1 awaits ApplicationJobQueue.acquire(remote) before each receive and releases at handler entry. All paths decode the protobuf body, allocate a new encoded body and reply operation, and submit two parts; they do not copy or reuse request parts.

Run start L1 request-serial-1024 r2: 2026-11-13T16:40:16Z

First result L1 request-serial-1024 r2: 8167.6 msg/s; errors=0 submitted=completed=received=40839. Raw: raw/session/confirm/request-serial-1024/L1/run2/run1/zlink-framework-node-request-serial-1024/results.json.

## Before confirm-L0-request-serial-1024-r3
built commit=c512887606aeb686eccc53162d9f02c3c4db63ed dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=d6db0481dc0af4f77e2fc19b8227b478e1a2539cca55d353eb7118106b1139e3
framework/languages/node/packages/framework/dist/runtime/host/application-job-queue.js sha256=07e9734c0f3466cdd3d174518f2b1d9fa357bef66b169a6d699aa6270493fa27
framework/languages/node/packages/framework/dist/index.js sha256=4aedc33b29681b11d37221bf122fd9fe882eb3b005a9bdb173676ec8dbe85ba9
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae

Binding receive/reply: zlink-raw-server/main.js:64-117
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
    91	  }
    92	  return handled;
    93	}
    94	
    95	function pumpCommands() {
    96	  let handled = 0;
    97	  while (handled < batch) {
    98	    let hasMessage;
    99	    try {
   100	      hasMessage = tryRecv(commandRouter, commandReceived);
   101	    } catch (error) {
   102	      metrics.recordError();
   103	      console.error(`raw command loop recv failed: ${error.message}`);
   104	      break;
   105	    }
   106	    if (!hasMessage) break;
   107	    handled += 1;
   108	    try {
   109	      const part = bodyPart(commandReceived);
   110	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
   111	      if (body === null) throw new Error('invalid raw protobuf payload');
   112	      metrics.record(body);
   113	    } catch (error) {
   114	      metrics.recordError();
   115	      console.error(`raw command loop failed: ${error.message}`);
   116	    }
   117	  }

L0 receive/reply: zlink-framework-server/main.js:93-155
    93	  function pump() {
    94	    let handled = 0;
    95	    while (handled < batch) {
    96	      let hasMessage;
    97	      try {
    98	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    99	      } catch (error) {
   100	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   101	        metrics.recordError();
   102	        console.error(`ladder L0 recv failed: ${error.message}`);
   103	        break;
   104	      }
   105	      if (!hasMessage) break;
   106	      handled += 1;
   107	      const parts = received.parts;
   108	      try {
   109	        const header = frameworkWire ? parts[0].data() : null;
   110	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   111	        if (payload === null) throw new Error('invalid benchmark payload');
   112	        if (received.replyToken !== null) {
   113	          echoHandler.handleL0(payload);
   114	          received.reply()
   115	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
   116	            .message(encodeBenchPayloadMessage(payload))
   117	            .submit();
   118	        } else {
   119	          commandHandler.handleL0(payload);
   120	        }
   121	      } catch (error) {
   122	        metrics.recordError();
   123	        console.error(`ladder L0 handler failed: ${error.message}`);
   124	      }
   125	    }
   126	    return handled;
   127	  }
   128	
   129	  function pumpCommands() {
   130	    if (commandRouter === null) return 0;
   131	    let handled = 0;
   132	    while (handled < batch) {
   133	      let hasMessage;
   134	      try {
   135	        hasMessage = commandRouter.recv(commandReceived, zlink.RecvFlags.DontWait);
   136	      } catch (error) {
   137	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   138	        metrics.recordError();
   139	        console.error(`ladder L0 command recv failed: ${error.message}`);
   140	        break;
   141	      }
   142	      if (!hasMessage) break;
   143	      handled += 1;
   144	      try {
   145	        const parts = commandReceived.parts;
   146	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   147	        if (payload === null) throw new Error('invalid benchmark payload');
   148	        commandHandler.handleL0(payload);
   149	      } catch (error) {
   150	        metrics.recordError();
   151	        console.error(`ladder L0 command handler failed: ${error.message}`);
   152	      }
   153	    }
   154	    return handled;
   155	  }

Per-message waits/allocations/atomics/copies: binding and L0/L0R use one reusable Received, DontWait, batch 256, setImmediate; no queue wait, no JS atomics. L1 awaits ApplicationJobQueue.acquire(remote) before each receive and releases at handler entry. All paths decode the protobuf body, allocate a new encoded body and reply operation, and submit two parts; they do not copy or reuse request parts.

Run start L0 request-serial-1024 r3: 2026-11-13T16:40:34Z

First result L0 request-serial-1024 r3: 7939.6 msg/s; errors=0 submitted=completed=received=39699. Raw: raw/session/confirm/request-serial-1024/L0/run3/run1/zlink-framework-node-request-serial-1024/results.json.

## Before confirm-L0R-request-serial-1024-r3
built commit=c512887606aeb686eccc53162d9f02c3c4db63ed dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=d6db0481dc0af4f77e2fc19b8227b478e1a2539cca55d353eb7118106b1139e3
framework/languages/node/packages/framework/dist/runtime/host/application-job-queue.js sha256=07e9734c0f3466cdd3d174518f2b1d9fa357bef66b169a6d699aa6270493fa27
framework/languages/node/packages/framework/dist/index.js sha256=4aedc33b29681b11d37221bf122fd9fe882eb3b005a9bdb173676ec8dbe85ba9
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae

Binding receive/reply: zlink-raw-server/main.js:64-117
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
    91	  }
    92	  return handled;
    93	}
    94	
    95	function pumpCommands() {
    96	  let handled = 0;
    97	  while (handled < batch) {
    98	    let hasMessage;
    99	    try {
   100	      hasMessage = tryRecv(commandRouter, commandReceived);
   101	    } catch (error) {
   102	      metrics.recordError();
   103	      console.error(`raw command loop recv failed: ${error.message}`);
   104	      break;
   105	    }
   106	    if (!hasMessage) break;
   107	    handled += 1;
   108	    try {
   109	      const part = bodyPart(commandReceived);
   110	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
   111	      if (body === null) throw new Error('invalid raw protobuf payload');
   112	      metrics.record(body);
   113	    } catch (error) {
   114	      metrics.recordError();
   115	      console.error(`raw command loop failed: ${error.message}`);
   116	    }
   117	  }

L0 receive/reply: zlink-framework-server/main.js:93-155
    93	  function pump() {
    94	    let handled = 0;
    95	    while (handled < batch) {
    96	      let hasMessage;
    97	      try {
    98	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    99	      } catch (error) {
   100	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   101	        metrics.recordError();
   102	        console.error(`ladder L0 recv failed: ${error.message}`);
   103	        break;
   104	      }
   105	      if (!hasMessage) break;
   106	      handled += 1;
   107	      const parts = received.parts;
   108	      try {
   109	        const header = frameworkWire ? parts[0].data() : null;
   110	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   111	        if (payload === null) throw new Error('invalid benchmark payload');
   112	        if (received.replyToken !== null) {
   113	          echoHandler.handleL0(payload);
   114	          received.reply()
   115	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
   116	            .message(encodeBenchPayloadMessage(payload))
   117	            .submit();
   118	        } else {
   119	          commandHandler.handleL0(payload);
   120	        }
   121	      } catch (error) {
   122	        metrics.recordError();
   123	        console.error(`ladder L0 handler failed: ${error.message}`);
   124	      }
   125	    }
   126	    return handled;
   127	  }
   128	
   129	  function pumpCommands() {
   130	    if (commandRouter === null) return 0;
   131	    let handled = 0;
   132	    while (handled < batch) {
   133	      let hasMessage;
   134	      try {
   135	        hasMessage = commandRouter.recv(commandReceived, zlink.RecvFlags.DontWait);
   136	      } catch (error) {
   137	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   138	        metrics.recordError();
   139	        console.error(`ladder L0 command recv failed: ${error.message}`);
   140	        break;
   141	      }
   142	      if (!hasMessage) break;
   143	      handled += 1;
   144	      try {
   145	        const parts = commandReceived.parts;
   146	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   147	        if (payload === null) throw new Error('invalid benchmark payload');
   148	        commandHandler.handleL0(payload);
   149	      } catch (error) {
   150	        metrics.recordError();
   151	        console.error(`ladder L0 command handler failed: ${error.message}`);
   152	      }
   153	    }
   154	    return handled;
   155	  }

Per-message waits/allocations/atomics/copies: binding and L0/L0R use one reusable Received, DontWait, batch 256, setImmediate; no queue wait, no JS atomics. L1 awaits ApplicationJobQueue.acquire(remote) before each receive and releases at handler entry. All paths decode the protobuf body, allocate a new encoded body and reply operation, and submit two parts; they do not copy or reuse request parts.

Run start L0R request-serial-1024 r3: 2026-11-13T16:40:49Z

First result L0R request-serial-1024 r3: 7744 msg/s; errors=0 submitted=completed=received=38721. Raw: raw/session/confirm/request-serial-1024/L0R/run3/run1/zlink-framework-node-request-serial-1024/results.json.

## Before confirm-L1-request-serial-1024-r3
built commit=c512887606aeb686eccc53162d9f02c3c4db63ed dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=d6db0481dc0af4f77e2fc19b8227b478e1a2539cca55d353eb7118106b1139e3
framework/languages/node/packages/framework/dist/runtime/host/application-job-queue.js sha256=07e9734c0f3466cdd3d174518f2b1d9fa357bef66b169a6d699aa6270493fa27
framework/languages/node/packages/framework/dist/index.js sha256=4aedc33b29681b11d37221bf122fd9fe882eb3b005a9bdb173676ec8dbe85ba9
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae

Binding receive/reply: zlink-raw-server/main.js:64-117
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
    91	  }
    92	  return handled;
    93	}
    94	
    95	function pumpCommands() {
    96	  let handled = 0;
    97	  while (handled < batch) {
    98	    let hasMessage;
    99	    try {
   100	      hasMessage = tryRecv(commandRouter, commandReceived);
   101	    } catch (error) {
   102	      metrics.recordError();
   103	      console.error(`raw command loop recv failed: ${error.message}`);
   104	      break;
   105	    }
   106	    if (!hasMessage) break;
   107	    handled += 1;
   108	    try {
   109	      const part = bodyPart(commandReceived);
   110	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
   111	      if (body === null) throw new Error('invalid raw protobuf payload');
   112	      metrics.record(body);
   113	    } catch (error) {
   114	      metrics.recordError();
   115	      console.error(`raw command loop failed: ${error.message}`);
   116	    }
   117	  }

L0 receive/reply: zlink-framework-server/main.js:93-155
    93	  function pump() {
    94	    let handled = 0;
    95	    while (handled < batch) {
    96	      let hasMessage;
    97	      try {
    98	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    99	      } catch (error) {
   100	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   101	        metrics.recordError();
   102	        console.error(`ladder L0 recv failed: ${error.message}`);
   103	        break;
   104	      }
   105	      if (!hasMessage) break;
   106	      handled += 1;
   107	      const parts = received.parts;
   108	      try {
   109	        const header = frameworkWire ? parts[0].data() : null;
   110	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   111	        if (payload === null) throw new Error('invalid benchmark payload');
   112	        if (received.replyToken !== null) {
   113	          echoHandler.handleL0(payload);
   114	          received.reply()
   115	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
   116	            .message(encodeBenchPayloadMessage(payload))
   117	            .submit();
   118	        } else {
   119	          commandHandler.handleL0(payload);
   120	        }
   121	      } catch (error) {
   122	        metrics.recordError();
   123	        console.error(`ladder L0 handler failed: ${error.message}`);
   124	      }
   125	    }
   126	    return handled;
   127	  }
   128	
   129	  function pumpCommands() {
   130	    if (commandRouter === null) return 0;
   131	    let handled = 0;
   132	    while (handled < batch) {
   133	      let hasMessage;
   134	      try {
   135	        hasMessage = commandRouter.recv(commandReceived, zlink.RecvFlags.DontWait);
   136	      } catch (error) {
   137	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   138	        metrics.recordError();
   139	        console.error(`ladder L0 command recv failed: ${error.message}`);
   140	        break;
   141	      }
   142	      if (!hasMessage) break;
   143	      handled += 1;
   144	      try {
   145	        const parts = commandReceived.parts;
   146	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   147	        if (payload === null) throw new Error('invalid benchmark payload');
   148	        commandHandler.handleL0(payload);
   149	      } catch (error) {
   150	        metrics.recordError();
   151	        console.error(`ladder L0 command handler failed: ${error.message}`);
   152	      }
   153	    }
   154	    return handled;
   155	  }

L1 permit receive/reply: zlink-framework-server/main.js:157-214
   157	  async function pumpWithPermits(socket, targetReceived, processMessage, label) {
   158	    let handled = 0;
   159	    while (handled < batch) {
   160	      const permit = await applicationJobQueue.acquire(undefined, 'remote');
   161	      let hasMessage;
   162	      try {
   163	        hasMessage = socket.recv(targetReceived, zlink.RecvFlags.DontWait);
   164	      } catch (error) {
   165	        permit.releaseUnqueued();
   166	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   167	        metrics.recordError();
   168	        console.error(`ladder L1 ${label} recv failed: ${error.message}`);
   169	        break;
   170	      }
   171	      if (!hasMessage) {
   172	        permit.releaseUnqueued();
   173	        break;
   174	      }
   175	      handled += 1;
   176	      try {
   177	        processMessage(permit);
   178	      } catch (error) {
   179	        permit.releaseAfterInternalProcessing();
   180	        metrics.recordError();
   181	        console.error(`ladder L1 ${label} handler failed: ${error.message}`);
   182	      }
   183	    }
   184	    return handled;
   185	  }
   186	
   187	  async function pumpRequestsWithPermits() {
   188	    return pumpWithPermits(router, received, (permit) => {
   189	      const parts = received.parts;
   190	      const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   191	      if (payload === null) throw new Error('invalid benchmark payload');
   192	      if (received.replyToken !== null) {
   193	        permit.releaseBeforeHandler();
   194	        echoHandler.handleL0(payload);
   195	        received.reply()
   196	          .message(RESPONSE_ENVELOPE)
   197	          .message(encodeBenchPayloadMessage(payload))
   198	          .submit();
   199	      } else {
   200	        permit.releaseBeforeHandler();
   201	        commandHandler.handleL0(payload);
   202	      }
   203	    }, 'request');
   204	  }
   205	
   206	  async function pumpCommandsWithPermits() {
   207	    return pumpWithPermits(commandRouter, commandReceived, (permit) => {
   208	      const parts = commandReceived.parts;
   209	      const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   210	      if (payload === null) throw new Error('invalid benchmark payload');
   211	      permit.releaseBeforeHandler();
   212	      commandHandler.handleL0(payload);
   213	    }, 'command');
   214	  }

Per-message waits/allocations/atomics/copies: binding and L0/L0R use one reusable Received, DontWait, batch 256, setImmediate; no queue wait, no JS atomics. L1 awaits ApplicationJobQueue.acquire(remote) before each receive and releases at handler entry. All paths decode the protobuf body, allocate a new encoded body and reply operation, and submit two parts; they do not copy or reuse request parts.

Run start L1 request-serial-1024 r3: 2026-11-13T16:40:59Z

First result L1 request-serial-1024 r3: 7904 msg/s; errors=0 submitted=completed=received=39521. Raw: raw/session/confirm/request-serial-1024/L1/run3/run1/zlink-framework-node-request-serial-1024/results.json.

## Before confirm-L0-request-serial-4096-r1
built commit=c512887606aeb686eccc53162d9f02c3c4db63ed dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=d6db0481dc0af4f77e2fc19b8227b478e1a2539cca55d353eb7118106b1139e3
framework/languages/node/packages/framework/dist/runtime/host/application-job-queue.js sha256=07e9734c0f3466cdd3d174518f2b1d9fa357bef66b169a6d699aa6270493fa27
framework/languages/node/packages/framework/dist/index.js sha256=4aedc33b29681b11d37221bf122fd9fe882eb3b005a9bdb173676ec8dbe85ba9
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae

Binding receive/reply: zlink-raw-server/main.js:64-117
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
    91	  }
    92	  return handled;
    93	}
    94	
    95	function pumpCommands() {
    96	  let handled = 0;
    97	  while (handled < batch) {
    98	    let hasMessage;
    99	    try {
   100	      hasMessage = tryRecv(commandRouter, commandReceived);
   101	    } catch (error) {
   102	      metrics.recordError();
   103	      console.error(`raw command loop recv failed: ${error.message}`);
   104	      break;
   105	    }
   106	    if (!hasMessage) break;
   107	    handled += 1;
   108	    try {
   109	      const part = bodyPart(commandReceived);
   110	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
   111	      if (body === null) throw new Error('invalid raw protobuf payload');
   112	      metrics.record(body);
   113	    } catch (error) {
   114	      metrics.recordError();
   115	      console.error(`raw command loop failed: ${error.message}`);
   116	    }
   117	  }

L0 receive/reply: zlink-framework-server/main.js:93-155
    93	  function pump() {
    94	    let handled = 0;
    95	    while (handled < batch) {
    96	      let hasMessage;
    97	      try {
    98	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    99	      } catch (error) {
   100	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   101	        metrics.recordError();
   102	        console.error(`ladder L0 recv failed: ${error.message}`);
   103	        break;
   104	      }
   105	      if (!hasMessage) break;
   106	      handled += 1;
   107	      const parts = received.parts;
   108	      try {
   109	        const header = frameworkWire ? parts[0].data() : null;
   110	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   111	        if (payload === null) throw new Error('invalid benchmark payload');
   112	        if (received.replyToken !== null) {
   113	          echoHandler.handleL0(payload);
   114	          received.reply()
   115	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
   116	            .message(encodeBenchPayloadMessage(payload))
   117	            .submit();
   118	        } else {
   119	          commandHandler.handleL0(payload);
   120	        }
   121	      } catch (error) {
   122	        metrics.recordError();
   123	        console.error(`ladder L0 handler failed: ${error.message}`);
   124	      }
   125	    }
   126	    return handled;
   127	  }
   128	
   129	  function pumpCommands() {
   130	    if (commandRouter === null) return 0;
   131	    let handled = 0;
   132	    while (handled < batch) {
   133	      let hasMessage;
   134	      try {
   135	        hasMessage = commandRouter.recv(commandReceived, zlink.RecvFlags.DontWait);
   136	      } catch (error) {
   137	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   138	        metrics.recordError();
   139	        console.error(`ladder L0 command recv failed: ${error.message}`);
   140	        break;
   141	      }
   142	      if (!hasMessage) break;
   143	      handled += 1;
   144	      try {
   145	        const parts = commandReceived.parts;
   146	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   147	        if (payload === null) throw new Error('invalid benchmark payload');
   148	        commandHandler.handleL0(payload);
   149	      } catch (error) {
   150	        metrics.recordError();
   151	        console.error(`ladder L0 command handler failed: ${error.message}`);
   152	      }
   153	    }
   154	    return handled;
   155	  }

Per-message waits/allocations/atomics/copies: binding and L0/L0R use one reusable Received, DontWait, batch 256, setImmediate; no queue wait, no JS atomics. L1 awaits ApplicationJobQueue.acquire(remote) before each receive and releases at handler entry. All paths decode the protobuf body, allocate a new encoded body and reply operation, and submit two parts; they do not copy or reuse request parts.

Run start L0 request-serial-4096 r1: 2026-11-13T16:41:10Z

First result L0 request-serial-4096 r1: 8032.4 msg/s; errors=0 submitted=completed=received=40163. Raw: raw/session/confirm/request-serial-4096/L0/run1/run1/zlink-framework-node-request-serial-4096/results.json.

## Before confirm-L0R-request-serial-4096-r1
built commit=c512887606aeb686eccc53162d9f02c3c4db63ed dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=d6db0481dc0af4f77e2fc19b8227b478e1a2539cca55d353eb7118106b1139e3
framework/languages/node/packages/framework/dist/runtime/host/application-job-queue.js sha256=07e9734c0f3466cdd3d174518f2b1d9fa357bef66b169a6d699aa6270493fa27
framework/languages/node/packages/framework/dist/index.js sha256=4aedc33b29681b11d37221bf122fd9fe882eb3b005a9bdb173676ec8dbe85ba9
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae

Binding receive/reply: zlink-raw-server/main.js:64-117
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
    91	  }
    92	  return handled;
    93	}
    94	
    95	function pumpCommands() {
    96	  let handled = 0;
    97	  while (handled < batch) {
    98	    let hasMessage;
    99	    try {
   100	      hasMessage = tryRecv(commandRouter, commandReceived);
   101	    } catch (error) {
   102	      metrics.recordError();
   103	      console.error(`raw command loop recv failed: ${error.message}`);
   104	      break;
   105	    }
   106	    if (!hasMessage) break;
   107	    handled += 1;
   108	    try {
   109	      const part = bodyPart(commandReceived);
   110	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
   111	      if (body === null) throw new Error('invalid raw protobuf payload');
   112	      metrics.record(body);
   113	    } catch (error) {
   114	      metrics.recordError();
   115	      console.error(`raw command loop failed: ${error.message}`);
   116	    }
   117	  }

L0 receive/reply: zlink-framework-server/main.js:93-155
    93	  function pump() {
    94	    let handled = 0;
    95	    while (handled < batch) {
    96	      let hasMessage;
    97	      try {
    98	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    99	      } catch (error) {
   100	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   101	        metrics.recordError();
   102	        console.error(`ladder L0 recv failed: ${error.message}`);
   103	        break;
   104	      }
   105	      if (!hasMessage) break;
   106	      handled += 1;
   107	      const parts = received.parts;
   108	      try {
   109	        const header = frameworkWire ? parts[0].data() : null;
   110	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   111	        if (payload === null) throw new Error('invalid benchmark payload');
   112	        if (received.replyToken !== null) {
   113	          echoHandler.handleL0(payload);
   114	          received.reply()
   115	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
   116	            .message(encodeBenchPayloadMessage(payload))
   117	            .submit();
   118	        } else {
   119	          commandHandler.handleL0(payload);
   120	        }
   121	      } catch (error) {
   122	        metrics.recordError();
   123	        console.error(`ladder L0 handler failed: ${error.message}`);
   124	      }
   125	    }
   126	    return handled;
   127	  }
   128	
   129	  function pumpCommands() {
   130	    if (commandRouter === null) return 0;
   131	    let handled = 0;
   132	    while (handled < batch) {
   133	      let hasMessage;
   134	      try {
   135	        hasMessage = commandRouter.recv(commandReceived, zlink.RecvFlags.DontWait);
   136	      } catch (error) {
   137	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   138	        metrics.recordError();
   139	        console.error(`ladder L0 command recv failed: ${error.message}`);
   140	        break;
   141	      }
   142	      if (!hasMessage) break;
   143	      handled += 1;
   144	      try {
   145	        const parts = commandReceived.parts;
   146	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   147	        if (payload === null) throw new Error('invalid benchmark payload');
   148	        commandHandler.handleL0(payload);
   149	      } catch (error) {
   150	        metrics.recordError();
   151	        console.error(`ladder L0 command handler failed: ${error.message}`);
   152	      }
   153	    }
   154	    return handled;
   155	  }

Per-message waits/allocations/atomics/copies: binding and L0/L0R use one reusable Received, DontWait, batch 256, setImmediate; no queue wait, no JS atomics. L1 awaits ApplicationJobQueue.acquire(remote) before each receive and releases at handler entry. All paths decode the protobuf body, allocate a new encoded body and reply operation, and submit two parts; they do not copy or reuse request parts.

Run start L0R request-serial-4096 r1: 2026-11-13T16:41:35Z

First result L0R request-serial-4096 r1: 7442 msg/s; errors=0 submitted=completed=received=37211. Raw: raw/session/confirm/request-serial-4096/L0R/run1/run1/zlink-framework-node-request-serial-4096/results.json.

## Before confirm-L1-request-serial-4096-r1
built commit=c512887606aeb686eccc53162d9f02c3c4db63ed dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=d6db0481dc0af4f77e2fc19b8227b478e1a2539cca55d353eb7118106b1139e3
framework/languages/node/packages/framework/dist/runtime/host/application-job-queue.js sha256=07e9734c0f3466cdd3d174518f2b1d9fa357bef66b169a6d699aa6270493fa27
framework/languages/node/packages/framework/dist/index.js sha256=4aedc33b29681b11d37221bf122fd9fe882eb3b005a9bdb173676ec8dbe85ba9
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae

Binding receive/reply: zlink-raw-server/main.js:64-117
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
    91	  }
    92	  return handled;
    93	}
    94	
    95	function pumpCommands() {
    96	  let handled = 0;
    97	  while (handled < batch) {
    98	    let hasMessage;
    99	    try {
   100	      hasMessage = tryRecv(commandRouter, commandReceived);
   101	    } catch (error) {
   102	      metrics.recordError();
   103	      console.error(`raw command loop recv failed: ${error.message}`);
   104	      break;
   105	    }
   106	    if (!hasMessage) break;
   107	    handled += 1;
   108	    try {
   109	      const part = bodyPart(commandReceived);
   110	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
   111	      if (body === null) throw new Error('invalid raw protobuf payload');
   112	      metrics.record(body);
   113	    } catch (error) {
   114	      metrics.recordError();
   115	      console.error(`raw command loop failed: ${error.message}`);
   116	    }
   117	  }

L0 receive/reply: zlink-framework-server/main.js:93-155
    93	  function pump() {
    94	    let handled = 0;
    95	    while (handled < batch) {
    96	      let hasMessage;
    97	      try {
    98	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    99	      } catch (error) {
   100	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   101	        metrics.recordError();
   102	        console.error(`ladder L0 recv failed: ${error.message}`);
   103	        break;
   104	      }
   105	      if (!hasMessage) break;
   106	      handled += 1;
   107	      const parts = received.parts;
   108	      try {
   109	        const header = frameworkWire ? parts[0].data() : null;
   110	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   111	        if (payload === null) throw new Error('invalid benchmark payload');
   112	        if (received.replyToken !== null) {
   113	          echoHandler.handleL0(payload);
   114	          received.reply()
   115	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
   116	            .message(encodeBenchPayloadMessage(payload))
   117	            .submit();
   118	        } else {
   119	          commandHandler.handleL0(payload);
   120	        }
   121	      } catch (error) {
   122	        metrics.recordError();
   123	        console.error(`ladder L0 handler failed: ${error.message}`);
   124	      }
   125	    }
   126	    return handled;
   127	  }
   128	
   129	  function pumpCommands() {
   130	    if (commandRouter === null) return 0;
   131	    let handled = 0;
   132	    while (handled < batch) {
   133	      let hasMessage;
   134	      try {
   135	        hasMessage = commandRouter.recv(commandReceived, zlink.RecvFlags.DontWait);
   136	      } catch (error) {
   137	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   138	        metrics.recordError();
   139	        console.error(`ladder L0 command recv failed: ${error.message}`);
   140	        break;
   141	      }
   142	      if (!hasMessage) break;
   143	      handled += 1;
   144	      try {
   145	        const parts = commandReceived.parts;
   146	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   147	        if (payload === null) throw new Error('invalid benchmark payload');
   148	        commandHandler.handleL0(payload);
   149	      } catch (error) {
   150	        metrics.recordError();
   151	        console.error(`ladder L0 command handler failed: ${error.message}`);
   152	      }
   153	    }
   154	    return handled;
   155	  }

L1 permit receive/reply: zlink-framework-server/main.js:157-214
   157	  async function pumpWithPermits(socket, targetReceived, processMessage, label) {
   158	    let handled = 0;
   159	    while (handled < batch) {
   160	      const permit = await applicationJobQueue.acquire(undefined, 'remote');
   161	      let hasMessage;
   162	      try {
   163	        hasMessage = socket.recv(targetReceived, zlink.RecvFlags.DontWait);
   164	      } catch (error) {
   165	        permit.releaseUnqueued();
   166	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   167	        metrics.recordError();
   168	        console.error(`ladder L1 ${label} recv failed: ${error.message}`);
   169	        break;
   170	      }
   171	      if (!hasMessage) {
   172	        permit.releaseUnqueued();
   173	        break;
   174	      }
   175	      handled += 1;
   176	      try {
   177	        processMessage(permit);
   178	      } catch (error) {
   179	        permit.releaseAfterInternalProcessing();
   180	        metrics.recordError();
   181	        console.error(`ladder L1 ${label} handler failed: ${error.message}`);
   182	      }
   183	    }
   184	    return handled;
   185	  }
   186	
   187	  async function pumpRequestsWithPermits() {
   188	    return pumpWithPermits(router, received, (permit) => {
   189	      const parts = received.parts;
   190	      const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   191	      if (payload === null) throw new Error('invalid benchmark payload');
   192	      if (received.replyToken !== null) {
   193	        permit.releaseBeforeHandler();
   194	        echoHandler.handleL0(payload);
   195	        received.reply()
   196	          .message(RESPONSE_ENVELOPE)
   197	          .message(encodeBenchPayloadMessage(payload))
   198	          .submit();
   199	      } else {
   200	        permit.releaseBeforeHandler();
   201	        commandHandler.handleL0(payload);
   202	      }
   203	    }, 'request');
   204	  }
   205	
   206	  async function pumpCommandsWithPermits() {
   207	    return pumpWithPermits(commandRouter, commandReceived, (permit) => {
   208	      const parts = commandReceived.parts;
   209	      const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   210	      if (payload === null) throw new Error('invalid benchmark payload');
   211	      permit.releaseBeforeHandler();
   212	      commandHandler.handleL0(payload);
   213	    }, 'command');
   214	  }

Per-message waits/allocations/atomics/copies: binding and L0/L0R use one reusable Received, DontWait, batch 256, setImmediate; no queue wait, no JS atomics. L1 awaits ApplicationJobQueue.acquire(remote) before each receive and releases at handler entry. All paths decode the protobuf body, allocate a new encoded body and reply operation, and submit two parts; they do not copy or reuse request parts.

Run start L1 request-serial-4096 r1: 2026-11-13T16:41:45Z

First result L1 request-serial-4096 r1: 6946.4 msg/s; errors=0 submitted=completed=received=34733. Raw: raw/session/confirm/request-serial-4096/L1/run1/run1/zlink-framework-node-request-serial-4096/results.json.

## Before confirm-L0-request-serial-4096-r2
built commit=c512887606aeb686eccc53162d9f02c3c4db63ed dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=d6db0481dc0af4f77e2fc19b8227b478e1a2539cca55d353eb7118106b1139e3
framework/languages/node/packages/framework/dist/runtime/host/application-job-queue.js sha256=07e9734c0f3466cdd3d174518f2b1d9fa357bef66b169a6d699aa6270493fa27
framework/languages/node/packages/framework/dist/index.js sha256=4aedc33b29681b11d37221bf122fd9fe882eb3b005a9bdb173676ec8dbe85ba9
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae

Binding receive/reply: zlink-raw-server/main.js:64-117
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
    91	  }
    92	  return handled;
    93	}
    94	
    95	function pumpCommands() {
    96	  let handled = 0;
    97	  while (handled < batch) {
    98	    let hasMessage;
    99	    try {
   100	      hasMessage = tryRecv(commandRouter, commandReceived);
   101	    } catch (error) {
   102	      metrics.recordError();
   103	      console.error(`raw command loop recv failed: ${error.message}`);
   104	      break;
   105	    }
   106	    if (!hasMessage) break;
   107	    handled += 1;
   108	    try {
   109	      const part = bodyPart(commandReceived);
   110	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
   111	      if (body === null) throw new Error('invalid raw protobuf payload');
   112	      metrics.record(body);
   113	    } catch (error) {
   114	      metrics.recordError();
   115	      console.error(`raw command loop failed: ${error.message}`);
   116	    }
   117	  }

L0 receive/reply: zlink-framework-server/main.js:93-155
    93	  function pump() {
    94	    let handled = 0;
    95	    while (handled < batch) {
    96	      let hasMessage;
    97	      try {
    98	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    99	      } catch (error) {
   100	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   101	        metrics.recordError();
   102	        console.error(`ladder L0 recv failed: ${error.message}`);
   103	        break;
   104	      }
   105	      if (!hasMessage) break;
   106	      handled += 1;
   107	      const parts = received.parts;
   108	      try {
   109	        const header = frameworkWire ? parts[0].data() : null;
   110	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   111	        if (payload === null) throw new Error('invalid benchmark payload');
   112	        if (received.replyToken !== null) {
   113	          echoHandler.handleL0(payload);
   114	          received.reply()
   115	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
   116	            .message(encodeBenchPayloadMessage(payload))
   117	            .submit();
   118	        } else {
   119	          commandHandler.handleL0(payload);
   120	        }
   121	      } catch (error) {
   122	        metrics.recordError();
   123	        console.error(`ladder L0 handler failed: ${error.message}`);
   124	      }
   125	    }
   126	    return handled;
   127	  }
   128	
   129	  function pumpCommands() {
   130	    if (commandRouter === null) return 0;
   131	    let handled = 0;
   132	    while (handled < batch) {
   133	      let hasMessage;
   134	      try {
   135	        hasMessage = commandRouter.recv(commandReceived, zlink.RecvFlags.DontWait);
   136	      } catch (error) {
   137	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   138	        metrics.recordError();
   139	        console.error(`ladder L0 command recv failed: ${error.message}`);
   140	        break;
   141	      }
   142	      if (!hasMessage) break;
   143	      handled += 1;
   144	      try {
   145	        const parts = commandReceived.parts;
   146	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   147	        if (payload === null) throw new Error('invalid benchmark payload');
   148	        commandHandler.handleL0(payload);
   149	      } catch (error) {
   150	        metrics.recordError();
   151	        console.error(`ladder L0 command handler failed: ${error.message}`);
   152	      }
   153	    }
   154	    return handled;
   155	  }

Per-message waits/allocations/atomics/copies: binding and L0/L0R use one reusable Received, DontWait, batch 256, setImmediate; no queue wait, no JS atomics. L1 awaits ApplicationJobQueue.acquire(remote) before each receive and releases at handler entry. All paths decode the protobuf body, allocate a new encoded body and reply operation, and submit two parts; they do not copy or reuse request parts.

Run start L0 request-serial-4096 r2: 2026-11-13T16:42:00Z

First result L0 request-serial-4096 r2: 6514.2 msg/s; errors=0 submitted=completed=received=32572. Raw: raw/session/confirm/request-serial-4096/L0/run2/run1/zlink-framework-node-request-serial-4096/results.json.

## Before confirm-L0R-request-serial-4096-r2
built commit=c512887606aeb686eccc53162d9f02c3c4db63ed dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=d6db0481dc0af4f77e2fc19b8227b478e1a2539cca55d353eb7118106b1139e3
framework/languages/node/packages/framework/dist/runtime/host/application-job-queue.js sha256=07e9734c0f3466cdd3d174518f2b1d9fa357bef66b169a6d699aa6270493fa27
framework/languages/node/packages/framework/dist/index.js sha256=4aedc33b29681b11d37221bf122fd9fe882eb3b005a9bdb173676ec8dbe85ba9
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae

Binding receive/reply: zlink-raw-server/main.js:64-117
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
    91	  }
    92	  return handled;
    93	}
    94	
    95	function pumpCommands() {
    96	  let handled = 0;
    97	  while (handled < batch) {
    98	    let hasMessage;
    99	    try {
   100	      hasMessage = tryRecv(commandRouter, commandReceived);
   101	    } catch (error) {
   102	      metrics.recordError();
   103	      console.error(`raw command loop recv failed: ${error.message}`);
   104	      break;
   105	    }
   106	    if (!hasMessage) break;
   107	    handled += 1;
   108	    try {
   109	      const part = bodyPart(commandReceived);
   110	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
   111	      if (body === null) throw new Error('invalid raw protobuf payload');
   112	      metrics.record(body);
   113	    } catch (error) {
   114	      metrics.recordError();
   115	      console.error(`raw command loop failed: ${error.message}`);
   116	    }
   117	  }

L0 receive/reply: zlink-framework-server/main.js:93-155
    93	  function pump() {
    94	    let handled = 0;
    95	    while (handled < batch) {
    96	      let hasMessage;
    97	      try {
    98	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    99	      } catch (error) {
   100	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   101	        metrics.recordError();
   102	        console.error(`ladder L0 recv failed: ${error.message}`);
   103	        break;
   104	      }
   105	      if (!hasMessage) break;
   106	      handled += 1;
   107	      const parts = received.parts;
   108	      try {
   109	        const header = frameworkWire ? parts[0].data() : null;
   110	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   111	        if (payload === null) throw new Error('invalid benchmark payload');
   112	        if (received.replyToken !== null) {
   113	          echoHandler.handleL0(payload);
   114	          received.reply()
   115	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
   116	            .message(encodeBenchPayloadMessage(payload))
   117	            .submit();
   118	        } else {
   119	          commandHandler.handleL0(payload);
   120	        }
   121	      } catch (error) {
   122	        metrics.recordError();
   123	        console.error(`ladder L0 handler failed: ${error.message}`);
   124	      }
   125	    }
   126	    return handled;
   127	  }
   128	
   129	  function pumpCommands() {
   130	    if (commandRouter === null) return 0;
   131	    let handled = 0;
   132	    while (handled < batch) {
   133	      let hasMessage;
   134	      try {
   135	        hasMessage = commandRouter.recv(commandReceived, zlink.RecvFlags.DontWait);
   136	      } catch (error) {
   137	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   138	        metrics.recordError();
   139	        console.error(`ladder L0 command recv failed: ${error.message}`);
   140	        break;
   141	      }
   142	      if (!hasMessage) break;
   143	      handled += 1;
   144	      try {
   145	        const parts = commandReceived.parts;
   146	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   147	        if (payload === null) throw new Error('invalid benchmark payload');
   148	        commandHandler.handleL0(payload);
   149	      } catch (error) {
   150	        metrics.recordError();
   151	        console.error(`ladder L0 command handler failed: ${error.message}`);
   152	      }
   153	    }
   154	    return handled;
   155	  }

Per-message waits/allocations/atomics/copies: binding and L0/L0R use one reusable Received, DontWait, batch 256, setImmediate; no queue wait, no JS atomics. L1 awaits ApplicationJobQueue.acquire(remote) before each receive and releases at handler entry. All paths decode the protobuf body, allocate a new encoded body and reply operation, and submit two parts; they do not copy or reuse request parts.

Run start L0R request-serial-4096 r2: 2026-11-13T16:42:14Z

First result L0R request-serial-4096 r2: 6585.8 msg/s; errors=0 submitted=completed=received=32930. Raw: raw/session/confirm/request-serial-4096/L0R/run2/run1/zlink-framework-node-request-serial-4096/results.json.

## Before confirm-L1-request-serial-4096-r2
built commit=c512887606aeb686eccc53162d9f02c3c4db63ed dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=d6db0481dc0af4f77e2fc19b8227b478e1a2539cca55d353eb7118106b1139e3
framework/languages/node/packages/framework/dist/runtime/host/application-job-queue.js sha256=07e9734c0f3466cdd3d174518f2b1d9fa357bef66b169a6d699aa6270493fa27
framework/languages/node/packages/framework/dist/index.js sha256=4aedc33b29681b11d37221bf122fd9fe882eb3b005a9bdb173676ec8dbe85ba9
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae

Binding receive/reply: zlink-raw-server/main.js:64-117
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
    91	  }
    92	  return handled;
    93	}
    94	
    95	function pumpCommands() {
    96	  let handled = 0;
    97	  while (handled < batch) {
    98	    let hasMessage;
    99	    try {
   100	      hasMessage = tryRecv(commandRouter, commandReceived);
   101	    } catch (error) {
   102	      metrics.recordError();
   103	      console.error(`raw command loop recv failed: ${error.message}`);
   104	      break;
   105	    }
   106	    if (!hasMessage) break;
   107	    handled += 1;
   108	    try {
   109	      const part = bodyPart(commandReceived);
   110	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
   111	      if (body === null) throw new Error('invalid raw protobuf payload');
   112	      metrics.record(body);
   113	    } catch (error) {
   114	      metrics.recordError();
   115	      console.error(`raw command loop failed: ${error.message}`);
   116	    }
   117	  }

L0 receive/reply: zlink-framework-server/main.js:93-155
    93	  function pump() {
    94	    let handled = 0;
    95	    while (handled < batch) {
    96	      let hasMessage;
    97	      try {
    98	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    99	      } catch (error) {
   100	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   101	        metrics.recordError();
   102	        console.error(`ladder L0 recv failed: ${error.message}`);
   103	        break;
   104	      }
   105	      if (!hasMessage) break;
   106	      handled += 1;
   107	      const parts = received.parts;
   108	      try {
   109	        const header = frameworkWire ? parts[0].data() : null;
   110	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   111	        if (payload === null) throw new Error('invalid benchmark payload');
   112	        if (received.replyToken !== null) {
   113	          echoHandler.handleL0(payload);
   114	          received.reply()
   115	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
   116	            .message(encodeBenchPayloadMessage(payload))
   117	            .submit();
   118	        } else {
   119	          commandHandler.handleL0(payload);
   120	        }
   121	      } catch (error) {
   122	        metrics.recordError();
   123	        console.error(`ladder L0 handler failed: ${error.message}`);
   124	      }
   125	    }
   126	    return handled;
   127	  }
   128	
   129	  function pumpCommands() {
   130	    if (commandRouter === null) return 0;
   131	    let handled = 0;
   132	    while (handled < batch) {
   133	      let hasMessage;
   134	      try {
   135	        hasMessage = commandRouter.recv(commandReceived, zlink.RecvFlags.DontWait);
   136	      } catch (error) {
   137	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   138	        metrics.recordError();
   139	        console.error(`ladder L0 command recv failed: ${error.message}`);
   140	        break;
   141	      }
   142	      if (!hasMessage) break;
   143	      handled += 1;
   144	      try {
   145	        const parts = commandReceived.parts;
   146	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   147	        if (payload === null) throw new Error('invalid benchmark payload');
   148	        commandHandler.handleL0(payload);
   149	      } catch (error) {
   150	        metrics.recordError();
   151	        console.error(`ladder L0 command handler failed: ${error.message}`);
   152	      }
   153	    }
   154	    return handled;
   155	  }

L1 permit receive/reply: zlink-framework-server/main.js:157-214
   157	  async function pumpWithPermits(socket, targetReceived, processMessage, label) {
   158	    let handled = 0;
   159	    while (handled < batch) {
   160	      const permit = await applicationJobQueue.acquire(undefined, 'remote');
   161	      let hasMessage;
   162	      try {
   163	        hasMessage = socket.recv(targetReceived, zlink.RecvFlags.DontWait);
   164	      } catch (error) {
   165	        permit.releaseUnqueued();
   166	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   167	        metrics.recordError();
   168	        console.error(`ladder L1 ${label} recv failed: ${error.message}`);
   169	        break;
   170	      }
   171	      if (!hasMessage) {
   172	        permit.releaseUnqueued();
   173	        break;
   174	      }
   175	      handled += 1;
   176	      try {
   177	        processMessage(permit);
   178	      } catch (error) {
   179	        permit.releaseAfterInternalProcessing();
   180	        metrics.recordError();
   181	        console.error(`ladder L1 ${label} handler failed: ${error.message}`);
   182	      }
   183	    }
   184	    return handled;
   185	  }
   186	
   187	  async function pumpRequestsWithPermits() {
   188	    return pumpWithPermits(router, received, (permit) => {
   189	      const parts = received.parts;
   190	      const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   191	      if (payload === null) throw new Error('invalid benchmark payload');
   192	      if (received.replyToken !== null) {
   193	        permit.releaseBeforeHandler();
   194	        echoHandler.handleL0(payload);
   195	        received.reply()
   196	          .message(RESPONSE_ENVELOPE)
   197	          .message(encodeBenchPayloadMessage(payload))
   198	          .submit();
   199	      } else {
   200	        permit.releaseBeforeHandler();
   201	        commandHandler.handleL0(payload);
   202	      }
   203	    }, 'request');
   204	  }
   205	
   206	  async function pumpCommandsWithPermits() {
   207	    return pumpWithPermits(commandRouter, commandReceived, (permit) => {
   208	      const parts = commandReceived.parts;
   209	      const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   210	      if (payload === null) throw new Error('invalid benchmark payload');
   211	      permit.releaseBeforeHandler();
   212	      commandHandler.handleL0(payload);
   213	    }, 'command');
   214	  }

Per-message waits/allocations/atomics/copies: binding and L0/L0R use one reusable Received, DontWait, batch 256, setImmediate; no queue wait, no JS atomics. L1 awaits ApplicationJobQueue.acquire(remote) before each receive and releases at handler entry. All paths decode the protobuf body, allocate a new encoded body and reply operation, and submit two parts; they do not copy or reuse request parts.

Run start L1 request-serial-4096 r2: 2026-11-13T16:42:25Z

First result L1 request-serial-4096 r2: 7416 msg/s; errors=0 submitted=completed=received=37081. Raw: raw/session/confirm/request-serial-4096/L1/run2/run1/zlink-framework-node-request-serial-4096/results.json.

## Before confirm-L0-request-serial-4096-r3
built commit=c512887606aeb686eccc53162d9f02c3c4db63ed dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=d6db0481dc0af4f77e2fc19b8227b478e1a2539cca55d353eb7118106b1139e3
framework/languages/node/packages/framework/dist/runtime/host/application-job-queue.js sha256=07e9734c0f3466cdd3d174518f2b1d9fa357bef66b169a6d699aa6270493fa27
framework/languages/node/packages/framework/dist/index.js sha256=4aedc33b29681b11d37221bf122fd9fe882eb3b005a9bdb173676ec8dbe85ba9
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae

Binding receive/reply: zlink-raw-server/main.js:64-117
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
    91	  }
    92	  return handled;
    93	}
    94	
    95	function pumpCommands() {
    96	  let handled = 0;
    97	  while (handled < batch) {
    98	    let hasMessage;
    99	    try {
   100	      hasMessage = tryRecv(commandRouter, commandReceived);
   101	    } catch (error) {
   102	      metrics.recordError();
   103	      console.error(`raw command loop recv failed: ${error.message}`);
   104	      break;
   105	    }
   106	    if (!hasMessage) break;
   107	    handled += 1;
   108	    try {
   109	      const part = bodyPart(commandReceived);
   110	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
   111	      if (body === null) throw new Error('invalid raw protobuf payload');
   112	      metrics.record(body);
   113	    } catch (error) {
   114	      metrics.recordError();
   115	      console.error(`raw command loop failed: ${error.message}`);
   116	    }
   117	  }

L0 receive/reply: zlink-framework-server/main.js:93-155
    93	  function pump() {
    94	    let handled = 0;
    95	    while (handled < batch) {
    96	      let hasMessage;
    97	      try {
    98	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    99	      } catch (error) {
   100	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   101	        metrics.recordError();
   102	        console.error(`ladder L0 recv failed: ${error.message}`);
   103	        break;
   104	      }
   105	      if (!hasMessage) break;
   106	      handled += 1;
   107	      const parts = received.parts;
   108	      try {
   109	        const header = frameworkWire ? parts[0].data() : null;
   110	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   111	        if (payload === null) throw new Error('invalid benchmark payload');
   112	        if (received.replyToken !== null) {
   113	          echoHandler.handleL0(payload);
   114	          received.reply()
   115	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
   116	            .message(encodeBenchPayloadMessage(payload))
   117	            .submit();
   118	        } else {
   119	          commandHandler.handleL0(payload);
   120	        }
   121	      } catch (error) {
   122	        metrics.recordError();
   123	        console.error(`ladder L0 handler failed: ${error.message}`);
   124	      }
   125	    }
   126	    return handled;
   127	  }
   128	
   129	  function pumpCommands() {
   130	    if (commandRouter === null) return 0;
   131	    let handled = 0;
   132	    while (handled < batch) {
   133	      let hasMessage;
   134	      try {
   135	        hasMessage = commandRouter.recv(commandReceived, zlink.RecvFlags.DontWait);
   136	      } catch (error) {
   137	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   138	        metrics.recordError();
   139	        console.error(`ladder L0 command recv failed: ${error.message}`);
   140	        break;
   141	      }
   142	      if (!hasMessage) break;
   143	      handled += 1;
   144	      try {
   145	        const parts = commandReceived.parts;
   146	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   147	        if (payload === null) throw new Error('invalid benchmark payload');
   148	        commandHandler.handleL0(payload);
   149	      } catch (error) {
   150	        metrics.recordError();
   151	        console.error(`ladder L0 command handler failed: ${error.message}`);
   152	      }
   153	    }
   154	    return handled;
   155	  }

Per-message waits/allocations/atomics/copies: binding and L0/L0R use one reusable Received, DontWait, batch 256, setImmediate; no queue wait, no JS atomics. L1 awaits ApplicationJobQueue.acquire(remote) before each receive and releases at handler entry. All paths decode the protobuf body, allocate a new encoded body and reply operation, and submit two parts; they do not copy or reuse request parts.

Run start L0 request-serial-4096 r3: 2026-11-13T16:42:49Z

First result L0 request-serial-4096 r3: 7776.4 msg/s; errors=0 submitted=completed=received=38883. Raw: raw/session/confirm/request-serial-4096/L0/run3/run1/zlink-framework-node-request-serial-4096/results.json.

## Before confirm-L0R-request-serial-4096-r3
built commit=c512887606aeb686eccc53162d9f02c3c4db63ed dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=d6db0481dc0af4f77e2fc19b8227b478e1a2539cca55d353eb7118106b1139e3
framework/languages/node/packages/framework/dist/runtime/host/application-job-queue.js sha256=07e9734c0f3466cdd3d174518f2b1d9fa357bef66b169a6d699aa6270493fa27
framework/languages/node/packages/framework/dist/index.js sha256=4aedc33b29681b11d37221bf122fd9fe882eb3b005a9bdb173676ec8dbe85ba9
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae

Binding receive/reply: zlink-raw-server/main.js:64-117
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
    91	  }
    92	  return handled;
    93	}
    94	
    95	function pumpCommands() {
    96	  let handled = 0;
    97	  while (handled < batch) {
    98	    let hasMessage;
    99	    try {
   100	      hasMessage = tryRecv(commandRouter, commandReceived);
   101	    } catch (error) {
   102	      metrics.recordError();
   103	      console.error(`raw command loop recv failed: ${error.message}`);
   104	      break;
   105	    }
   106	    if (!hasMessage) break;
   107	    handled += 1;
   108	    try {
   109	      const part = bodyPart(commandReceived);
   110	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
   111	      if (body === null) throw new Error('invalid raw protobuf payload');
   112	      metrics.record(body);
   113	    } catch (error) {
   114	      metrics.recordError();
   115	      console.error(`raw command loop failed: ${error.message}`);
   116	    }
   117	  }

L0 receive/reply: zlink-framework-server/main.js:93-155
    93	  function pump() {
    94	    let handled = 0;
    95	    while (handled < batch) {
    96	      let hasMessage;
    97	      try {
    98	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    99	      } catch (error) {
   100	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   101	        metrics.recordError();
   102	        console.error(`ladder L0 recv failed: ${error.message}`);
   103	        break;
   104	      }
   105	      if (!hasMessage) break;
   106	      handled += 1;
   107	      const parts = received.parts;
   108	      try {
   109	        const header = frameworkWire ? parts[0].data() : null;
   110	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   111	        if (payload === null) throw new Error('invalid benchmark payload');
   112	        if (received.replyToken !== null) {
   113	          echoHandler.handleL0(payload);
   114	          received.reply()
   115	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
   116	            .message(encodeBenchPayloadMessage(payload))
   117	            .submit();
   118	        } else {
   119	          commandHandler.handleL0(payload);
   120	        }
   121	      } catch (error) {
   122	        metrics.recordError();
   123	        console.error(`ladder L0 handler failed: ${error.message}`);
   124	      }
   125	    }
   126	    return handled;
   127	  }
   128	
   129	  function pumpCommands() {
   130	    if (commandRouter === null) return 0;
   131	    let handled = 0;
   132	    while (handled < batch) {
   133	      let hasMessage;
   134	      try {
   135	        hasMessage = commandRouter.recv(commandReceived, zlink.RecvFlags.DontWait);
   136	      } catch (error) {
   137	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   138	        metrics.recordError();
   139	        console.error(`ladder L0 command recv failed: ${error.message}`);
   140	        break;
   141	      }
   142	      if (!hasMessage) break;
   143	      handled += 1;
   144	      try {
   145	        const parts = commandReceived.parts;
   146	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   147	        if (payload === null) throw new Error('invalid benchmark payload');
   148	        commandHandler.handleL0(payload);
   149	      } catch (error) {
   150	        metrics.recordError();
   151	        console.error(`ladder L0 command handler failed: ${error.message}`);
   152	      }
   153	    }
   154	    return handled;
   155	  }

Per-message waits/allocations/atomics/copies: binding and L0/L0R use one reusable Received, DontWait, batch 256, setImmediate; no queue wait, no JS atomics. L1 awaits ApplicationJobQueue.acquire(remote) before each receive and releases at handler entry. All paths decode the protobuf body, allocate a new encoded body and reply operation, and submit two parts; they do not copy or reuse request parts.

Run start L0R request-serial-4096 r3: 2026-11-13T16:43:00Z

First result L0R request-serial-4096 r3: 5276.4 msg/s; errors=0 submitted=completed=received=26383. Raw: raw/session/confirm/request-serial-4096/L0R/run3/run1/zlink-framework-node-request-serial-4096/results.json.

## Before confirm-L1-request-serial-4096-r3
built commit=c512887606aeb686eccc53162d9f02c3c4db63ed dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=d6db0481dc0af4f77e2fc19b8227b478e1a2539cca55d353eb7118106b1139e3
framework/languages/node/packages/framework/dist/runtime/host/application-job-queue.js sha256=07e9734c0f3466cdd3d174518f2b1d9fa357bef66b169a6d699aa6270493fa27
framework/languages/node/packages/framework/dist/index.js sha256=4aedc33b29681b11d37221bf122fd9fe882eb3b005a9bdb173676ec8dbe85ba9
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae

Binding receive/reply: zlink-raw-server/main.js:64-117
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
    91	  }
    92	  return handled;
    93	}
    94	
    95	function pumpCommands() {
    96	  let handled = 0;
    97	  while (handled < batch) {
    98	    let hasMessage;
    99	    try {
   100	      hasMessage = tryRecv(commandRouter, commandReceived);
   101	    } catch (error) {
   102	      metrics.recordError();
   103	      console.error(`raw command loop recv failed: ${error.message}`);
   104	      break;
   105	    }
   106	    if (!hasMessage) break;
   107	    handled += 1;
   108	    try {
   109	      const part = bodyPart(commandReceived);
   110	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
   111	      if (body === null) throw new Error('invalid raw protobuf payload');
   112	      metrics.record(body);
   113	    } catch (error) {
   114	      metrics.recordError();
   115	      console.error(`raw command loop failed: ${error.message}`);
   116	    }
   117	  }

L0 receive/reply: zlink-framework-server/main.js:93-155
    93	  function pump() {
    94	    let handled = 0;
    95	    while (handled < batch) {
    96	      let hasMessage;
    97	      try {
    98	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    99	      } catch (error) {
   100	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   101	        metrics.recordError();
   102	        console.error(`ladder L0 recv failed: ${error.message}`);
   103	        break;
   104	      }
   105	      if (!hasMessage) break;
   106	      handled += 1;
   107	      const parts = received.parts;
   108	      try {
   109	        const header = frameworkWire ? parts[0].data() : null;
   110	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   111	        if (payload === null) throw new Error('invalid benchmark payload');
   112	        if (received.replyToken !== null) {
   113	          echoHandler.handleL0(payload);
   114	          received.reply()
   115	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
   116	            .message(encodeBenchPayloadMessage(payload))
   117	            .submit();
   118	        } else {
   119	          commandHandler.handleL0(payload);
   120	        }
   121	      } catch (error) {
   122	        metrics.recordError();
   123	        console.error(`ladder L0 handler failed: ${error.message}`);
   124	      }
   125	    }
   126	    return handled;
   127	  }
   128	
   129	  function pumpCommands() {
   130	    if (commandRouter === null) return 0;
   131	    let handled = 0;
   132	    while (handled < batch) {
   133	      let hasMessage;
   134	      try {
   135	        hasMessage = commandRouter.recv(commandReceived, zlink.RecvFlags.DontWait);
   136	      } catch (error) {
   137	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   138	        metrics.recordError();
   139	        console.error(`ladder L0 command recv failed: ${error.message}`);
   140	        break;
   141	      }
   142	      if (!hasMessage) break;
   143	      handled += 1;
   144	      try {
   145	        const parts = commandReceived.parts;
   146	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   147	        if (payload === null) throw new Error('invalid benchmark payload');
   148	        commandHandler.handleL0(payload);
   149	      } catch (error) {
   150	        metrics.recordError();
   151	        console.error(`ladder L0 command handler failed: ${error.message}`);
   152	      }
   153	    }
   154	    return handled;
   155	  }

L1 permit receive/reply: zlink-framework-server/main.js:157-214
   157	  async function pumpWithPermits(socket, targetReceived, processMessage, label) {
   158	    let handled = 0;
   159	    while (handled < batch) {
   160	      const permit = await applicationJobQueue.acquire(undefined, 'remote');
   161	      let hasMessage;
   162	      try {
   163	        hasMessage = socket.recv(targetReceived, zlink.RecvFlags.DontWait);
   164	      } catch (error) {
   165	        permit.releaseUnqueued();
   166	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   167	        metrics.recordError();
   168	        console.error(`ladder L1 ${label} recv failed: ${error.message}`);
   169	        break;
   170	      }
   171	      if (!hasMessage) {
   172	        permit.releaseUnqueued();
   173	        break;
   174	      }
   175	      handled += 1;
   176	      try {
   177	        processMessage(permit);
   178	      } catch (error) {
   179	        permit.releaseAfterInternalProcessing();
   180	        metrics.recordError();
   181	        console.error(`ladder L1 ${label} handler failed: ${error.message}`);
   182	      }
   183	    }
   184	    return handled;
   185	  }
   186	
   187	  async function pumpRequestsWithPermits() {
   188	    return pumpWithPermits(router, received, (permit) => {
   189	      const parts = received.parts;
   190	      const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   191	      if (payload === null) throw new Error('invalid benchmark payload');
   192	      if (received.replyToken !== null) {
   193	        permit.releaseBeforeHandler();
   194	        echoHandler.handleL0(payload);
   195	        received.reply()
   196	          .message(RESPONSE_ENVELOPE)
   197	          .message(encodeBenchPayloadMessage(payload))
   198	          .submit();
   199	      } else {
   200	        permit.releaseBeforeHandler();
   201	        commandHandler.handleL0(payload);
   202	      }
   203	    }, 'request');
   204	  }
   205	
   206	  async function pumpCommandsWithPermits() {
   207	    return pumpWithPermits(commandRouter, commandReceived, (permit) => {
   208	      const parts = commandReceived.parts;
   209	      const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   210	      if (payload === null) throw new Error('invalid benchmark payload');
   211	      permit.releaseBeforeHandler();
   212	      commandHandler.handleL0(payload);
   213	    }, 'command');
   214	  }

Per-message waits/allocations/atomics/copies: binding and L0/L0R use one reusable Received, DontWait, batch 256, setImmediate; no queue wait, no JS atomics. L1 awaits ApplicationJobQueue.acquire(remote) before each receive and releases at handler entry. All paths decode the protobuf body, allocate a new encoded body and reply operation, and submit two parts; they do not copy or reuse request parts.

Run start L1 request-serial-4096 r3: 2026-11-13T16:43:35Z

First result L1 request-serial-4096 r3: 6960.8 msg/s; errors=0 submitted=completed=received=34805. Raw: raw/session/confirm/request-serial-4096/L1/run3/run1/zlink-framework-node-request-serial-4096/results.json.

## Before confirm-binding-send-saturation-4096-r1
built commit=c512887606aeb686eccc53162d9f02c3c4db63ed dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=d6db0481dc0af4f77e2fc19b8227b478e1a2539cca55d353eb7118106b1139e3
framework/languages/node/packages/framework/dist/runtime/host/application-job-queue.js sha256=07e9734c0f3466cdd3d174518f2b1d9fa357bef66b169a6d699aa6270493fa27
framework/languages/node/packages/framework/dist/index.js sha256=4aedc33b29681b11d37221bf122fd9fe882eb3b005a9bdb173676ec8dbe85ba9
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae

Binding receive/reply: zlink-raw-server/main.js:64-117
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
    91	  }
    92	  return handled;
    93	}
    94	
    95	function pumpCommands() {
    96	  let handled = 0;
    97	  while (handled < batch) {
    98	    let hasMessage;
    99	    try {
   100	      hasMessage = tryRecv(commandRouter, commandReceived);
   101	    } catch (error) {
   102	      metrics.recordError();
   103	      console.error(`raw command loop recv failed: ${error.message}`);
   104	      break;
   105	    }
   106	    if (!hasMessage) break;
   107	    handled += 1;
   108	    try {
   109	      const part = bodyPart(commandReceived);
   110	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
   111	      if (body === null) throw new Error('invalid raw protobuf payload');
   112	      metrics.record(body);
   113	    } catch (error) {
   114	      metrics.recordError();
   115	      console.error(`raw command loop failed: ${error.message}`);
   116	    }
   117	  }

L0 receive/reply: zlink-framework-server/main.js:93-155
    93	  function pump() {
    94	    let handled = 0;
    95	    while (handled < batch) {
    96	      let hasMessage;
    97	      try {
    98	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    99	      } catch (error) {
   100	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   101	        metrics.recordError();
   102	        console.error(`ladder L0 recv failed: ${error.message}`);
   103	        break;
   104	      }
   105	      if (!hasMessage) break;
   106	      handled += 1;
   107	      const parts = received.parts;
   108	      try {
   109	        const header = frameworkWire ? parts[0].data() : null;
   110	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   111	        if (payload === null) throw new Error('invalid benchmark payload');
   112	        if (received.replyToken !== null) {
   113	          echoHandler.handleL0(payload);
   114	          received.reply()
   115	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
   116	            .message(encodeBenchPayloadMessage(payload))
   117	            .submit();
   118	        } else {
   119	          commandHandler.handleL0(payload);
   120	        }
   121	      } catch (error) {
   122	        metrics.recordError();
   123	        console.error(`ladder L0 handler failed: ${error.message}`);
   124	      }
   125	    }
   126	    return handled;
   127	  }
   128	
   129	  function pumpCommands() {
   130	    if (commandRouter === null) return 0;
   131	    let handled = 0;
   132	    while (handled < batch) {
   133	      let hasMessage;
   134	      try {
   135	        hasMessage = commandRouter.recv(commandReceived, zlink.RecvFlags.DontWait);
   136	      } catch (error) {
   137	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   138	        metrics.recordError();
   139	        console.error(`ladder L0 command recv failed: ${error.message}`);
   140	        break;
   141	      }
   142	      if (!hasMessage) break;
   143	      handled += 1;
   144	      try {
   145	        const parts = commandReceived.parts;
   146	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   147	        if (payload === null) throw new Error('invalid benchmark payload');
   148	        commandHandler.handleL0(payload);
   149	      } catch (error) {
   150	        metrics.recordError();
   151	        console.error(`ladder L0 command handler failed: ${error.message}`);
   152	      }
   153	    }
   154	    return handled;
   155	  }

Per-message waits/allocations/atomics/copies: binding and L0/L0R use one reusable Received, DontWait, batch 256, setImmediate; no queue wait, no JS atomics. L1 awaits ApplicationJobQueue.acquire(remote) before each receive and releases at handler entry. All paths decode the protobuf body, allocate a new encoded body and reply operation, and submit two parts; they do not copy or reuse request parts.

Run start binding send-saturation-4096 r1: 2026-11-13T16:43:45Z

First result binding send-saturation-4096 r1: 87876.2 msg/s; errors=0 submitted=completed=received=439381. Raw: raw/session/confirm/send-saturation-4096/binding/run1/run1/zlink-node-send-saturation-4096/results.json.

## Before confirm-L0-send-saturation-4096-r1
built commit=c512887606aeb686eccc53162d9f02c3c4db63ed dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=d6db0481dc0af4f77e2fc19b8227b478e1a2539cca55d353eb7118106b1139e3
framework/languages/node/packages/framework/dist/runtime/host/application-job-queue.js sha256=07e9734c0f3466cdd3d174518f2b1d9fa357bef66b169a6d699aa6270493fa27
framework/languages/node/packages/framework/dist/index.js sha256=4aedc33b29681b11d37221bf122fd9fe882eb3b005a9bdb173676ec8dbe85ba9
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae

Binding receive/reply: zlink-raw-server/main.js:64-117
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
    91	  }
    92	  return handled;
    93	}
    94	
    95	function pumpCommands() {
    96	  let handled = 0;
    97	  while (handled < batch) {
    98	    let hasMessage;
    99	    try {
   100	      hasMessage = tryRecv(commandRouter, commandReceived);
   101	    } catch (error) {
   102	      metrics.recordError();
   103	      console.error(`raw command loop recv failed: ${error.message}`);
   104	      break;
   105	    }
   106	    if (!hasMessage) break;
   107	    handled += 1;
   108	    try {
   109	      const part = bodyPart(commandReceived);
   110	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
   111	      if (body === null) throw new Error('invalid raw protobuf payload');
   112	      metrics.record(body);
   113	    } catch (error) {
   114	      metrics.recordError();
   115	      console.error(`raw command loop failed: ${error.message}`);
   116	    }
   117	  }

L0 receive/reply: zlink-framework-server/main.js:93-155
    93	  function pump() {
    94	    let handled = 0;
    95	    while (handled < batch) {
    96	      let hasMessage;
    97	      try {
    98	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    99	      } catch (error) {
   100	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   101	        metrics.recordError();
   102	        console.error(`ladder L0 recv failed: ${error.message}`);
   103	        break;
   104	      }
   105	      if (!hasMessage) break;
   106	      handled += 1;
   107	      const parts = received.parts;
   108	      try {
   109	        const header = frameworkWire ? parts[0].data() : null;
   110	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   111	        if (payload === null) throw new Error('invalid benchmark payload');
   112	        if (received.replyToken !== null) {
   113	          echoHandler.handleL0(payload);
   114	          received.reply()
   115	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
   116	            .message(encodeBenchPayloadMessage(payload))
   117	            .submit();
   118	        } else {
   119	          commandHandler.handleL0(payload);
   120	        }
   121	      } catch (error) {
   122	        metrics.recordError();
   123	        console.error(`ladder L0 handler failed: ${error.message}`);
   124	      }
   125	    }
   126	    return handled;
   127	  }
   128	
   129	  function pumpCommands() {
   130	    if (commandRouter === null) return 0;
   131	    let handled = 0;
   132	    while (handled < batch) {
   133	      let hasMessage;
   134	      try {
   135	        hasMessage = commandRouter.recv(commandReceived, zlink.RecvFlags.DontWait);
   136	      } catch (error) {
   137	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   138	        metrics.recordError();
   139	        console.error(`ladder L0 command recv failed: ${error.message}`);
   140	        break;
   141	      }
   142	      if (!hasMessage) break;
   143	      handled += 1;
   144	      try {
   145	        const parts = commandReceived.parts;
   146	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   147	        if (payload === null) throw new Error('invalid benchmark payload');
   148	        commandHandler.handleL0(payload);
   149	      } catch (error) {
   150	        metrics.recordError();
   151	        console.error(`ladder L0 command handler failed: ${error.message}`);
   152	      }
   153	    }
   154	    return handled;
   155	  }

Per-message waits/allocations/atomics/copies: binding and L0/L0R use one reusable Received, DontWait, batch 256, setImmediate; no queue wait, no JS atomics. L1 awaits ApplicationJobQueue.acquire(remote) before each receive and releases at handler entry. All paths decode the protobuf body, allocate a new encoded body and reply operation, and submit two parts; they do not copy or reuse request parts.

Run start L0 send-saturation-4096 r1: 2026-11-13T16:43:56Z

First result L0 send-saturation-4096 r1: 88355.4 msg/s; errors=0 submitted=completed=received=441949. Raw: raw/session/confirm/send-saturation-4096/L0/run1/run1/zlink-framework-node-send-saturation-4096/results.json.

## Before confirm-binding-send-saturation-4096-r2
built commit=c512887606aeb686eccc53162d9f02c3c4db63ed dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=d6db0481dc0af4f77e2fc19b8227b478e1a2539cca55d353eb7118106b1139e3
framework/languages/node/packages/framework/dist/runtime/host/application-job-queue.js sha256=07e9734c0f3466cdd3d174518f2b1d9fa357bef66b169a6d699aa6270493fa27
framework/languages/node/packages/framework/dist/index.js sha256=4aedc33b29681b11d37221bf122fd9fe882eb3b005a9bdb173676ec8dbe85ba9
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae

Binding receive/reply: zlink-raw-server/main.js:64-117
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
    91	  }
    92	  return handled;
    93	}
    94	
    95	function pumpCommands() {
    96	  let handled = 0;
    97	  while (handled < batch) {
    98	    let hasMessage;
    99	    try {
   100	      hasMessage = tryRecv(commandRouter, commandReceived);
   101	    } catch (error) {
   102	      metrics.recordError();
   103	      console.error(`raw command loop recv failed: ${error.message}`);
   104	      break;
   105	    }
   106	    if (!hasMessage) break;
   107	    handled += 1;
   108	    try {
   109	      const part = bodyPart(commandReceived);
   110	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
   111	      if (body === null) throw new Error('invalid raw protobuf payload');
   112	      metrics.record(body);
   113	    } catch (error) {
   114	      metrics.recordError();
   115	      console.error(`raw command loop failed: ${error.message}`);
   116	    }
   117	  }

L0 receive/reply: zlink-framework-server/main.js:93-155
    93	  function pump() {
    94	    let handled = 0;
    95	    while (handled < batch) {
    96	      let hasMessage;
    97	      try {
    98	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    99	      } catch (error) {
   100	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   101	        metrics.recordError();
   102	        console.error(`ladder L0 recv failed: ${error.message}`);
   103	        break;
   104	      }
   105	      if (!hasMessage) break;
   106	      handled += 1;
   107	      const parts = received.parts;
   108	      try {
   109	        const header = frameworkWire ? parts[0].data() : null;
   110	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   111	        if (payload === null) throw new Error('invalid benchmark payload');
   112	        if (received.replyToken !== null) {
   113	          echoHandler.handleL0(payload);
   114	          received.reply()
   115	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
   116	            .message(encodeBenchPayloadMessage(payload))
   117	            .submit();
   118	        } else {
   119	          commandHandler.handleL0(payload);
   120	        }
   121	      } catch (error) {
   122	        metrics.recordError();
   123	        console.error(`ladder L0 handler failed: ${error.message}`);
   124	      }
   125	    }
   126	    return handled;
   127	  }
   128	
   129	  function pumpCommands() {
   130	    if (commandRouter === null) return 0;
   131	    let handled = 0;
   132	    while (handled < batch) {
   133	      let hasMessage;
   134	      try {
   135	        hasMessage = commandRouter.recv(commandReceived, zlink.RecvFlags.DontWait);
   136	      } catch (error) {
   137	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   138	        metrics.recordError();
   139	        console.error(`ladder L0 command recv failed: ${error.message}`);
   140	        break;
   141	      }
   142	      if (!hasMessage) break;
   143	      handled += 1;
   144	      try {
   145	        const parts = commandReceived.parts;
   146	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   147	        if (payload === null) throw new Error('invalid benchmark payload');
   148	        commandHandler.handleL0(payload);
   149	      } catch (error) {
   150	        metrics.recordError();
   151	        console.error(`ladder L0 command handler failed: ${error.message}`);
   152	      }
   153	    }
   154	    return handled;
   155	  }

Per-message waits/allocations/atomics/copies: binding and L0/L0R use one reusable Received, DontWait, batch 256, setImmediate; no queue wait, no JS atomics. L1 awaits ApplicationJobQueue.acquire(remote) before each receive and releases at handler entry. All paths decode the protobuf body, allocate a new encoded body and reply operation, and submit two parts; they do not copy or reuse request parts.

Run start binding send-saturation-4096 r2: 2026-11-13T16:44:11Z

First result binding send-saturation-4096 r2: 86888.8 msg/s; errors=0 submitted=completed=received=434917. Raw: raw/session/confirm/send-saturation-4096/binding/run2/run1/zlink-node-send-saturation-4096/results.json.

## Before confirm-L0-send-saturation-4096-r2
built commit=c512887606aeb686eccc53162d9f02c3c4db63ed dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=d6db0481dc0af4f77e2fc19b8227b478e1a2539cca55d353eb7118106b1139e3
framework/languages/node/packages/framework/dist/runtime/host/application-job-queue.js sha256=07e9734c0f3466cdd3d174518f2b1d9fa357bef66b169a6d699aa6270493fa27
framework/languages/node/packages/framework/dist/index.js sha256=4aedc33b29681b11d37221bf122fd9fe882eb3b005a9bdb173676ec8dbe85ba9
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae

Binding receive/reply: zlink-raw-server/main.js:64-117
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
    91	  }
    92	  return handled;
    93	}
    94	
    95	function pumpCommands() {
    96	  let handled = 0;
    97	  while (handled < batch) {
    98	    let hasMessage;
    99	    try {
   100	      hasMessage = tryRecv(commandRouter, commandReceived);
   101	    } catch (error) {
   102	      metrics.recordError();
   103	      console.error(`raw command loop recv failed: ${error.message}`);
   104	      break;
   105	    }
   106	    if (!hasMessage) break;
   107	    handled += 1;
   108	    try {
   109	      const part = bodyPart(commandReceived);
   110	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
   111	      if (body === null) throw new Error('invalid raw protobuf payload');
   112	      metrics.record(body);
   113	    } catch (error) {
   114	      metrics.recordError();
   115	      console.error(`raw command loop failed: ${error.message}`);
   116	    }
   117	  }

L0 receive/reply: zlink-framework-server/main.js:93-155
    93	  function pump() {
    94	    let handled = 0;
    95	    while (handled < batch) {
    96	      let hasMessage;
    97	      try {
    98	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    99	      } catch (error) {
   100	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   101	        metrics.recordError();
   102	        console.error(`ladder L0 recv failed: ${error.message}`);
   103	        break;
   104	      }
   105	      if (!hasMessage) break;
   106	      handled += 1;
   107	      const parts = received.parts;
   108	      try {
   109	        const header = frameworkWire ? parts[0].data() : null;
   110	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   111	        if (payload === null) throw new Error('invalid benchmark payload');
   112	        if (received.replyToken !== null) {
   113	          echoHandler.handleL0(payload);
   114	          received.reply()
   115	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
   116	            .message(encodeBenchPayloadMessage(payload))
   117	            .submit();
   118	        } else {
   119	          commandHandler.handleL0(payload);
   120	        }
   121	      } catch (error) {
   122	        metrics.recordError();
   123	        console.error(`ladder L0 handler failed: ${error.message}`);
   124	      }
   125	    }
   126	    return handled;
   127	  }
   128	
   129	  function pumpCommands() {
   130	    if (commandRouter === null) return 0;
   131	    let handled = 0;
   132	    while (handled < batch) {
   133	      let hasMessage;
   134	      try {
   135	        hasMessage = commandRouter.recv(commandReceived, zlink.RecvFlags.DontWait);
   136	      } catch (error) {
   137	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   138	        metrics.recordError();
   139	        console.error(`ladder L0 command recv failed: ${error.message}`);
   140	        break;
   141	      }
   142	      if (!hasMessage) break;
   143	      handled += 1;
   144	      try {
   145	        const parts = commandReceived.parts;
   146	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   147	        if (payload === null) throw new Error('invalid benchmark payload');
   148	        commandHandler.handleL0(payload);
   149	      } catch (error) {
   150	        metrics.recordError();
   151	        console.error(`ladder L0 command handler failed: ${error.message}`);
   152	      }
   153	    }
   154	    return handled;
   155	  }

Per-message waits/allocations/atomics/copies: binding and L0/L0R use one reusable Received, DontWait, batch 256, setImmediate; no queue wait, no JS atomics. L1 awaits ApplicationJobQueue.acquire(remote) before each receive and releases at handler entry. All paths decode the protobuf body, allocate a new encoded body and reply operation, and submit two parts; they do not copy or reuse request parts.

Run start L0 send-saturation-4096 r2: 2026-11-13T16:44:26Z

First result L0 send-saturation-4096 r2: 78145.4 msg/s; errors=0 submitted=completed=received=391899. Raw: raw/session/confirm/send-saturation-4096/L0/run2/run1/zlink-framework-node-send-saturation-4096/results.json.

## Before confirm-binding-send-saturation-4096-r3
built commit=c512887606aeb686eccc53162d9f02c3c4db63ed dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=d6db0481dc0af4f77e2fc19b8227b478e1a2539cca55d353eb7118106b1139e3
framework/languages/node/packages/framework/dist/runtime/host/application-job-queue.js sha256=07e9734c0f3466cdd3d174518f2b1d9fa357bef66b169a6d699aa6270493fa27
framework/languages/node/packages/framework/dist/index.js sha256=4aedc33b29681b11d37221bf122fd9fe882eb3b005a9bdb173676ec8dbe85ba9
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae

Binding receive/reply: zlink-raw-server/main.js:64-117
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
    91	  }
    92	  return handled;
    93	}
    94	
    95	function pumpCommands() {
    96	  let handled = 0;
    97	  while (handled < batch) {
    98	    let hasMessage;
    99	    try {
   100	      hasMessage = tryRecv(commandRouter, commandReceived);
   101	    } catch (error) {
   102	      metrics.recordError();
   103	      console.error(`raw command loop recv failed: ${error.message}`);
   104	      break;
   105	    }
   106	    if (!hasMessage) break;
   107	    handled += 1;
   108	    try {
   109	      const part = bodyPart(commandReceived);
   110	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
   111	      if (body === null) throw new Error('invalid raw protobuf payload');
   112	      metrics.record(body);
   113	    } catch (error) {
   114	      metrics.recordError();
   115	      console.error(`raw command loop failed: ${error.message}`);
   116	    }
   117	  }

L0 receive/reply: zlink-framework-server/main.js:93-155
    93	  function pump() {
    94	    let handled = 0;
    95	    while (handled < batch) {
    96	      let hasMessage;
    97	      try {
    98	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    99	      } catch (error) {
   100	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   101	        metrics.recordError();
   102	        console.error(`ladder L0 recv failed: ${error.message}`);
   103	        break;
   104	      }
   105	      if (!hasMessage) break;
   106	      handled += 1;
   107	      const parts = received.parts;
   108	      try {
   109	        const header = frameworkWire ? parts[0].data() : null;
   110	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   111	        if (payload === null) throw new Error('invalid benchmark payload');
   112	        if (received.replyToken !== null) {
   113	          echoHandler.handleL0(payload);
   114	          received.reply()
   115	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
   116	            .message(encodeBenchPayloadMessage(payload))
   117	            .submit();
   118	        } else {
   119	          commandHandler.handleL0(payload);
   120	        }
   121	      } catch (error) {
   122	        metrics.recordError();
   123	        console.error(`ladder L0 handler failed: ${error.message}`);
   124	      }
   125	    }
   126	    return handled;
   127	  }
   128	
   129	  function pumpCommands() {
   130	    if (commandRouter === null) return 0;
   131	    let handled = 0;
   132	    while (handled < batch) {
   133	      let hasMessage;
   134	      try {
   135	        hasMessage = commandRouter.recv(commandReceived, zlink.RecvFlags.DontWait);
   136	      } catch (error) {
   137	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   138	        metrics.recordError();
   139	        console.error(`ladder L0 command recv failed: ${error.message}`);
   140	        break;
   141	      }
   142	      if (!hasMessage) break;
   143	      handled += 1;
   144	      try {
   145	        const parts = commandReceived.parts;
   146	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   147	        if (payload === null) throw new Error('invalid benchmark payload');
   148	        commandHandler.handleL0(payload);
   149	      } catch (error) {
   150	        metrics.recordError();
   151	        console.error(`ladder L0 command handler failed: ${error.message}`);
   152	      }
   153	    }
   154	    return handled;
   155	  }

Per-message waits/allocations/atomics/copies: binding and L0/L0R use one reusable Received, DontWait, batch 256, setImmediate; no queue wait, no JS atomics. L1 awaits ApplicationJobQueue.acquire(remote) before each receive and releases at handler entry. All paths decode the protobuf body, allocate a new encoded body and reply operation, and submit two parts; they do not copy or reuse request parts.

Run start binding send-saturation-4096 r3: 2026-11-13T16:44:45Z

First result binding send-saturation-4096 r3: 77440.2 msg/s; errors=0 submitted=completed=received=387201. Raw: raw/session/confirm/send-saturation-4096/binding/run3/run1/zlink-node-send-saturation-4096/results.json.

## Before confirm-L0-send-saturation-4096-r3
built commit=c512887606aeb686eccc53162d9f02c3c4db63ed dirty=false base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970
framework/bench/grpc/node/zlink-framework-server/main.js sha256=d6db0481dc0af4f77e2fc19b8227b478e1a2539cca55d353eb7118106b1139e3
framework/languages/node/packages/framework/dist/runtime/host/application-job-queue.js sha256=07e9734c0f3466cdd3d174518f2b1d9fa357bef66b169a6d699aa6270493fa27
framework/languages/node/packages/framework/dist/index.js sha256=4aedc33b29681b11d37221bf122fd9fe882eb3b005a9bdb173676ec8dbe85ba9
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node sha256=ed260e4bb579df207784f76ed5e518d214f4baf1bcf485d9f303b917c23dbe05
framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 sha256=85f228638e895335b5b2919661100defcca7fcfe9f7863b473ebeb17dbf649ae

Binding receive/reply: zlink-raw-server/main.js:64-117
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
    91	  }
    92	  return handled;
    93	}
    94	
    95	function pumpCommands() {
    96	  let handled = 0;
    97	  while (handled < batch) {
    98	    let hasMessage;
    99	    try {
   100	      hasMessage = tryRecv(commandRouter, commandReceived);
   101	    } catch (error) {
   102	      metrics.recordError();
   103	      console.error(`raw command loop recv failed: ${error.message}`);
   104	      break;
   105	    }
   106	    if (!hasMessage) break;
   107	    handled += 1;
   108	    try {
   109	      const part = bodyPart(commandReceived);
   110	      const body = part === null ? null : decodeBenchPayloadBody(part.data());
   111	      if (body === null) throw new Error('invalid raw protobuf payload');
   112	      metrics.record(body);
   113	    } catch (error) {
   114	      metrics.recordError();
   115	      console.error(`raw command loop failed: ${error.message}`);
   116	    }
   117	  }

L0 receive/reply: zlink-framework-server/main.js:93-155
    93	  function pump() {
    94	    let handled = 0;
    95	    while (handled < batch) {
    96	      let hasMessage;
    97	      try {
    98	        hasMessage = router.recv(received, zlink.RecvFlags.DontWait);
    99	      } catch (error) {
   100	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   101	        metrics.recordError();
   102	        console.error(`ladder L0 recv failed: ${error.message}`);
   103	        break;
   104	      }
   105	      if (!hasMessage) break;
   106	      handled += 1;
   107	      const parts = received.parts;
   108	      try {
   109	        const header = frameworkWire ? parts[0].data() : null;
   110	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   111	        if (payload === null) throw new Error('invalid benchmark payload');
   112	        if (received.replyToken !== null) {
   113	          echoHandler.handleL0(payload);
   114	          received.reply()
   115	            .message(frameworkWire ? frameworkResponseHeader(header) : RESPONSE_ENVELOPE)
   116	            .message(encodeBenchPayloadMessage(payload))
   117	            .submit();
   118	        } else {
   119	          commandHandler.handleL0(payload);
   120	        }
   121	      } catch (error) {
   122	        metrics.recordError();
   123	        console.error(`ladder L0 handler failed: ${error.message}`);
   124	      }
   125	    }
   126	    return handled;
   127	  }
   128	
   129	  function pumpCommands() {
   130	    if (commandRouter === null) return 0;
   131	    let handled = 0;
   132	    while (handled < batch) {
   133	      let hasMessage;
   134	      try {
   135	        hasMessage = commandRouter.recv(commandReceived, zlink.RecvFlags.DontWait);
   136	      } catch (error) {
   137	        if (error instanceof zlink.RecvError && error.result === zlink.RecvResult.NoData) break;
   138	        metrics.recordError();
   139	        console.error(`ladder L0 command recv failed: ${error.message}`);
   140	        break;
   141	      }
   142	      if (!hasMessage) break;
   143	      handled += 1;
   144	      try {
   145	        const parts = commandReceived.parts;
   146	        const payload = decodeBenchPayloadBody(parts[parts.length - 1].data());
   147	        if (payload === null) throw new Error('invalid benchmark payload');
   148	        commandHandler.handleL0(payload);
   149	      } catch (error) {
   150	        metrics.recordError();
   151	        console.error(`ladder L0 command handler failed: ${error.message}`);
   152	      }
   153	    }
   154	    return handled;
   155	  }

Per-message waits/allocations/atomics/copies: binding and L0/L0R use one reusable Received, DontWait, batch 256, setImmediate; no queue wait, no JS atomics. L1 awaits ApplicationJobQueue.acquire(remote) before each receive and releases at handler entry. All paths decode the protobuf body, allocate a new encoded body and reply operation, and submit two parts; they do not copy or reuse request parts.

Run start L0 send-saturation-4096 r3: 2026-11-13T16:45:06Z

First result L0 send-saturation-4096 r3: 71080.8 msg/s; errors=0 submitted=completed=received=355404. Raw: raw/session/confirm/send-saturation-4096/L0/run3/run1/zlink-framework-node-send-saturation-4096/results.json.
