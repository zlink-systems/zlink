#!/usr/bin/env node
import assert from 'node:assert/strict';
import { mkdir, copyFile, writeFile, rm, readFile, readdir } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { join, resolve, basename } from 'node:path';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { randomUUID } from 'node:crypto';
import { args, command, delay, integer, main, portLocks, readJson, rpc, writeJson } from './common.mjs';
import { LIMITS } from './constants.mjs';

const execute = promisify(execFile);
const directory = fileURLToPath(new URL('.', import.meta.url));
main(async () => {
  const options = args({ 'log-dir': 'string', backend: 'string', runs: 'string', 'isolation-only': 'boolean' });
  if (!options['log-dir']) throw new Error('--log-dir is required');
  const logDirectory = resolve(options['log-dir']); await mkdir(logDirectory, { recursive: true });
  const runRoot = join(directory, '.runs', `verify-${randomUUID()}`);
  const runs = Array.from({ length: integer(options.runs ?? 2, 'runs', 1, 4) }, (_, i) => join(runRoot, String(i + 1)));
  const backend = options.backend ?? 'docker';
  let step = 0;
  async function cli(script, tokens, name) {
    const path = join(logDirectory, `${String(++step).padStart(2, '0')}-${name}.log`);
    try {
      const result = await execute(process.execPath, [join(directory, script), ...tokens], { timeout: LIMITS.verificationMs, maxBuffer: LIMITS.commandOutputBytes });
      await writeFile(path, result.stdout + result.stderr); return result.stdout;
    } catch (error) { await writeFile(path, `${error.stdout ?? ''}${error.stderr ?? ''}\n${error.message}\n`); throw error; }
  }
  const cluster = (run, action, extra = []) => cli('etcd-cluster.mjs', [action, '--run-dir', run, ...extra], `${action}-${basename(run)}`);
  const kv = (endpoint, action, extra = []) => cli('raw-kv.mjs', [action, '--endpoint', endpoint, ...extra], `raw-${action}`);
  const manifests = [];
  let originalError;
  try {
    const startups = await Promise.allSettled(runs.map((run) => cluster(run, 'up', ['--backend', backend])));
    for (const result of startups) if (result.status === 'rejected') throw result.reason;
    for (const run of runs) manifests.push(await readJson(join(run, 'cluster.json')));
    for (const run of runs) await cluster(run, 'status');
    const [a] = manifests;
    for (const property of ['clusterId', 'token', 'runId']) assert.equal(new Set(manifests.map((state) => state[property])).size, manifests.length, property);
    for (const property of ['clientPort', 'dataDir', 'name', ...(backend === 'docker' ? ['containerId'] : ['pid', 'peerPort', 'peerBackendPort'])]) {
      const values = manifests.flatMap((state) => state.members.map((m) => m[property])); assert.equal(new Set(values).size, values.length, property);
    }
    const allPorts = manifests.flatMap((s) => backend === 'docker' ? s.members.map((m) => m.clientPort) : [s.peerControlPort, ...s.members.flatMap((m) => [m.clientPort, m.peerPort, m.peerBackendPort])]);
    assert.equal(new Set(allPorts).size, allPorts.length);
    if (backend === 'native') {
      for (const state of manifests) {
        for (const port of [state.peerControlPort, ...state.members.flatMap((m) => [m.clientPort, m.peerPort, m.peerBackendPort])])
          assert.equal(await readFile(new URL(String(port), portLocks), 'utf8'), state.runId, `port lock owner: ${port}`);
      }
    } else {
      for (const state of manifests) for (const member of state.members) {
        assert.equal(member.peerPort, undefined);
        assert.equal(member.peerBackendPort, undefined);
      }
    }
    await writeJson(join(logDirectory, 'isolation.json'), { runIds: manifests.map((s) => s.runId), clusterIds: manifests.map((s) => s.clusterId), allPorts, result: 'PASS' });
    console.log(`${backend}: concurrent runs=${runs.length}, overlapping ports/containers/data=0: PASS`);
    if (!options['isolation-only']) {
      const endpoint = a.members[1].endpoint;
      const put = JSON.parse(await kv(endpoint, 'put', ['--key-hex', '006162ff', '--value-hex', '00ff80']));
      const range = JSON.parse(await kv(endpoint, 'range', ['--key-hex', '006162ff', '--revision', put.header.revision]));
      assert.equal(range.bytes[0].valueHex, '00ff80');
      await kv(endpoint, 'put', ['--key-hex', '006162ff', '--value-hex', '02']);
      const historical = JSON.parse(await kv(endpoint, 'range', ['--key-hex', '006162ff', '--revision', put.header.revision]));
      assert.equal(historical.bytes[0].valueHex, '00ff80');
      const txnPath = join(runs[0], 'txn.json');
      await writeJson(txnPath, { compare: [], success: [{ request_put: { key: 'dHhu', value: 'bWFqb3JpdHk=' } }], failure: [] });
      async function majorityReady() {
        const deadline = Date.now() + LIMITS.readinessMs;
        let last;
        while (Date.now() < deadline) {
          try { await rpc(endpoint, '/v3/kv/range', { key: 'dHhu' }); return; }
          catch (error) { last = error; await delay(LIMITS.pollMs); }
        }
        throw last;
      }
      await cluster(runs[0], 'kill', ['--member', '1']); await majorityReady();
      assert.equal(JSON.parse(await kv(endpoint, 'txn', ['--body-file', txnPath])).succeeded, true);
      await cluster(runs[0], 'resume', ['--member', '1']);
      Object.assign(a, await readJson(join(runs[0], 'cluster.json')));
      await cluster(runs[0], 'pause', ['--member', '1']); await majorityReady();
      assert.equal(JSON.parse(await kv(endpoint, 'txn', ['--body-file', txnPath])).succeeded, true);
      await cluster(runs[0], 'resume', ['--member', '1']);
      await cluster(runs[0], 'partition', ['--member', '1']); await majorityReady();
      assert.equal(JSON.parse(await kv(endpoint, 'txn', ['--body-file', txnPath])).succeeded, true);
      // Maintenance status is local: it must remain reachable while peer traffic is blocked.
      assert.equal((await rpc(a.members[0].endpoint, '/v3/maintenance/status')).header.cluster_id, a.clusterId);
      await assert.rejects(rpc(a.members[0].endpoint, '/v3/kv/range', { key: 'dHhu' }), /timeout|abort|deadline|unavailable/i);
      await cluster(runs[0], 'heal');
      await cluster(runs[0], 'resume', ['--member', '1']);
      await cluster(runs[0], 'status');
      const revision = JSON.parse(await kv(endpoint, 'put', ['--key-hex', '636f6d70616374', '--value-hex', '01'])).header.revision;
      await cluster(runs[0], 'compact', ['--revision', revision]);
      await assert.rejects(kv(endpoint, 'range', ['--key-hex', '006162ff', '--revision', put.header.revision]), /Command failed/);
      await kv(endpoint, 'delete', ['--key-hex', '006162ff']);
      console.log('cluster lifecycle, concurrent run isolation, historical raw KV, majority Txn, peer-only partition and compaction: PASS');
    }
  } catch (error) { originalError = error; }
  const cleanupErrors = [];
  const auditedRunIds = [];
  for (const run of runs) {
    try {
      await cluster(run, 'down');
      const state = await readJson(join(run, 'cluster.json'));
      auditedRunIds.push(state.runId);
      for (const member of state.members) await copyFile(member.log, join(logDirectory, `${state.runId}-member-${member.index}.log`));
      await copyFile(join(run, 'cluster.json'), join(logDirectory, `${state.runId}-cluster.json`));
      if (backend === 'docker') {
        const remaining = await command('docker', ['ps', '-aq', '--filter', `label=${LIMITS.label}=${state.runId}`]);
        assert.equal(remaining, '');
        const networks = await command('docker', ['network', 'ls', '-q', '--filter', `label=${LIMITS.label}=${state.runId}`]);
        assert.equal(networks, '');
      } else {
        for (const name of await readdir(portLocks)) assert.notEqual(await readFile(new URL(name, portLocks), 'utf8'), state.runId, `remaining port lock: ${name}`);
        if (state.peerProxy) await copyFile(state.peerProxy.log, join(logDirectory, `${state.runId}-peer-proxy.log`));
        for (const record of [...state.members, state.peerProxy].filter(Boolean)) {
          let stat;
          try { stat = await readFile(`/proc/${record.pid}/stat`, 'utf8'); }
          catch (error) { if (error.code === 'ENOENT') continue; throw error; }
          const fields = stat.slice(stat.lastIndexOf(')') + 2).split(' ');
          assert.ok(fields[0] === 'Z' || fields[19] !== record.birth, `run process still alive: ${record.pid}`);
        }
      }
    } catch (error) { cleanupErrors.push(error); }
  }
  // Only this invocation's UUID directory is removed, after preserving logs and successful cleanup.
  if (!cleanupErrors.length) {
    const expectedParent = resolve(directory, '.runs');
    assert.equal(resolve(runRoot, '..'), expectedParent);
    await rm(runRoot, { recursive: true });
  }
  await writeJson(join(logDirectory, 'cleanup.json'), { result: cleanupErrors.length ? 'FAIL' : 'PASS',
    auditedRunIds, backend, remainingRunResources: cleanupErrors.length ? null : 0,
    errors: cleanupErrors.map((e) => e.message) });
  if (originalError || cleanupErrors.length) throw new AggregateError([originalError, ...cleanupErrors].filter(Boolean), 'harness verification failed; see step logs');
  console.log('cleanup: PASS (run containers, networks and processes removed; member logs retained)');
});
