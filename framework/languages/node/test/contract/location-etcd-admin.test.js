const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { setTimeout: delay } = require('node:timers/promises');
const test = require('node:test');

const repository = path.resolve(__dirname, '../../../../..');
const packageDirectory = path.resolve(__dirname, '../../packages/framework-locations-etcd');
const admin = path.join(packageDirectory, 'dist/admin.js');
const logDirectory = process.env.ZLINK_ETCD_TEST_LOG_DIR || path.join(os.tmpdir(), 'zlink-etcd-admin-tests');

function child(executable, args, options = {}) {
  const processHandle = spawn(executable, args, { windowsHide: true, ...options });
  let stdout = '';
  let stderr = '';
  processHandle.stdout?.on('data', (bytes) => { stdout += bytes; });
  processHandle.stderr?.on('data', (bytes) => { stderr += bytes; });
  const exited = new Promise((resolve, reject) => {
    processHandle.on('error', reject);
    processHandle.on('exit', (code, signal) => resolve({ code, signal, stdout, stderr }));
  });
  return { processHandle, exited };
}

async function command(args, name, timeout = 180000) {
  const running = child(process.execPath, args, { cwd: repository });
  const timer = setTimeout(() => running.processHandle.kill(), timeout);
  try {
    const result = await running.exited;
    fs.mkdirSync(logDirectory, { recursive: true });
    fs.writeFileSync(path.join(logDirectory, `${name}.log`), `${result.stdout}\n${result.stderr}`);
    return result;
  } finally { clearTimeout(timer); }
}

function writeConfiguration(directory, name, value) {
  const file = path.join(directory, `${name}.json`);
  fs.writeFileSync(file, JSON.stringify(value));
  return file;
}

async function fakeServer(handlers) {
  const grpc = require('@grpc/grpc-js');
  const { protoDefinition } = require('../../packages/framework-locations-etcd/dist/internal/client');
  const definitions = grpc.loadPackageDefinition(protoDefinition()).etcdserverpb;
  const server = new grpc.Server();
  if (handlers.KV) server.addService(definitions.KV.service, handlers.KV);
  if (handlers.Auth) server.addService(definitions.Auth.service, handlers.Auth);
  const port = await new Promise((resolve, reject) => {
    server.bindAsync('127.0.0.1:0', grpc.ServerCredentials.createInsecure(), (error, bound) => error ? reject(error) : resolve(bound));
  });
  return { endpoint: `http://127.0.0.1:${port}`, close: () => server.forceShutdown() };
}

test('etcd calls classify status and description together and preserve the error', () => {
  const { classifyError, EtcdCallError, CHANNEL_OPTIONS } = require('../../packages/framework-locations-etcd/dist/internal/client');
  const cases = [
    [11, 'etcdserver: mvcc: required revision has been compacted', 'compaction'],
    [8, 'etcdserver: mvcc: database space exceeded', 'nospace'],
    [16, 'etcdserver: invalid auth token', 'auth'],
    [3, 'etcdserver: revision of auth store is old', 'auth'],
    [11, 'another out of range error', 'storeFailure'],
    [8, 'client received message larger than maximum', 'storeFailure'],
    [16, 'etcdserver: authentication failed, invalid user ID or password', 'storeFailure'],
    [3, 'invalid auth token', 'storeFailure'],
    [14, 'database space exceeded', 'storeFailure']
  ];
  for (const [code, description, kind] of cases) {
    assert.equal(classifyError(code, description), kind);
    const error = new EtcdCallError(kind, code, description, 'http://endpoint');
    assert.equal(error.code, code);
    assert.equal(error.description, description);
    assert.equal(error.endpoint, 'http://endpoint');
  }
  assert.equal(CHANNEL_OPTIONS['grpc.enable_retries'], 0);
  assert.equal(CHANNEL_OPTIONS['grpc.service_config_disable_resolution'], 1);
  assert.equal(CHANNEL_OPTIONS['grpc.max_send_message_length'], 16 * 1024 * 1024);
  assert.equal(CHANNEL_OPTIONS['grpc.max_receive_message_length'], 8 * 1024 * 1024);
});

