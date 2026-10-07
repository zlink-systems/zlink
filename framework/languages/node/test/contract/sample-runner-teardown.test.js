const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const { once } = require('node:events');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname, '../../samples/run-sample.mjs'), 'utf8');
function lifecycleDeclarations(scope) {
  function declaration(start, end) {
    return source.slice(source.indexOf(start), source.indexOf(end, source.indexOf(start)));
  }
  const functions = [
    declaration('function createContext(', 'async function waitForExit('),
    declaration('async function waitForExit(', 'async function reserveBrowserSafePort('),
    declaration('function ensureChildrenRunning(', 'function run('),
    declaration('async function cleanup()', 'function printLogs()')
  ];
  const referenced = new Set();
  for (const fn of functions) {
    const parameters = [
      ...[...fn.matchAll(/\bfunction(?:\s+[A-Za-z_$][\w$]*)?\s*\(([^)]*)\)/g)].map(
        ([, names]) => names
      ),
      ...[...fn.matchAll(/\b(?:async\s+)?([A-Za-z_$][\w$]*)\s*\(([^)]*)\)\s*\{/g)]
        .filter(([, name]) => !['if', 'for', 'while', 'switch', 'catch', 'with'].includes(name))
        .map(([, , names]) => names),
      ...[...fn.matchAll(/\(([^()]*)\)\s*=>/g)].map(([, names]) => names),
      ...[...fn.matchAll(/\b([A-Za-z_$][\w$]*)\s*=>/g)].map(([, name]) => name)
    ];
    const localParameters = new Set(
      parameters.flatMap((names) =>
        [...names.matchAll(/\b[A-Za-z_$][\w$]*\b/g)].map(([name]) => name)
      )
    );
    for (const [name] of fn.matchAll(/\b[A-Za-z_$][\w$]*\b/g)) {
      if (!localParameters.has(name)) referenced.add(name);
    }
  }
  const provided = new Set(Object.keys(scope));
  const constants = [...source.matchAll(/^const\s+([A-Za-z_$][\w$]*)\s*=\s*[\s\S]*?;/gm)]
    .filter(([, name]) => referenced.has(name) && !provided.has(name))
    .map(([constant]) => constant);
  return [...constants, ...functions];
}
function runner(children) {
  // Run the runner's actual lifecycle functions without its build/Redis/browser entry point.
  const scope = vm.createContext({
    children,
    cleaning: false,
    redisContainer: undefined,
    portLeases: new Map(),
    process,
    fs,
    path,
    setTimeout,
    clearTimeout,
    sampleRoot: os.tmpdir(),
    runnerOptions: {},
    nodeRoot: os.tmpdir(),
    logDir: os.tmpdir(),
    runDir: os.tmpdir(),
    workDir: os.tmpdir(),
    sleep: (ms) => new Promise((resolve) => setTimeout(resolve, ms)),
    reserveBrowserSafePort() {},
    waitTcp() {},
    waitHttp() {},
    waitLog() {},
    waitAnyLog() {},
    assertLogCount() {}
  });
  vm.runInContext(lifecycleDeclarations(scope).join('\n'), scope);
  return {
    context: scope.createContext('unused'),
    cleanup: scope.cleanup,
    ensure: scope.ensureChildrenRunning
  };
}
async function role(t, behavior) {
  const child = spawn(
    process.execPath,
    [
      '-e',
      `
    const timer = setInterval(() => {}, 1000);
    for (const signal of ['SIGINT', 'SIGTERM']) process.on(signal, () => { ${behavior} });
    console.log('ready');
  `
    ],
    { stdio: ['pipe', 'pipe', 'pipe'] }
  );
  const state = { child, name: 'role', logPath: 'role.log', closed: false };
  state.exited = new Promise((resolve) =>
    child.once('close', () => {
      state.closed = true;
      resolve(state.status);
    })
  );
  child.once('exit', (code, signal) => {
    state.exitCode = code;
    state.signalCode = signal;
    state.status = code ?? (signal ? 1 : 0);
  });
  t.after(async () => {
    if (child.exitCode === null && child.signalCode === null) {
      const exited = once(child, 'exit');
      child.kill('SIGKILL');
      await exited;
    }
  });
  await once(child.stdout, 'data');
  return state;
}

test('scenario reaps an intentional owner SIGKILL before clean teardown', async (t) => {
  const owner = await role(t, 'clearInterval(timer);');
  const survivor = await role(t, 'clearInterval(timer);');
  survivor.name = 'survivor';
  const runtime = runner([owner, survivor]);
  await runtime.context.stop('role', 'SIGKILL');
  assert.equal(owner.signalCode, 'SIGKILL');
  runtime.ensure();
  await runtime.cleanup();
  assert.equal(survivor.exitCode, 0);
});

test('cleanup waits for the role termination boundary', async (t) => {
  const state = await role(
    t,
    `
    console.log('stopping');
    process.stdin.once('data', () => { clearInterval(timer); process.stdin.destroy(); });
  `
  );
  const stopping = once(state.child.stdout, 'data');
  let completed = false;
  const cleanup = runner([state])
    .cleanup()
    .then(() => {
      completed = true;
    });
  await stopping;
  assert.equal(completed, false);
  state.child.stdin.end('finish');
  await cleanup;
  assert.equal(state.exitCode, 0);
});

test('cleanup reports an externally killed role that does not terminate', async (t) => {
  const state = await role(t, "console.log('stopping');");
  const stopping = once(state.child.stdout, 'data');
  const rejected = assert.rejects(runner([state]).cleanup(), /role.*cleanup.*SIGKILL/);
  await stopping;
  state.child.kill('SIGKILL');
  await rejected;
});

test('cleanup still reports a role killed after cleanup starts', async (t) => {
  const state = await role(t, "process.kill(process.pid, 'SIGKILL');");
  await assert.rejects(runner([state]).cleanup(), /role.*cleanup.*SIGKILL/);
});

test('unexpected early exit remains a scenario failure', async (t) => {
  const state = await role(t, 'clearInterval(timer);');
  const exited = once(state.child, 'exit');
  state.child.kill('SIGINT');
  await exited;
  assert.throws(runner([state]).ensure, /role exited before the sample client ran/);
});
