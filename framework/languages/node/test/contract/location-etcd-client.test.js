const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const grpc = require('@grpc/grpc-js');
const { EtcdClient, protoDefinition } = require('../../packages/framework-locations-etcd/dist/internal/client');
const { validateOptions } = require('../../packages/framework-locations-etcd/dist/internal/options');
const definitions = grpc.loadPackageDefinition(protoDefinition()).etcdserverpb;
const certificate = fs.readFileSync(path.join(__dirname, 'location-etcd-fixtures/localhost-cert.pem'));
const privateKey = fs.readFileSync(path.join(__dirname, 'location-etcd-fixtures/localhost-key.pem'));

async function serve(server, credentials = grpc.ServerCredentials.createInsecure()) {
  return new Promise((resolve, reject) => server.bindAsync('127.0.0.1:0', credentials,
    (error, port) => error ? reject(error) : resolve(port)));
}

test('etcd TLS uses normalized HTTPS URLs and supports trusted CA plus client identity', async () => {
  const server = new grpc.Server();
  server.addService(definitions.KV.service, { range(_call, callback) { callback(null, { header: { cluster_id: '18446744073709551615' } }); } });
  const port = await serve(server, grpc.ServerCredentials.createSsl(certificate, [{ private_key: privateKey, cert_chain: certificate }], true));
  const client = new EtcdClient({
    endpoints: [`HTTPS://localhost:${port}`], namespace: 'tls',
    tls: { trustedCaPem: certificate.toString(), clientIdentity: { certificatePem: certificate.toString(), keyPem: privateKey.toString() } }
  });
  try {
    assert.equal((await client.rpc('KV', 'range', {}, client.operation())).header.cluster_id, '18446744073709551615');
  } finally { client.close(); server.forceShutdown(); }
});

test('etcd CLI retains a termination signal received while loading its dependencies', { timeout: 10000 }, async () => {
  const { spawn } = require('node:child_process');
  const directory = fs.mkdtempSync(path.resolve(__dirname, '../../packages/framework-locations-etcd/.startup-test-'));
  const server = new grpc.Server();
  let reads = 0;
  server.addService(definitions.KV.service, { range(_call, callback) { reads++; callback(null, { kvs: [] }); } });
  let running;
  try {
    const port = await serve(server);
    const preloader = path.join(directory, 'signal.cjs');
    fs.writeFileSync(preloader, `const Module=require('node:module');const load=Module._load;Module._load=function(name,...args){if(name==='@grpc/grpc-js'){Module._load=load;process.emit('SIGTERM');}return load.call(this,name,...args);};`);
    const config = path.join(directory, 'config.json');
    fs.writeFileSync(config, JSON.stringify({ endpoints: [`http://127.0.0.1:${port}`], namespace: 'startup-stop', operationTimeoutMs: 1000 }));
    running = spawn(process.execPath, ['--require', preloader, path.resolve(__dirname, '../../packages/framework-locations-etcd/dist/admin.js'), 'run', '--config', config], { windowsHide: true });
    let stderr = '';
    running.stderr.on('data', bytes => { stderr += bytes; });
    const code = await new Promise((resolve, reject) => { running.on('error', reject); running.on('exit', resolve); });
    assert.equal(code, 0, stderr);
    assert.equal(reads, 0, 'startup termination must prevent the first RPC');
  } finally {
    running?.kill(); server.forceShutdown(); fs.rmSync(directory, { recursive: true, force: true });
  }
});

test('etcd option validation rejects null timeout and malformed client PEM before RPC', () => {
  const valid = { endpoints: ['https://localhost:2379'], namespace: 'options' };
  assert.throws(() => validateOptions({ ...valid, operationTimeoutMs: null }), RangeError);
  assert.throws(() => validateOptions({ ...valid, tls: [] }), TypeError);
  assert.throws(() => validateOptions({ ...valid, tls: { clientIdentity: { certificatePem: 'bad-cert', keyPem: 'bad-key' } } }), TypeError);
});

test('etcd HTTPS without an explicit CA uses the operating system trust store', async () => {
  const tls = require('node:tls');
  const original = tls.getCACertificates;
  let reads = 0;
  tls.getCACertificates = (type) => { assert.equal(type, 'system'); reads++; return [certificate.toString()]; };
  const server = new grpc.Server();
  let client;
  try {
    server.addService(definitions.KV.service, { range(_call, callback) { callback(null, {}); } });
    const port = await serve(server, grpc.ServerCredentials.createSsl(null, [{ private_key: privateKey, cert_chain: certificate }]));
    client = new EtcdClient({ endpoints: [`https://localhost:${port}`], namespace: 'system-ca' });
    await client.rpc('KV', 'range', {}, client.operation());
    assert.equal(reads, 1);
  } finally { client?.close(); server.forceShutdown(); tls.getCACertificates = original; }
});

for (const stage of ['initial authentication', 'authentication rejection']) {
  test(`etcd stop during ${stage} waits for the sent response and starts no follow-up RPC`, { timeout: 5000 }, async () => {
    let authCalls = 0;
    let writes = 0;
    let release;
    let entered;
    const waiting = new Promise(resolve => { entered = resolve; });
    const server = new grpc.Server();
    server.addService(definitions.Auth.service, { authenticate(_call, callback) {
      authCalls++;
      if (stage === 'initial authentication') { release = () => callback(null, { token: 'token' }); entered(); }
      else callback(null, { token: 'token' });
    } });
    server.addService(definitions.KV.service, { txn(_call, callback) {
      writes++;
      release = () => callback({ code: grpc.status.UNAUTHENTICATED, details: 'invalid auth token' });
      entered();
    } });
    const port = await serve(server);
    const client = new EtcdClient({ endpoints: [`http://127.0.0.1:${port}`], namespace: 'stop', credentials: { user: 'test', password: 'test' } });
    const stop = new AbortController();
    try {
      const response = client.rpc('KV', 'txn', {}, client.operation(stop.signal));
      await waiting;
      stop.abort();
      release();
      await assert.rejects(response, /Store operation stopped/);
      assert.equal(authCalls, 1);
      assert.equal(writes, stage === 'initial authentication' ? 0 : 1);
    } finally { client.close(); server.forceShutdown(); }
  });
}