test('only a rejected authentication RPC is replayed once, within the original deadline', async () => {
  const { EtcdClient } = require('../../packages/framework-locations-etcd/dist/internal/client');
  let authCalls = 0;
  let writes = 0;
  let rejectAgain = false;
  const deadlines = [];
  const server = await fakeServer({
    Auth: { authenticate(call, callback) {
      authCalls++;
      deadlines.push(Number(call.getDeadline()));
      assert.equal(call.request.name, 'test-user');
      callback(null, { token: `token-${authCalls}` });
    } },
    KV: { txn(call, callback) {
      writes++;
      deadlines.push(Number(call.getDeadline()));
      assert.deepEqual(call.metadata.get('token'), [`token-${authCalls}`]);
      if (writes === 1 || rejectAgain) callback({ code: 16, details: 'etcdserver: invalid auth token' });
      else callback(null, { succeeded: true, header: { revision: '9007199254740993', cluster_id: '18446744073709551615' } });
    } }
  });
  const client = new EtcdClient({ endpoints: [server.endpoint], namespace: 'rpc-auth', credentials: { user: 'test-user', password: 'test-password' }, operationTimeoutMs: 1500 });
  try {
    const operation = client.operation();
    const result = await client.rpc('KV', 'txn', {}, operation);
    assert.equal(result.header.revision, '9007199254740993');
    assert.equal(result.header.cluster_id, '18446744073709551615');
    assert.equal(authCalls, 2);
    assert.equal(writes, 2);
    for (const deadline of deadlines) assert.ok(Math.abs(deadline - operation.deadline.getTime()) < 30, `deadline changed: ${deadline}`);
    rejectAgain = true;
    await assert.rejects(client.rpc('KV', 'txn', {}, client.operation()), (error) => error.kind === 'auth');
    assert.equal(authCalls, 3);
    assert.equal(writes, 4, 'the second authentication rejection ends the operation');
  } finally { client.close(); server.close(); }
});

test('an unavailable write is not replayed and the next RPC uses the next endpoint', async () => {
  const { EtcdClient } = require('../../packages/framework-locations-etcd/dist/internal/client');
  let firstCalls = 0;
  let secondCalls = 0;
  const first = await fakeServer({ KV: { txn(_call, callback) { firstCalls++; callback({ code: 14, details: 'connection unavailable after write' }); } } });
  const second = await fakeServer({ KV: { txn(_call, callback) { secondCalls++; callback(null, { succeeded: true }); } } });
  const client = new EtcdClient({ endpoints: [first.endpoint, second.endpoint], namespace: 'rpc-rotation' });
  try {
    const operation = client.operation();
    await assert.rejects(client.rpc('KV', 'txn', {}, operation), (error) => error.kind === 'storeFailure' && error.code === 14);
    assert.equal(firstCalls, 1);
    assert.equal(secondCalls, 0);
    assert.equal((await client.rpc('KV', 'txn', {}, operation)).succeeded, true);
    assert.equal(firstCalls, 1);
    assert.equal(secondCalls, 1);
  } finally { client.close(); first.close(); second.close(); }
});

test('non-authentication permission errors never trigger authentication or replay', async () => {
  const { EtcdClient } = require('../../packages/framework-locations-etcd/dist/internal/client');
  let authCalls = 0;
  let writes = 0;
  const server = await fakeServer({
    Auth: { authenticate(_call, callback) { authCalls++; callback(null, { token: 'initial-token' }); } },
    KV: { txn(_call, callback) { writes++; callback({ code: 7, details: 'etcdserver: permission denied' }); } }
  });
  const client = new EtcdClient({ endpoints: [server.endpoint], namespace: 'rpc-permission', credentials: { user: 'test-user', password: 'test-password' } });
  try {
    await assert.rejects(client.rpc('KV', 'txn', {}, client.operation()), (error) => error.kind === 'storeFailure' && error.code === 7);
    assert.equal(authCalls, 1);
    assert.equal(writes, 1);
  } finally { client.close(); server.close(); }
});

