const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const { once } = require('node:events');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname, '../../samples/run-sample.mjs'), 'utf8');
function declaration(start, end) {
  return source.slice(source.indexOf(start), source.indexOf(end, source.indexOf(start)));
}
function runner(children) {
  // Run the runner's actual lifecycle functions without its build/Redis/browser entry point.
  const scope = vm.createContext({
    children, cleaning: false, redisContainer: undefined, portLeases: new Map(),
    process, fs, path, setTimeout, clearTimeout, sampleRoot: os.tmpdir(), runnerOptions: {},
    nodeRoot: os.tmpdir(), logDir: os.tmpdir(), runDir: os.tmpdir(), workDir: os.tmpdir(),
    sleep: ms => new Promise(resolve => setTimeout(resolve, ms)),
    reserveBrowserSafePort() {}, waitTcp() {}, waitHttp() {}, waitLog() {}, waitAnyLog() {}, assertLogCount() {}
  });
  vm.runInContext([
    declaration('function createContext(', 'async function waitForExit('),
    declaration('async function waitForExit(', 'async function reserveBrowserSafePort('),
    declaration('function ensureChildrenRunning(', 'function run('),
    declaration('async function cleanup()', 'function printLogs()')
  ].join('\n'), scope);
  return { context: scope.createContext('unused'), cleanup: scope.cleanup, ensure: scope.ensureChildrenRunning };
}
async function role(t, behavior) {
  const child = spawn(process.execPath, ['-e', `
    const timer = setInterval(() => {}, 1000);
    for (const signal of ['SIGINT', 'SIGTERM']) process.on(signal, () => { ${behavior} });
    console.log('ready');
  `], { stdio: ['ignore', 'pipe', 'pipe'] });
  const state = { child, name: 'role', logPath: 'role.log', closed: false };
  state.exited = new Promise(resolve => child.once('close', () => { state.closed = true; resolve(state.status); }));
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

test('scenario reaps an intentional owner SIGKILL before clean teardown', async t => {
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

test('cleanup reports a role that ignores SIGINT and needs SIGKILL', async t => {
  const state = await role(t, '');
  await assert.rejects(runner([state]).cleanup(), /role.*cleanup.*SIGKILL/);
});

test('cleanup still reports a role killed after cleanup starts', async t => {
  const state = await role(t, "process.kill(process.pid, 'SIGKILL');");
  await assert.rejects(runner([state]).cleanup(), /role.*cleanup.*SIGKILL/);
});

test('unexpected early exit remains a scenario failure', async t => {
  const state = await role(t, 'clearInterval(timer);');
  const exited = once(state.child, 'exit');
  state.child.kill('SIGINT');
  await exited;
  assert.throws(runner([state]).ensure, /role exited before the sample client ran/);
});
