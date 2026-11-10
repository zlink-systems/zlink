#!/usr/bin/env node
import { mkdir, open, readFile, rm, writeFile, access, rename } from 'node:fs/promises';
import { createHash, randomUUID } from 'node:crypto';
import { resolve, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { args, background, command, commandToLog, delay, integer, main, readJson, reservePorts, releasePorts, rpc, writeJson, revision } from './common.mjs';
import { LIMITS, VERSION } from './constants.mjs';

const directory = fileURLToPath(new URL('.', import.meta.url));
const binaryCache = resolve(directory, '.cache');
const configFile = (run) => join(run, 'cluster.json');
const b64 = (text) => Buffer.from(text).toString('base64');
async function nativeBinary(run) {
  const archive = VERSION.archives[process.arch];
  if (process.platform !== 'linux' || !archive) throw new Error('native backend requires Linux x64/arm64; use --backend docker');
  if (binaryCache === run || binaryCache.startsWith(`${run}/`)) throw new Error('binary cache must be outside run directory');
  await mkdir(binaryCache, { recursive: true });
  const name = `etcd-v${VERSION.version}-${archive.platform}`;
  const archivePath = join(binaryCache, `${name}.tar.gz`);
  let bytes;
  let cacheMissing = false;
  try { bytes = await readFile(archivePath); }
  catch (error) {
    if (error.code !== 'ENOENT') throw error;
    cacheMissing = true;
    const url = `https://github.com/etcd-io/etcd/releases/download/v${VERSION.version}/${name}.tar.gz`;
    const response = await fetch(url, { signal: AbortSignal.timeout(LIMITS.commandMs) });
    if (!response.ok) throw new Error(`download ${url}: HTTP ${response.status}`);
    bytes = Buffer.from(await response.arrayBuffer());
  }
  if (createHash('sha256').update(bytes).digest('hex') !== archive.sha256) throw new Error(`SHA-256 mismatch: ${archivePath}`);
  // Extract a verified archive into a private cache directory; never trust a previously extracted executable.
  const extraction = join(binaryCache, randomUUID());
  await mkdir(extraction);
  const temporary = `${archivePath}.${randomUUID()}.tmp`;
  try {
    await writeFile(temporary, bytes);
    await command('tar', ['-xzf', temporary, '-C', extraction, `${name}/etcd`]);
    if (cacheMissing) await rename(temporary, archivePath);
    else await rm(temporary);
  } catch (error) {
    await rm(temporary, { force: true }); await rm(extraction, { recursive: true }); throw error;
  }
  return { executable: join(extraction, name, 'etcd'), extraction };
}
async function birth(pid) {
  const stat = await readFile(`/proc/${pid}/stat`, 'utf8');
  return stat.slice(stat.lastIndexOf(')') + 2).split(' ')[19];
}
async function signalOwned(record, signal) {
  if (!record.pid) return;
  try {
    if (await birth(record.pid) !== record.birth) throw new Error(`PID ${record.pid} was reused; refusing ${signal}`);
    process.kill(record.pid, signal);
  } catch (error) { if (error.code !== 'ENOENT' && error.code !== 'ESRCH') throw error; }
}
async function startNative(record, executable, argv) {
  record.pid = await background(executable, argv, record.log);
  record.birth = await birth(record.pid);
}
function memberArgs(state, member) {
  const docker = state.backend === 'docker';
  return [
    `--name=${member.name}`, `--data-dir=${docker ? '/data' : member.dataDir}`,
    `--listen-client-urls=http://${docker ? '0.0.0.0' : '127.0.0.1'}:${docker ? LIMITS.containerClientPort : member.clientPort}`,
    `--advertise-client-urls=${member.endpoint}`,
    `--listen-peer-urls=http://${docker ? '0.0.0.0' : '127.0.0.1'}:${docker ? LIMITS.containerPeerPort : member.peerBackendPort}`,
    `--initial-advertise-peer-urls=${member.peerEndpoint}`,
    `--initial-cluster=${state.members.map((m) => `${m.name}=${m.peerEndpoint}`).join(',')}`,
    `--initial-cluster-token=${state.token}`, '--initial-cluster-state=new',
    `--max-txn-ops=${LIMITS.maxTxnOps}`, `--max-request-bytes=${LIMITS.maxRequestBytes}`,
    `--auto-compaction-mode=${LIMITS.compactionMode}`, `--auto-compaction-retention=${LIMITS.compactionRetention}`,
    ...(state.quotaBytes ? [`--quota-backend-bytes=${state.quotaBytes}`] : [])
  ];
}
async function dockerOwned(state, member) {
  const data = JSON.parse(await command('docker', ['inspect', member.containerId]))[0];
  if (data.Config.Labels?.[LIMITS.label] !== state.runId) throw new Error('container ownership mismatch');
  return data;
}
async function dockerEndpoint(member) {
  const published = await command('docker', ['port', member.containerId, `${LIMITS.containerClientPort}/tcp`]);
  if (!/^127\.0\.0\.1:[0-9]+$/.test(published)) throw new Error(`unexpected Docker port: ${published}`);
  member.clientPort = integer(published.split(':')[1], 'Docker client port', 1, LIMITS.maximumPort);
  member.endpoint = `http://${published}`;
}
export async function ready(state) {
  const deadline = Date.now() + LIMITS.readinessMs;
  let failure;
  while (Date.now() < deadline) {
    if (state.backend === 'native') {
      for (const record of [...state.members, state.peerProxy].filter(Boolean)) {
        const log = await readFile(record.log, 'utf8');
        const bindFailure = log.split('\n').find((line) => /address already in use|EADDRINUSE/i.test(line));
        if (bindFailure) throw new Error(`port in use; bind failed: ${bindFailure}`);
      }
    }
    try {
      const reports = await Promise.all(state.members.map((m) => rpc(m.endpoint, '/v3/maintenance/status')));
      const ids = reports.map((r) => r.header.cluster_id);
      if (!ids[0] || ids.some((id) => id !== ids[0]) || (state.clusterId && state.clusterId !== ids[0])) throw new Error('cluster ID mismatch');
      for (let i = 0; i < state.members.length; i++) {
        const member = state.members[i];
        const key = b64(`\0zlink-harness-ready/${state.runId}/${member.index}`);
        const read = await rpc(member.endpoint, '/v3/kv/range', { key, serializable: false });
        const txn = await rpc(member.endpoint, '/v3/kv/txn', { compare: [], success: [
          { request_put: { key, value: b64(state.runId) } }
        ], failure: [] });
        if (!txn.succeeded || read.header.cluster_id !== ids[0] || txn.header.cluster_id !== ids[0]) throw new Error('linearizable read/Txn readiness failed');
        await rpc(member.endpoint, '/v3/kv/deleterange', { key });
        member.memberId = BigInt(reports[i].header.member_id).toString(16);
      }
      state.clusterId = ids[0];
      return reports;
    } catch (error) { failure = error; await delay(LIMITS.pollMs); }
  }
  throw new Error(`cluster readiness failed: ${failure?.message}`, { cause: failure });
}
async function down(state, run) {
  const failures = [];
  const attempt = async (fn) => { try { await fn(); } catch (error) { failures.push(error.message); } };
  if (state.backend === 'docker') {
    // Labels recover even a container created immediately before a manifest write failed.
    const ids = (await command('docker', ['ps', '-aq', '--filter', `label=${LIMITS.label}=${state.runId}`])).split(/\s+/).filter(Boolean);
    for (const id of ids) {
      const member = state.members.find((m) => m.containerId?.startsWith(id));
      if (member) await attempt(() => commandToLog('docker', ['logs', '--timestamps', id], member.log));
      await attempt(() => command('docker', ['rm', '-f', id]));
    }
    for (const network of state.networks ?? []) await attempt(async () => {
      const existing = (await command('docker', ['network', 'ls', '-q', '--filter', `label=${LIMITS.label}=${state.runId}`, '--filter', `name=^${network}$`])).trim();
      if (existing) await command('docker', ['network', 'rm', existing]);
    });
  } else {
    for (const record of [...state.members, state.peerProxy].filter(Boolean)) await attempt(async () => {
      await signalOwned(record, 'SIGCONT'); await signalOwned(record, 'SIGTERM');
      const deadline = Date.now() + LIMITS.requestMs;
      while (Date.now() < deadline) {
        try { const text = await readFile(`/proc/${record.pid}/stat`, 'utf8'); if (text.includes(') Z ')) break; }
        catch (error) { if (error.code === 'ENOENT') return; throw error; }
        await delay(LIMITS.pollMs);
      }
      await signalOwned(record, 'SIGKILL');
    });
    if (state.extraction) await attempt(async () => {
      if (resolve(state.extraction, '..') !== binaryCache) throw new Error('extraction is outside binary cache');
      await rm(state.extraction, { recursive: true, force: true });
    });
  }
  if (state.backend === 'native' && !failures.length) await attempt(() => releasePorts(state.runId));
  state.phase = failures.length ? 'cleanup-failed' : 'down';
  await writeJson(configFile(run), state);
  if (failures.length) throw new Error(`cleanup failed: ${failures.join('; ')}`);
}
async function up(options, run) {
  try { await access(configFile(run)); throw new Error('run directory already has cluster.json; use a fresh run directory'); }
  catch (error) { if (error.code !== 'ENOENT') throw error; }
  const count = integer(options.members ?? LIMITS.members, 'members', 3, 5);
  if (count !== 3 && count !== 5) throw new Error('members must be 3 or 5');
  const backend = options.backend ?? (process.platform === 'linux' ? 'native' : 'docker');
  if (!['native', 'docker'].includes(backend)) throw new Error('backend must be native or docker');
  const runId = randomUUID();
  const state = { schemaVersion: LIMITS.stateVersion, runId, backend, version: VERSION.version,
    image: VERSION.image, token: `zlink-${runId}`, clusterId: null, phase: 'starting', members: [], networks: [] };
  if (options['quota-bytes']) state.quotaBytes = integer(options['quota-bytes'], 'quota-bytes');
  await writeJson(configFile(run), state);
  try {
    let peerBase;
    if (backend === 'docker') {
      await command('docker', ['pull', VERSION.image]);
      state.networks = [`zl-etcd-${runId}-client`, `zl-etcd-${runId}-peer`];
      await writeJson(configFile(run), state);
      for (const network of state.networks) await command('docker', ['network', 'create', '--label', `${LIMITS.label}=${runId}`, network]);
      const network = JSON.parse(await command('docker', ['network', 'inspect', state.networks[1]]))[0];
      const subnet = network.IPAM.Config[0].Subnet;
      // Newer engines (the hosted ubuntu-24.04 image) accept `connect --ip` only on networks
      // created with --subnet, so recreate
      // the peer network with the subnet Docker just chose for it.
      await command('docker', ['network', 'rm', state.networks[1]]);
      await command('docker', ['network', 'create', '--label', `${LIMITS.label}=${runId}`, '--subnet', subnet, state.networks[1]]);
      const [base, prefix] = subnet.split('/');
      if (Number(prefix) > 28 || base.includes(':')) throw new Error('peer network requires an IPv4 subnet with room for members');
      peerBase = base.split('.').reduce((n, octet) => n * 256 + Number(octet), 0);
    } else { Object.assign(state, await nativeBinary(run)); }
    const ports = backend === 'native' ? await reservePorts(count * 3 + 1, runId) : [];
    if (backend === 'native') state.peerControlPort = ports[count * 3];
    for (let i = 0; i < count; i++) {
      const index = i + 1;
      const member = { index, name: `zl-etcd-${runId}-${index}`, dataDir: join(run, `member-${index}`, 'data'),
        log: join(run, `member-${index}`, 'member.log'),
        ...(backend === 'native' ? { clientPort: ports[i * 3], peerPort: ports[i * 3 + 1], peerBackendPort: ports[i * 3 + 2] } : {}) };
      member.endpoint = backend === 'docker' ? `http://${member.name}:${LIMITS.containerClientPort}` : `http://127.0.0.1:${member.clientPort}`;
      if (backend === 'docker') {
        const value = peerBase + index + 1;
        member.peerIp = [24, 16, 8, 0].map((shift) => (value >>> shift) & 255).join('.');
        member.peerEndpoint = `http://${member.peerIp}:${LIMITS.containerPeerPort}`;
      } else member.peerEndpoint = `http://127.0.0.1:${member.peerPort}`;
      await mkdir(member.dataDir, { recursive: true });
      state.members.push(member);
    }
    state.endpoints = state.members.map((m) => m.endpoint);
    await writeJson(configFile(run), state);
    if (backend === 'native') {
      state.peerProxy = { log: join(run, 'peer-proxy.log') };
      await startNative(state.peerProxy, process.execPath, [join(directory, 'native-peer-proxy.mjs'), '--config', configFile(run)]);
      await writeJson(configFile(run), state);
    }
    for (const member of state.members) {
      if (backend === 'docker') {
        member.containerId = await command('docker', ['create', '--name', member.name, '--label', `${LIMITS.label}=${runId}`,
          '--network', state.networks[0], '-p', `127.0.0.1::${LIMITS.containerClientPort}`,
          '--mount', `type=bind,source=${member.dataDir},target=/data`, '--entrypoint', '/usr/local/bin/etcd', VERSION.image, ...memberArgs(state, member)]);
        await writeJson(configFile(run), state);
        await command('docker', ['network', 'connect', '--ip', member.peerIp, state.networks[1], member.containerId]);
        await command('docker', ['start', member.containerId]);
        await dockerEndpoint(member);
      } else await startNative(member, state.executable, memberArgs(state, member));
      await writeJson(configFile(run), state);
    }
    state.endpoints = state.members.map((m) => m.endpoint);
    await ready(state);
    if (backend === 'native') {
      await writeJson(configFile(run), state);
      await rpc(`http://127.0.0.1:${state.peerControlPort}`, '/configure');
    }
    state.phase = 'ready';
    await writeJson(configFile(run), state);
    return state;
  } catch (error) {
    try { await down(state, run); } catch (cleanup) { throw new AggregateError([error, cleanup], 'up failed and cleanup failed'); }
    throw error;
  }
}
main(async () => {
  const options = args({ 'run-dir': 'string', members: 'string', 'quota-bytes': 'string', member: 'string', revision: 'string', backend: 'string' });
  if (!options['run-dir']) throw new Error('--run-dir is required');
  const run = resolve(options['run-dir']);
  await mkdir(run, { recursive: true });
  const lockPath = join(run, '.cluster.lock');
  const lock = await open(lockPath, 'wx').catch((error) => { throw new Error(`run is locked: ${lockPath}: ${error.message}`); });
  try {
    if (options.command === 'up') { console.log(JSON.stringify(await up(options, run), null, 2)); return; }
    const state = await readJson(configFile(run));
    if (state.schemaVersion !== LIMITS.stateVersion) throw new Error('unknown cluster manifest version');
    if (options.command === 'down') { await down(state, run); console.log('cluster down; member logs retained'); return; }
    if (state.phase === 'down') throw new Error('cluster is down');
    if (options.command === 'status') {
      const reports = await Promise.all(state.members.map(async (m) => {
        try { return { member: m.index, endpoint: m.endpoint, status: await rpc(m.endpoint, '/v3/maintenance/status') }; }
        catch (error) { return { member: m.index, error: error.message }; }
      }));
      console.log(JSON.stringify({ runId: state.runId, clusterId: state.clusterId, members: reports }, null, 2));
      if (reports.some((m) => m.error || m.status.header.cluster_id !== state.clusterId)) process.exitCode = 1;
      return;
    }
    if (options.command === 'compact') {
      revision(options.revision);
      let result; let error;
      for (const endpoint of state.endpoints) { try { result = await rpc(endpoint, '/v3/kv/compaction', { revision: options.revision }); break; } catch (failure) { error = failure; } }
      if (!result) throw error;
      console.log(JSON.stringify(result)); return;
    }
    const member = options.member ? state.members.find((m) => m.index === integer(options.member, 'member')) : null;
    if (options.command !== 'heal' && !member) throw new Error('valid --member (1-based) required');
    const selected = options.command === 'heal' ? state.members.filter((m) => m.partitioned) : [member];
    if (['partition', 'heal'].includes(options.command)) {
      for (const target of selected) {
        if (state.backend === 'docker') {
          const container = await dockerOwned(state, target);
          const connected = Boolean(container.NetworkSettings.Networks[state.networks[1]]);
          if (options.command === 'partition' && connected) await command('docker', ['network', 'disconnect', state.networks[1], target.containerId]);
          if (options.command === 'heal' && !connected) await command('docker', ['network', 'connect', '--ip', target.peerIp, state.networks[1], target.containerId]);
        } else await rpc(`http://127.0.0.1:${state.peerControlPort}`, options.command === 'partition' ? `/partition?id=${target.memberId}` : '/heal');
        target.partitioned = options.command === 'partition';
        await writeJson(configFile(run), state);
      }
    } else if (['kill', 'pause', 'resume'].includes(options.command)) {
      if (state.backend === 'docker') {
        const container = await dockerOwned(state, member);
        if (options.command === 'kill') {
          if (container.State.Paused) await command('docker', ['unpause', member.containerId]);
          await command('docker', ['kill', member.containerId]);
        } else if (options.command === 'pause') await command('docker', ['pause', member.containerId]);
        else if (container.State.Paused) await command('docker', ['unpause', member.containerId]);
        else if (!container.State.Running) {
          await command('docker', ['start', member.containerId]);
          await dockerEndpoint(member);
          state.endpoints = state.members.map((m) => m.endpoint);
        }
      } else if (options.command === 'resume' && member.killed) await startNative(member, state.executable, memberArgs(state, member));
      else await signalOwned(member, { kill: 'SIGKILL', pause: 'SIGSTOP', resume: 'SIGCONT' }[options.command]);
      member.killed = options.command === 'kill';
      member.paused = options.command === 'pause';
      await writeJson(configFile(run), state);
      if (options.command === 'resume' && !state.members.some((m) => m.partitioned || m.killed || m.paused)) await ready(state);
    } else throw new Error('expected up/down/status/kill/pause/resume/partition/heal/compact');
    await writeJson(configFile(run), state);
    console.log(`${options.command}: OK`);
  } finally { await lock.close(); await rm(lockPath); }
});