test('cleanup traverses more than a thousand expired records with conditional pair deletion', async () => {
  const { EtcdAdminStore } = require('../../packages/framework-locations-etcd/dist/internal/admin-engine');
  const { encodeMetadata, encodeClock, keys } = require('../../packages/framework-locations-etcd/dist/internal/encoding');
  const key = keys('cleanup-pages');
  const records = Array.from({ length: 1005 }, (_, index) => ({
    key: key.metadata(Buffer.from(String(index).padStart(4, '0'))),
    value: encodeMetadata({ hasExpiry: true, expiresAtMs: 1n, payloadLength: 1 }),
    mod_revision: String(index + 2)
  }));
  const deleted = new Set();
  const pages = [];
  const client = {
    options: { namespace: 'cleanup-pages' },
    operation: () => ({ deadline: new Date(Date.now() + 5000) }),
    async rpc(service, method, request) {
      assert.equal(service, 'KV');
      if (method === 'range' && request.key.equals(key.clock)) return { kvs: [{ value: encodeClock(1n), mod_revision: '1' }] };
      if (method === 'range') {
        assert.equal(request.limit, '1000');
        const remaining = records.filter((record) => Buffer.compare(record.key, request.key) >= 0);
        pages.push(remaining.slice(0, 1000).length);
        return { kvs: remaining.slice(0, 1000), more: remaining.length > 1000 };
      }
      assert.equal(method, 'txn');
      const condition = request.compare[0];
      assert.equal(condition.target, 'MOD');
      assert.equal(condition.result, 'EQUAL');
      const original = records.find((record) => record.key.equals(condition.key));
      assert.equal(condition.mod_revision, original.mod_revision);
      assert.equal(request.success.length, 2);
      assert.ok(request.success[0].request_delete_range.key.equals(original.key));
      const frameworkKey = original.key.subarray(key.metadataPrefix.length);
      assert.ok(request.success[1].request_delete_range.key.equals(key.payload(frameworkKey)));
      assert.equal(deleted.has(original.key.toString('hex')), false);
      deleted.add(original.key.toString('hex'));
      return { succeeded: true };
    }
  };
  await new EtcdAdminStore(client).cleanup();
  assert.deepEqual(pages, [1000, 5]);
  assert.equal(deleted.size, 1005);
});

test('cleanup gives each delayed page and deletion its own operation deadline', { timeout: 180000 }, async () => {
  const { EtcdClient } = require('../../packages/framework-locations-etcd/dist/internal/client');
  const { EtcdAdminStore } = require('../../packages/framework-locations-etcd/dist/internal/admin-engine');
  const { encodeMetadata, encodeClock, keys } = require('../../packages/framework-locations-etcd/dist/internal/encoding');
  const key = keys('cleanup-deadlines');
  const records = Array.from({ length: 1005 }, (_, index) => ({
    key: key.metadata(Buffer.from(String(index).padStart(4, '0'))),
    value: encodeMetadata({ hasExpiry: true, expiresAtMs: 1n, payloadLength: 1 }), mod_revision: String(index + 2)
  }));
  let deletes = 0;
  const pageSizes = [];
  const server = await fakeServer({ KV: {
    range(call, callback) {
      if (call.request.key.equals(key.clock)) { callback(null, { kvs: [{ value: encodeClock(1n), mod_revision: '1' }] }); return; }
      const remaining = records.filter(record => Buffer.compare(record.key, call.request.key) >= 0);
      const kvs = remaining.slice(0, 1000);
      pageSizes.push(kvs.length);
      setTimeout(() => callback(null, { kvs, more: remaining.length > 1000 }), 100);
    },
    txn(_call, callback) { deletes++; setTimeout(() => callback(null, { succeeded: true }), 100); }
  } });
  const client = new EtcdClient({ endpoints: [server.endpoint], namespace: 'cleanup-deadlines', operationTimeoutMs: 200 });
  const stop = new AbortController();
  const operation = client.operation.bind(client);
  let operations = 0;
  client.operation = signal => { assert.equal(signal, stop.signal); operations++; return operation(signal); };
  try {
    await new EtcdAdminStore(client).cleanup(stop.signal);
    assert.deepEqual(pageSizes, [1000, 5]);
    assert.equal(deletes, 1005);
    assert.equal(operations, 1008);
  } finally { client.close(); server.close(); }
});

