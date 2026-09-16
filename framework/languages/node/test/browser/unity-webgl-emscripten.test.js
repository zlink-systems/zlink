// The Unity WebGL adapter's jslib boundary, linked by emscripten and run in Chromium.
//
// test/contract/unity-webgl-jslib.test.js drives the same boundary through a
// JavaScript stand-in for the emscripten runtime. That covers the boundary logic
// without a toolchain, but it cannot cover the link itself. This test does:
//
//   - ZlinkStreamConnector.jslib goes through emcc's --js-library, so its syntax,
//     its `$` dependency rules and its symbol names are the real ones.
//   - Both .jspre files go through --pre-js, so "the plugins are linked" is
//     checked by the compiler rather than by hand.
//   - The event sink is a real wasm function pointer reached through wasmTable,
//     which is what IL2CPP produces for an [AOT.MonoPInvokeCallback] method.
//   - Marshalling runs against the real _malloc, _free and HEAPU8, so buffer
//     ownership and leaks are measured on the real allocator.
//   - The optimization levels a Unity release player links at are linked here
//     too, because the committed bundle has to survive emscripten's JS optimizer.
//
// The harness stands in for the IL2CPP output; see support/unity-webgl/harness.c.
const assert = require('node:assert/strict');
const childProcess = require('node:child_process');
const fs = require('node:fs');
const http = require('node:http');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');
const { after, before, describe, it } = require('node:test');
const { closeBrowser, closeBrowserServer, closeContext, closeServer, stopChild } = require('../support/bounded-cleanup');
const {
  EMSDK_VERSION,
  HARNESS_SOURCE,
  INSTALL_HINT,
  OPTIMIZATION,
  buildHarness,
  emccArguments,
  findEmscripten,
  packageRoot,
  runEmcc
} = require('./support/unity-webgl/build');

const workspaceRoot = path.resolve(__dirname, '../..');
process.env.PLAYWRIGHT_BROWSERS_PATH ??= path.join(workspaceRoot, '.cache/ms-playwright');
const serverScript = path.join(__dirname, 'support/stream-server.js');
const driverScript = path.join(__dirname, 'support/unity-webgl/driver.js');

const emscripten = findEmscripten();
const CONNECTOR_OPTIONS = {
  dispatchMode: 'manual',
  heartbeat: { enabled: false },
  reconnect: { enabled: false }
};

