
## Before binding-send-saturation-1024
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

First result binding send-saturation-1024: 110892.6 msg/s; errors=0 server_errors=0; submitted=561108 completed=561108 target_received=561108. Raw: raw/session/binding/run1/zlink-node-send-saturation-1024/results.json.

## Before L0-send-saturation-1024
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

First result L0 send-saturation-1024: 109492.8 msg/s; errors=0 server_errors=0; submitted=547464 completed=547464 target_received=547464. Raw: raw/session/L0/run1/zlink-framework-node-send-saturation-1024/results.json.

## Before L0R-send-saturation-1024
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