test('cleanup skips corrupt metadata in the middle of a page and retains its pair', async () => {
  const { EtcdClient } = require('../../packages/framework-locations-etcd/dist/internal/client');
  const { EtcdAdminStore } = require('../../packages/framework-locations-etcd/dist/internal/admin-engine');
  const { encodeMetadata, encodeClock, keys } = require('../../packages/framework-locations-etcd/dist/internal/encoding');
  const key = keys('cleanup-corrupt');
  const metadata = encodeMetadata({ hasExpiry: true, expiresAtMs: 1n, payloadLength: 1 });
  const records = ['before', 'corrupt', 'later'].map((name, index) => ({
    key: key.metadata(Buffer.from(name)), value: index === 1 ? Buffer.from('secret corrupt value') : metadata, mod_revision: String(index + 2)
  }));
  const present = new Set(records.flatMap(record => [record.key.toString('hex'), key.payload(record.key.subarray(key.metadataPrefix.length)).toString('hex')]));
  const server = await fakeServer({ KV: {
    range(call, callback) { callback(null, { kvs: call.request.key.equals(key.clock) ? [{ value: encodeClock(1n), mod_revision: '1' }] : records }); },
    txn(call, callback) {
      for (const entry of call.request.success) present.delete(entry.request_delete_range.key.toString('hex'));
      callback(null, { succeeded: true });
    }
  } });
  const client = new EtcdClient({ endpoints: [server.endpoint], namespace: 'cleanup-corrupt' });
  const original = process.stderr.write;
  let stderr = '';
  process.stderr.write = chunk => { stderr += chunk; return true; };
  try {
    await new EtcdAdminStore(client).cleanup();
    assert.deepEqual([...present].sort(), [records[1].key.toString('hex'), key.payload(Buffer.from('corrupt')).toString('hex')].sort());
    assert.equal(stderr, 'corrupt metadata skipped\n');
  } finally { process.stderr.write = original; client.close(); server.close(); }
});

test('a deadline-exceeded write is sent once and the next operation uses the next endpoint', async () => {
  const { EtcdClient } = require('../../packages/framework-locations-etcd/dist/internal/client');
  let firstCalls = 0;
  let secondCalls = 0;
  const first = await fakeServer({ KV: { txn() { firstCalls++; } } });
  const second = await fakeServer({ KV: { txn(_call, callback) { secondCalls++; callback(null, { succeeded: true }); } } });
  const client = new EtcdClient({ endpoints: [first.endpoint, second.endpoint], namespace: 'rpc-deadline', operationTimeoutMs: 250 });
  try {
    await assert.rejects(client.rpc('KV', 'txn', {}, client.operation()), error => error.kind === 'storeFailure' && error.code === 4);
    assert.equal(firstCalls, 1);
    assert.equal(secondCalls, 0);
    assert.equal((await client.rpc('KV', 'txn', {}, client.operation())).succeeded, true);
    assert.equal(firstCalls, 1);
    assert.equal(secondCalls, 1);
  } finally { client.close(); first.close(); second.close(); }
});