describe('Unity WebGL adapter linked by emscripten', { skip: emscripten ? false : INSTALL_HINT, timeout: 600_000 }, () => {
  let outputDirectory;
  let built;
  let withoutAllocator;
  let staticServer;
  let browserServer;
  let browser;
  let context;
  let page;
  let streamServer;

  before(async () => {
    outputDirectory = fs.mkdtempSync(path.join(os.tmpdir(), 'zlink-unity-webgl-'));
    const started = Date.now();
    built = buildHarness(emscripten, path.join(outputDirectory, 'unity-like'));
    built.seconds = (Date.now() - started) / 1000;
    withoutAllocator = buildHarness(emscripten, path.join(outputDirectory, 'no-allocator'), {
      exportAllocator: false
    });

    staticServer = await startStaticServer(built, withoutAllocator);
    browserServer = await require('playwright').chromium.launchServer({ headless: true });
    browser = await require('playwright').chromium.connect({ wsEndpoint: browserServer.wsEndpoint() });
    context = await browser.newContext();
    page = await context.newPage();
    page.on('pageerror', (error) => { process.stderr.write(`page error: ${error.stack ?? error.message}\n`); });
    page.on('console', (message) => {
      if (message.type() === 'error') process.stderr.write(`page console: ${message.text()}\n`);
    });
    await page.goto(staticServer.url);
    const linked = await page.evaluate(() => window.harnessReady);

    // The package README's manual check - "the built framework.js contains
    // ZlinkStreamWebGlRuntime and ZlinkStreamConnectorBundle" - asserted here.
    assert.deepEqual(linked, { runtime: true, bundle: true }, 'both .jspre plugins must be linked');
    assert.match(fs.readFileSync(built.js, 'utf8'), /_ZlinkStreamPump/, 'the jslib must be linked into the module');
  });

  after(async () => {
    await closeContext(context).catch(() => undefined);
    await closeBrowser(browser).catch(() => undefined);
    await closeBrowserServer(browserServer).catch(() => undefined);
    await stopChild(streamServer).catch(() => undefined);
    await closeServer(staticServer?.server).catch(() => undefined);
    if (outputDirectory) fs.rmSync(outputDirectory, { recursive: true, force: true });
  });

  // wasm-ld resolves the harness's externs against the jslib, so a name that only
  // exists on one side fails the link. What that cannot catch is the harness
  // drifting from the C# it stands in for, which is what this compares.
  it('links every entry point ZlinkStreamInterop.cs declares', () => {
    process.stdout.write(
      `# emscripten ${EMSDK_VERSION} ${OPTIMIZATION}: linked in ${built.seconds.toFixed(1)}s, ` +
      `${(fs.statSync(built.js).size / 1024).toFixed(0)} KiB js + ` +
      `${(fs.statSync(built.wasm).size / 1024).toFixed(0)} KiB wasm\n`
    );

    const interop = fs.readFileSync(path.join(packageRoot, 'Runtime/Interop/ZlinkStreamInterop.cs'), 'utf8');
    const declared = [...interop.matchAll(/EntryPoint = "(\w+)"/g)].map(([, name]) => name).sort();
    const harness = fs.readFileSync(HARNESS_SOURCE, 'utf8');
    const externs = [...harness.matchAll(/^extern [\w *]+?\**(\w+)\(/gm)].map(([, name]) => name).sort();

    assert.equal(declared.length, 20, 'the C# boundary should declare 20 entry points');
    assert.deepEqual(externs, declared, 'the C harness must declare the same boundary as the C# side');

    const linked = fs.readFileSync(built.js, 'utf8');
    for (const name of declared) {
      assert.match(linked, new RegExp(`\\b_${name}\\b`), `${name} is not in the linked module`);
    }
  });

  it('rejects the endpoints the browser sandbox cannot open', async () => {
    const outcome = await page.evaluate(async (endpoints) => {
      window.zl.raw.reset();
      const results = [];
      for (const endpoint of endpoints) {
        const before = window.zl.raw.heapInUse();
        try {
          window.zl.create({ ...window.connectorOptions, endpoint });
          results.push({ endpoint, created: true });
        } catch (error) {
          results.push({ endpoint, created: false, detail: error.detail, leaked: window.zl.raw.heapInUse() - before });
        }
      }
      return results;
    }, ['tcp://127.0.0.1:19000', 'tls://127.0.0.1:19000']);

    for (const result of outcome) {
      assert.equal(result.created, false, `${result.endpoint} must not create a connector`);
      assert.equal(JSON.parse(result.detail).code, 'configurationError', result.detail);
      // TakeLastError is the one buffer the managed side owns. It was freed with
      // ZlinkStreamFreeBuffer, so the failed create left nothing on the heap.
      assert.equal(result.leaked, 0, `${result.endpoint} left ${result.leaked} bytes allocated`);
    }
  });

  it('drives a real STREAM server through the linked boundary', async () => {
    const port = await freePort();
    streamServer = await startStreamServer(`ws://127.0.0.1:${port}`);
    const report = await page.evaluate(async (endpoint) => {
      const zl = window.zl;
      const steps = {};
      zl.create({ ...window.connectorOptions, endpoint });
      steps.beforeConnect = { state: zl.raw.state(), diagnosticsLevel: zl.raw.diagnosticsLevel() };

      await zl.connect();
      steps.afterConnect = { state: zl.raw.state(), isConnected: zl.raw.isConnected() === 1 };

      // The payload buffer is poisoned and freed the instant Request returns, so
      // a reply that still carries the original value proves the jslib copied out
      // of HEAPU8 before returning.
      const reply = await zl.request(JSON.stringify({ value: 'emscripten-request' }), {
        codec: 1, packetName: 'EchoReq', metadata: { tenant: 'unity' }, timeoutMs: 10000
      });
      steps.reply = { text: reply.text, payload: reply.payload };

      // No handler registered: the push waits in the unread history and the wait
      // surface consumes it without a Dispatch.
      const observed = await zl.waitFor('EchoPush', 10000);
      steps.observed = observed;
      steps.handlerRunsAfterWait = zl.raw.handlerRuns();
      steps.receivedAfterWait = zl.raw.receivedCount('EchoPush');

      // A registered handler takes the next one instead, and must not run before Dispatch.
      zl.raw.registerHandler('EchoPush');
      await zl.send(JSON.stringify({ value: 'emscripten-send' }), { codec: 1, packetName: 'EchoReq' });
      await zl.untilPending(10000);
      steps.beforeDispatch = { pending: zl.raw.pendingDispatch(), handlerRuns: zl.raw.handlerRuns() };
      await zl.dispatch(10000);
      steps.afterDispatch = { pending: zl.raw.pendingDispatch(), log: zl.handlerLog() };

      await zl.close(10000);
      steps.afterClose = { state: zl.raw.state(), isConnected: zl.raw.isConnected() === 1 };
      await zl.dispatch(10000);
      steps.stateChanges = zl.stateLog();
      steps.snapshot = zl.snapshot();
      return steps;
    }, `ws://127.0.0.1:${port}`);

    // Spec 32 sections 2.2 and 6.1: manual dispatch is the game-engine default and
    // the connector starts in Created with Errors diagnostics.
    assert.deepEqual(report.beforeConnect, { state: 0, diagnosticsLevel: 1 });
    assert.deepEqual(report.afterConnect, { state: 2, isConnected: true });
    assert.equal(JSON.parse(report.reply.payload).value, 'emscripten-request');
    assert.equal(JSON.parse(report.reply.text).codec, 1);

    // Spec 32 section 10.1.1: the wait surface consumes the unread queue and runs
    // no registered callback.
    assert.equal(report.observed.name, 'EchoPush');
    assert.equal(report.observed.codec, 1);
    assert.equal(JSON.parse(report.observed.payload).value, 'emscripten-request');
    assert.equal(report.handlerRunsAfterWait, 0);
    assert.equal(report.receivedAfterWait, 0);

    assert.equal(report.beforeDispatch.handlerRuns, 0, 'manual dispatch must hold the handler until Dispatch runs');
    assert.ok(report.beforeDispatch.pending > 0);
    assert.equal(report.afterDispatch.pending, 0);
    assert.deepEqual(
      report.afterDispatch.log.map((line) => line.split('|')).map(([name, codec, payload]) => ({
        name, codec: Number(codec), value: JSON.parse(payload).value
      })),
      [{ name: 'EchoPush', codec: 1, value: 'emscripten-send' }]
    );

    assert.deepEqual(report.afterClose, { state: 5, isConnected: false });
    assert.ok(
      report.stateChanges.some((change) => change.current === 'closed'),
      `connection state changes reached the managed side: ${JSON.stringify(report.stateChanges)}`
    );
    assert.ok(report.snapshot.disconnects >= 1, 'the disconnect callback reached the managed side');
    assert.equal(report.snapshot.pumpFailures, 0, report.snapshot.pumpFailureText);
    assert.equal(report.snapshot.violations, '', 'the sink saw a malformed event');

    await stopChild(streamServer);
    streamServer = undefined;
  });

  it('refuses a nested pump and never delivers an event twice', async () => {
    const port = await freePort();
    streamServer = await startStreamServer(`ws://127.0.0.1:${port}`);
    const report = await page.evaluate(async (endpoint) => {
      const zl = window.zl;
      zl.create({ ...window.connectorOptions, endpoint });
      // The sink calls ZlinkStreamPump again on every event: user code re-entering
      // the boundary from inside the callback.
      zl.raw.setNestedPump(1);
      await zl.connect();
      zl.raw.registerHandler('EchoPush');
      for (const value of ['nested-a', 'nested-b', 'nested-c']) {
        await zl.request(JSON.stringify({ value }), { codec: 1, packetName: 'EchoReq', timeoutMs: 10000 });
      }
      await zl.frames(20);
      await zl.dispatch(10000);
      await zl.close(10000);
      await zl.dispatch(10000);
      const snapshot = zl.snapshot();
      const log = zl.handlerLog();
      zl.raw.setNestedPump(0);
      zl.raw.destroy();
      return { snapshot, log };
    }, `ws://127.0.0.1:${port}`);

    assert.ok(report.snapshot.nestedPumpCalls > 0, 'the sink ran at least once');
    assert.equal(
      report.snapshot.nestedPumpNotRefused,
      0,
      `every nested ZlinkStreamPump must return -1; ${report.snapshot.nestedPumpNotRefused} of ` +
      `${report.snapshot.nestedPumpCalls} did not`
    );

    const values = report.log.map((line) => JSON.parse(line.split('|').slice(2).join('|')).value);
    assert.deepEqual(values, ['nested-a', 'nested-b', 'nested-c'], 'each push must arrive exactly once');
    assert.equal(report.snapshot.pumpFailures, 0, report.snapshot.pumpFailureText);
    assert.equal(report.snapshot.violations, '');

    await stopChild(streamServer);
    streamServer = undefined;
  });

  it('frees every boundary buffer it allocates', async () => {
    const port = await freePort();
    streamServer = await startStreamServer(`ws://127.0.0.1:${port}`);
    const rounds = 60;
    const report = await page.evaluate(async ([endpoint, iterations]) => {
      const zl = window.zl;
      zl.create({ ...window.connectorOptions, endpoint });
      await zl.connect();
      zl.raw.observe('EchoPush');

      // One warm-up round trip so dlmalloc's arena is at its steady state before
      // the baseline is taken.
      const round = async (value) => {
        await zl.request(JSON.stringify({ value }), { codec: 1, packetName: 'EchoReq', timeoutMs: 10000 });
        await zl.waitFor('EchoPush', 10000);
      };
      await round('warmup');
      const baseline = { heapInUse: zl.raw.heapInUse(), liveAllocs: zl.raw.liveAllocs(), heapBreak: zl.raw.heapBreak() };
      for (let index = 0; index < iterations; index += 1) await round(`leak-${index}`);
      const settled = { heapInUse: zl.raw.heapInUse(), liveAllocs: zl.raw.liveAllocs(), heapBreak: zl.raw.heapBreak() };

      await zl.close(10000);
      await zl.dispatch(10000);
      zl.raw.destroy();
      const afterDestroy = { heapInUse: zl.raw.heapInUse(), liveAllocs: zl.raw.liveAllocs() };
      return { baseline, settled, afterDestroy, sinkCalls: zl.raw.sinkCalls(), violations: zl.raw.violations() };
    }, [`ws://127.0.0.1:${port}`, rounds]);

    process.stdout.write(
      `# leak check: ${rounds} round trips, ${report.sinkCalls} sink calls, ` +
      `dlmalloc uordblks ${report.baseline.heapInUse} -> ${report.settled.heapInUse}, ` +
      `sbrk ${report.baseline.heapBreak} -> ${report.settled.heapBreak}\n`
    );

    // The managed side's own bookkeeping first, so a heap delta can be attributed.
    assert.equal(report.settled.liveAllocs, report.baseline.liveAllocs, 'the harness itself leaked');
    // dlmalloc's uordblks counts bytes handed out and not returned. The only wasm
    // heap traffic during a round trip is the jslib's pump buffers and the
    // harness's copies of them, so any growth here is an unfreed boundary buffer.
    assert.equal(
      report.settled.heapInUse,
      report.baseline.heapInUse,
      `${report.settled.heapInUse - report.baseline.heapInUse} bytes were not freed over ${rounds} round trips`
    );
    assert.equal(report.violations, '');

    await stopChild(streamServer);
    streamServer = undefined;
  });

  // ZlinkStreamConnector.jslib declares `malloc` and `free` in a __deps list, so
  // emscripten pulls the _malloc and _free bindings in on the library's own say-so.
  // This build drops them from -sEXPORTED_FUNCTIONS: the boundary must still
  // allocate, because the requirement belongs to the library and not to the host
  // link. Delete the __deps entries and this goes back to
  // "ReferenceError: _malloc is not defined" on the first buffer.
  it('brings its own _malloc and _free in, without the host link exporting them', async () => {
    assert.ok(
      !emccArguments(HARNESS_SOURCE, 'x.js', { exportAllocator: false }).some((flag) =>
        flag.startsWith('-sEXPORTED_FUNCTIONS')),
      'this build must not export the allocator'
    );
    const port = await freePort();
    streamServer = await startStreamServer(`ws://127.0.0.1:${port}`);
    const other = await context.newPage();
    try {
      await other.goto(`${staticServer.url}/no-allocator/`);
      await other.evaluate(() => window.harnessReady);
      const report = await other.evaluate(async (endpoint) => {
        const zl = window.zl;
        const steps = {};
        // Every allocating entry point: the failed create's last-error string,
        // then the pump's text and payload buffers.
        try {
          zl.create({ ...window.connectorOptions, endpoint: 'tcp://127.0.0.1:19000' });
          steps.rejected = null;
        } catch (error) {
          steps.rejected = error.detail;
        }
        zl.create({ ...window.connectorOptions, endpoint });
        await zl.connect();
        const reply = await zl.request(JSON.stringify({ value: 'no-allocator-export' }), {
          codec: 1, packetName: 'EchoReq', timeoutMs: 10000
        });
        steps.reply = reply.payload;
        steps.observed = await zl.waitFor('EchoPush', 10000);
        await zl.close(10000);
        steps.snapshot = zl.snapshot();
        zl.raw.destroy();
        return steps;
      }, `ws://127.0.0.1:${port}`);

      assert.equal(JSON.parse(report.rejected).code, 'configurationError', 'TakeLastError must allocate');
      assert.equal(JSON.parse(report.reply).value, 'no-allocator-export');
      assert.equal(JSON.parse(report.observed.payload).value, 'no-allocator-export');
      assert.equal(report.snapshot.pumpFailures, 0, report.snapshot.pumpFailureText);
      assert.equal(report.snapshot.violations, '');
    } finally {
      await other.close();
      await stopChild(streamServer);
      streamServer = undefined;
    }
  });

  // Before the pump reported failures it logged to the console and returned the
  // partial count, which reads exactly like "no events yet": the managed side kept
  // waiting for events that had stopped coming. The fault is injected from the page
  // so the path is exercised without the adapter having to be broken.
  it('reports a pump failure to the managed side instead of going quiet', async () => {
    const port = await freePort();
    streamServer = await startStreamServer(`ws://127.0.0.1:${port}`);
    const report = await page.evaluate(async (endpoint) => {
      const zl = window.zl;
      zl.create({ ...window.connectorOptions, endpoint });
      await zl.connect();
      const runtime = globalThis.ZlinkStreamWebGlRuntime;
      const original = runtime.takeEvent;
      runtime.takeEvent = function () { throw new Error('injected pump fault'); };
      let result;
      try {
        result = zl.raw.pump();
      } finally {
        runtime.takeEvent = original;
      }
      const after = { failures: zl.raw.pumpFailures(), text: zl.raw.pumpFailureText() };
      // The boundary keeps working once the fault is gone.
      const recovered = zl.raw.pump();
      await zl.close(10000);
      zl.raw.destroy();
      return { result, after, recovered };
    }, `ws://127.0.0.1:${port}`);

    assert.equal(report.result, -2, 'a failed pump must not look like a count');
    assert.equal(report.after.failures, 1);
    assert.deepEqual(JSON.parse(report.after.text), {
      code: 'pumpFailed',
      message: 'injected pump fault'
    });
    assert.ok(report.recovered >= 0, 'the pump guard must be released after a failure');

    await stopChild(streamServer);
    streamServer = undefined;
  });

  // Unity's Code Optimization settings above "Shorter Build Time" link with -O2 or
  // higher, which hands the concatenated --pre-js content to emscripten 3.1.38's
  // JS optimizer. Two of its stages set the language level the committed bundle
  // has to stay inside: the parser is fixed at ecmaVersion 2020, and the terser
  // that converts the tree predates ES2020 and has no ChainExpression handler. The
  // IIFE bundle is built for es2019 for both reasons; raise the target again and
  // these links die with "SyntaxError: Unexpected token" or "MOZ_TO_ME[node.type]
  // is not a function".
  it('links at every optimization level a Unity release player uses', () => {
    for (const optimization of ['-O2', '-O3', '-Os']) {
      const output = path.join(outputDirectory, `release${optimization.slice(1)}/harness.js`);
      fs.mkdirSync(path.dirname(output), { recursive: true });
      const result = runEmcc(emscripten, emccArguments(HARNESS_SOURCE, output, { optimization }));
      assert.equal(result.status, 0, `${optimization}: ${result.stdout ?? ''}${result.stderr ?? ''}`);
      assert.ok(fs.existsSync(output), `${optimization} produced no module`);
    }
  });

  // The third stage of that optimizer, JSDCE, is not a language-level limit but a
  // bug, and no bundler setting reaches it: esbuild refuses to lower destructuring
  // while the target still allows async. JSDCE's VariableDeclarator handler reads
  // `node.id.name`, which is undefined for a pattern, so every destructuring
  // declaration registers a binding literally named "undefined". A scope with no
  // reference to the identifier `undefined` therefore has one that is defined and
  // never used, and the cleanup deletes every declarator whose id.name is undefined
  // - the destructuring declarations. The connector's message drain loses
  // `const { message, signal } = queued` and the player fails at runtime with
  // "ReferenceError: message is not defined", after a link that reported success.
  //
  // What does work, verified by the assertion below: --extern-pre-js puts the same
  // bundle outside the module, where emscripten emits it after the optimizer has
  // run. Unity's importer only ever passes --pre-js for a .jspre, so reaching it
  // needs PlayerSettings.WebGL.emscriptenArgs.
  it('leaves the bundle intact at a Unity release optimization level', { todo: 'emscripten 3.1.38 JSDCE deletes destructuring declarations from --pre-js content' }, () => {
    // The --extern-pre-js link is the same bundle with the optimizer skipped, so
    // it says how many destructuring declarations the bundle has. Counting them
    // in both outputs reports every one JSDCE deleted, not just the known name.
    const counted = destructuringDeclarations();
    assert.equal(
      counted.optimized,
      counted.untouched,
      `JSDCE deleted ${counted.untouched - counted.optimized} of ${counted.untouched} destructuring declarations`
    );
  });

  it('leaves the bundle intact when the optimizer never sees it', () => {
    const output = link('extern', { optimization: '-O2', externBundle: true });
    const linked = fs.readFileSync(output, 'utf8');
    assert.match(linked, /const \{ message, signal \} = queued/, '--extern-pre-js content must reach the output unrewritten');
    assert.ok(
      (linked.match(DESTRUCTURING) ?? []).length > 0,
      'the untouched bundle is the baseline for how many destructuring declarations there are'
    );
    assert.match(linked, /ZlinkStreamConnectorBundle/);
  });

  function link(name, options) {
    const output = path.join(outputDirectory, `${name}/harness.js`);
    fs.mkdirSync(path.dirname(output), { recursive: true });
    const result = runEmcc(emscripten, emccArguments(HARNESS_SOURCE, output, options));
    assert.equal(result.status, 0, `${name}: ${result.stdout ?? ''}${result.stderr ?? ''}`);
    return output;
  }

  // The start of `const {a, b} = x` or `for (const [k, v] of xs)`, before and
  // after minification. Counting the openers avoids matching nested braces.
  const DESTRUCTURING = /\b(?:const|let|var)\s*[{[]/g;

  function destructuringDeclarations() {
    const count = (file) => (fs.readFileSync(file, 'utf8').match(DESTRUCTURING) ?? []).length;
    return {
      optimized: count(link('jsdce', { optimization: '-O2' })),
      untouched: count(link('jsdce-extern', { optimization: '-O2', externBundle: true }))
    };
  }
});

async function startStaticServer(unityLike, withoutAllocator) {
  const driver = fs.readFileSync(driverScript);
  const files = new Map([
    ['/harness.js', { body: fs.readFileSync(unityLike.js), type: 'text/javascript' }],
    ['/harness.wasm', { body: fs.readFileSync(unityLike.wasm), type: 'application/wasm' }],
    ['/driver.js', { body: driver, type: 'text/javascript' }],
    ['/no-allocator/harness.js', { body: fs.readFileSync(withoutAllocator.js), type: 'text/javascript' }],
    ['/no-allocator/harness.wasm', { body: fs.readFileSync(withoutAllocator.wasm), type: 'application/wasm' }],
    ['/no-allocator/driver.js', { body: driver, type: 'text/javascript' }]
  ]);
  // Relative script sources so the same document works under / and /no-allocator/,
  // which is also how emscripten resolves the .wasm beside its loader.
  const page = `<!doctype html><meta charset="utf-8">
<script src="harness.js"></script>
<script src="driver.js"></script>
<script>
  window.connectorOptions = ${JSON.stringify(CONNECTOR_OPTIONS)};
  window.harnessReady = startZlinkUnityHarness().then((harness) => {
    window.zl = harness;
    return harness.linkedPlugins;
  });
</script>`;
  const server = http.createServer((request, response) => {
    const file = files.get(request.url);
    if (file) {
      response.writeHead(200, { 'content-type': file.type });
      response.end(file.body);
      return;
    }
    response.writeHead(200, { 'content-type': 'text/html' });
    response.end(page);
  });
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  return { url: `http://127.0.0.1:${server.address().port}`, server };
}

function startStreamServer(endpoint) {
  const child = childProcess.spawn(process.execPath, [serverScript, '--endpoint', endpoint], {
    cwd: workspaceRoot,
    stdio: ['ignore', 'pipe', 'pipe']
  });
  let output = '';
  child.stdout.on('data', (chunk) => { output += chunk; });
  child.stderr.on('data', (chunk) => { output += chunk; });
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`Stream server start timeout: ${output}`)), 15_000);
    const check = () => {
      if (!output.includes('"event":"ready"')) return;
      clearTimeout(timer);
      child.stdout.off('data', check);
      child.capturedOutput = () => output;
      resolve(child);
    };
    child.stdout.on('data', check);
    child.once('exit', (code) => {
      if (!output.includes('"event":"ready"')) {
        clearTimeout(timer);
        reject(new Error(`Stream server exited ${code}: ${output}`));
      }
    });
  });
}

function freePort() {
  return new Promise((resolve, reject) => {
    const server = net.createServer();
    server.once('error', reject);
    server.listen(0, '127.0.0.1', () => {
      const { port } = server.address();
      server.close(() => resolve(port));
    });
  });
}