test('etcd admin rejects malformed arguments and options before connecting', async () => {
  const directory = fs.mkdtempSync(path.join(packageDirectory, '.admin-config-test-'));
  const valid = { endpoints: ['http://127.0.0.1:1'], namespace: 'config-test', operationTimeoutMs: 100 };
  try {
    fs.writeFileSync(path.join(directory, 'bad-cert.pem'), 'not-a-certificate');
    fs.writeFileSync(path.join(directory, 'bad-key.pem'), 'not-a-private-key');
    const invalid = [
      { ...valid, endpoints: [] },
      { ...valid, endpoints: ['http://127.0.0.1:1', 'https://127.0.0.1:2'] },
      { ...valid, tls: {} },
      { ...valid, namespace: '' },
      { ...valid, namespace: 'a\0b' },
      { ...valid, namespace: '한'.repeat(22) },
      { ...valid, operationTimeoutMs: 0 },
      { ...valid, operationTimeoutMs: null },
      { ...valid, endpoints: ['https://127.0.0.1:1'], tls: [] },
      { ...valid, endpoints: ['https://127.0.0.1:1'], tls: { clientIdentity: { certificatePemFile: 'bad-cert.pem', keyPemFile: 'bad-key.pem' } } },
      { ...valid, credentials: { user: '', passwordFile: 'missing' } },
      { ...valid, tls: { trustedCaPemFile: 'missing' } }
    ];
    for (const [index, value] of invalid.entries()) {
      const file = writeConfiguration(directory, `invalid-${index}`, value);
      const result = await command([admin, 'init', '--config', file], `etcd-invalid-${index}`);
      assert.equal(result.code, 1, `invalid options ${index}: ${result.stderr}`);
    }
    const file = writeConfiguration(directory, 'valid', valid);
    for (const [index, args] of [[], ['other', '--config', file], ['init'], ['init', '--config', file, '--unexpected']].entries()) {
      const result = await command([admin, ...args], `etcd-invalid-command-${index}`);
      assert.equal(result.code, 1, `invalid arguments ${index}: ${result.stderr}`);
    }
  } finally { fs.rmSync(directory, { recursive: true, force: true }); }
});

test('etcd admin initialization, three clocks, cleanup and unavailable cluster', {
  skip: process.env.ZLINK_ETCD_INTEGRATION !== '1', timeout: 240000
}, async (t) => {
  require('../../packages/framework-locations-etcd/dist/internal/client');
  const directory = fs.mkdtempSync(path.join(packageDirectory, '.admin-cluster-test-'));
  const runDirectory = path.join(directory, 'cluster');
  const harness = path.join(repository, 'scripts/etcd/etcd-cluster.mjs');
  const runners = [];
  let cluster;
  let stopped = false;
  const b64 = (value) => Buffer.from(value).toString('base64');
  const clockKey = (namespace) => Buffer.from(`${namespace}\0c`);
  async function gateway(method, body) {
    const response = await fetch(`${cluster.members[0].endpoint}/v3/kv/${method}`, {
      method: 'POST', body: JSON.stringify(body), headers: { 'content-type': 'application/json' }, signal: AbortSignal.timeout(5000)
    });
    const result = await response.json();
    assert.equal(response.ok && !result.error, true, JSON.stringify(result));
    return result;
  }
  async function readClock(namespace) {
    const result = await gateway('range', { key: b64(clockKey(namespace)) });
    const record = result.kvs?.[0];
    return record && { value: Buffer.from(record.value, 'base64').readBigUInt64BE(), revision: BigInt(record.mod_revision), version: BigInt(record.version) };
  }
  async function cli(commandName, file, label) {
    return command([admin, commandName, '--config', file], `etcd-${label}`);
  }
  function startRunner(file, label) {
    // Windows terminates a process on child.kill('SIGTERM'); dispatch the registered handler through IPC there.
    const bootstrap = `process.on('message',()=>process.emit('SIGTERM'));require(${JSON.stringify(admin)}).main(['run','--config',${JSON.stringify(file)}]).then(code=>{process.exitCode=code;if(process.connected)process.disconnect();});`;
    const running = process.platform === 'win32'
      ? child(process.execPath, ['-e', bootstrap], { stdio: ['ignore', 'pipe', 'pipe', 'ipc'] })
      : child(process.execPath, [admin, 'run', '--config', file]);
    running.label = label;
    runners.push(running);
    return running;
  }
  async function stopRunner(running) {
    if (running.processHandle.exitCode === null && !running.processHandle.killed) {
      if (process.platform === 'win32') running.processHandle.send('stop');
      else running.processHandle.kill('SIGTERM');
    }
    const timer = setTimeout(() => running.processHandle.kill(), 10000);
    try {
      const result = await running.exited;
      fs.writeFileSync(path.join(logDirectory, `etcd-${running.label}.log`), `${result.stdout}\n${result.stderr}`);
      return result;
    } finally { clearTimeout(timer); }
  }
  try {
    const up = await command([harness, 'up', '--backend', 'docker', '--run-dir', runDirectory], 'etcd-cluster-up');
    assert.equal(up.code, 0, up.stderr);
    cluster = JSON.parse(fs.readFileSync(path.join(runDirectory, 'cluster.json'), 'utf8'));
    const base = { endpoints: cluster.members.map((member) => member.endpoint), operationTimeoutMs: 5000 };
    const namespace = 'node-admin-integration';
    const config = writeConfiguration(directory, 'admin', { ...base, namespace });

    await t.test('empty namespace initialization succeeds, existing namespace fails', async () => {
      assert.equal((await cli('init', config, 'init-empty')).code, 0);
      assert.equal((await cli('init', config, 'init-existing')).code, 2);
      const occupied = 'node-admin-occupied';
      await gateway('put', { key: b64(Buffer.from(`${occupied}\0p\0x`)), value: b64('x') });
      const file = writeConfiguration(directory, 'occupied', { ...base, namespace: occupied });
      assert.equal((await cli('init', file, 'init-occupied')).code, 2);
      assert.equal(await readClock(occupied), undefined);
    });

    await t.test('concurrent initialization has exactly one winner', async () => {
      const file = writeConfiguration(directory, 'concurrent', { ...base, namespace: 'node-admin-concurrent' });
      const results = await Promise.all([cli('init', file, 'init-concurrent-1'), cli('init', file, 'init-concurrent-2')]);
      assert.deepEqual(results.map((result) => result.code).sort(), [0, 2]);
    });

    await t.test('three processes write about once a second and stop cleanly', async () => {
      const initial = await readClock(namespace);
      const active = Array.from({ length: 3 }, (_, index) => startRunner(config, `run-${index}`));
      await delay(6500);
      const current = await readClock(namespace);
      const writes = current.version - initial.version;
      t.diagnostic(`three processes: ${writes} clock writes in 6.5 seconds, Store clock advance ${current.value - initial.value} ms`);
      assert.ok(writes >= 4n && writes <= 7n, `clock writes in 6.5 seconds: ${writes}`);
      assert.ok(current.value > initial.value);
      assert.ok(current.value - initial.value <= 7579n, `clock advance ${current.value - initial.value}`);
      for (const running of active) assert.equal((await stopRunner(running)).code, 0);
    });

    await t.test('cleanup deletes expired pairs and keeps live, durable and rewritten pairs', async () => {
      const { encodeMetadata } = require('../../packages/framework-locations-etcd/dist/internal/encoding');
      const now = (await readClock(namespace)).value;
      const metadata = (key) => Buffer.from(`${namespace}\0m\0${key}`);
      const payload = (key) => Buffer.from(`${namespace}\0p\0${key}`);
      async function putPair(key, expiresAtMs, hasExpiry) {
        return gateway('txn', { success: [
          { request_put: { key: b64(metadata(key)), value: b64(encodeMetadata({ expiresAtMs, hasExpiry, payloadLength: 1 })) } },
          { request_put: { key: b64(payload(key)), value: b64('x') } }
        ] });
      }
      await putPair('expired', now, true);
      await putPair('live', now + 60000n, true);
      await putPair('durable', 0n, false);
      await putPair('rewritten', now, true);
      const { EtcdClient } = require('../../packages/framework-locations-etcd/dist/internal/client');
      const { EtcdAdminStore } = require('../../packages/framework-locations-etcd/dist/internal/admin-engine');
      const client = new EtcdClient({ ...base, namespace });
      const rpc = client.rpc.bind(client);
      let rewritten = false;
      client.rpc = async (service, method, request, operation) => {
        if (service === 'KV' && method.toLowerCase() === 'txn' && request.compare?.some((entry) => Buffer.from(entry.key).equals(metadata('rewritten')))) {
          await putPair('rewritten', now + 60000n, true);
          rewritten = true;
        }
        return rpc(service, method, request, operation);
      };
      try { await new EtcdAdminStore(client).cleanup(); }
      finally { client.close(); }
      assert.equal(rewritten, true, 'rewrite occurs after cleanup read and before conditional deletion');
      for (const key of ['expired', 'live', 'durable', 'rewritten']) {
        for (const recordKey of [metadata(key), payload(key)]) {
          const found = await gateway('range', { key: b64(recordKey) });
          assert.equal(found.kvs?.length || 0, key === 'expired' ? 0 : 1, key);
        }
      }
      assert.equal((await cli('cleanup', config, 'cleanup')).code, 0);
    });

    await t.test('missing clock exits with code 3 and is never recreated', async () => {
      const running = startRunner(config, 'run-missing-clock');
      await delay(1200);
      await gateway('deleterange', { key: b64(clockKey(namespace)) });
      const timer = setTimeout(() => running.processHandle.kill(), 10000);
      let result;
      try { result = await running.exited; } finally { clearTimeout(timer); }
      assert.equal(result.code, 3, result.stderr);
      assert.equal(await readClock(namespace), undefined);
      assert.equal((await cli('cleanup', config, 'cleanup-missing-clock')).code, 3);
    });

    await t.test('unavailable cluster fails init and cleanup with code 4', async () => {
      const outageConfig = writeConfiguration(directory, 'outage-run', { ...base, namespace: 'node-admin-concurrent' });
      const running = startRunner(outageConfig, 'run-outage');
      await delay(300);
      const down = await command([harness, 'down', '--run-dir', runDirectory], 'etcd-cluster-down');
      assert.equal(down.code, 0, down.stderr);
      stopped = true;
      await delay(5500);
      assert.equal(running.processHandle.exitCode, null, 'run stays alive while every endpoint is unavailable');
      const stoppedRunner = await stopRunner(running);
      assert.equal(stoppedRunner.code, 0, stoppedRunner.stderr);
      assert.notEqual(stoppedRunner.stderr.trim(), '', 'run reports the outage');
      const file = writeConfiguration(directory, 'unavailable', { ...base, namespace, operationTimeoutMs: 1000 });
      assert.equal((await cli('init', file, 'init-unavailable')).code, 4);
      assert.equal((await cli('cleanup', file, 'cleanup-unavailable')).code, 4);
    });
  } finally {
    for (const running of runners) {
      if (running.processHandle.exitCode === null && !running.processHandle.killed) running.processHandle.kill();
      await running.exited;
    }
    if (!stopped && fs.existsSync(path.join(runDirectory, 'cluster.json'))) {
      const result = await command([harness, 'down', '--run-dir', runDirectory], 'etcd-cluster-finally-down');
      assert.equal(result.code, 0, result.stderr);
    }
    if (cluster) {
      for (const [resource, args] of [['containers', ['ps', '-aq']], ['networks', ['network', 'ls', '-q']]]) {
        const running = child('docker', [...args, '--filter', `label=dev.zlink.etcd.run=${cluster.runId}`]);
        const result = await running.exited;
        assert.equal(result.code, 0, result.stderr);
        assert.equal(result.stdout.trim(), '', `remaining ${resource}`);
      }
    }
    if (fs.existsSync(runDirectory)) fs.cpSync(runDirectory, path.join(logDirectory, 'etcd-cluster-artifacts'), { recursive: true });
    fs.rmSync(directory, { recursive: true, force: true });
  }
});
