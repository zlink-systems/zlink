const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');

const samplesRoot = path.resolve(__dirname, '../../..');

function readProcess(pid) {
  try {
    const stat = fs.readFileSync(`/proc/${pid}/stat`, 'utf8').split(') ')[1].split(' ');
    return {
      pid, ppid: Number(stat[1]), pgid: Number(stat[2]), start: stat[19], state: stat[0],
      command: fs.readFileSync(`/proc/${pid}/cmdline`, 'utf8').replaceAll('\0', ' ').trim()
    };
  } catch (error) {
    if (error.code !== 'ENOENT' && error.code !== 'ESRCH') throw error;
    return undefined;
  }
}

function descendants(root, observed) {
  const queue = [root, ...observed.values()];
  const visited = new Set();
  while (queue.length > 0) {
    const expected = queue.pop();
    if (visited.has(expected.pid)) continue;
    visited.add(expected.pid);
    const current = readProcess(expected.pid);
    if (!current || current.start !== expected.start) continue;
    const previous = observed.get(current.pid);
    observed.set(current.pid, {
      ...current,
      parentAtDiscovery: previous?.parentAtDiscovery ?? current.ppid,
      command: current.command || previous?.command || ''
    });
    let children;
    try {
      children = fs.readFileSync(`/proc/${current.pid}/task/${current.pid}/children`, 'utf8');
    } catch (error) {
      if (error.code !== 'ENOENT' && error.code !== 'ESRCH') throw error;
      continue;
    }
    for (const pid of children.trim().split(/\s+/).filter(Boolean).map(Number)) {
      const child = readProcess(pid);
      if (child) queue.push(child);
    }
  }
}

for (const mode of ['normal', 'sigterm', 'sigterm-b8']) {
  test(`ZoneWorld reaps its preview tree and removes run directories after ${mode}`, {
    skip: process.platform !== 'linux', timeout: 600_000
  }, async (t) => {
    const evidence = fs.mkdtempSync(path.join(os.tmpdir(), `zoneworld-cleanup-${mode}-`));
    const tempRoot = path.join(evidence, 'tmp');
    fs.mkdirSync(tempRoot);
    const logPath = path.join(evidence, 'run.log');
    const log = fs.openSync(logPath, 'w');
    // This is the same aggregate entry point and single-PID SIGTERM used by the npm gate.
    const runner = spawn('bash', [path.join(samplesRoot, 'run_samples.sh'), 'ZoneWorld'], {
      env: { ...process.env, TMPDIR: tempRoot }, stdio: ['ignore', log, log]
    });
    fs.closeSync(log);
    const root = readProcess(runner.pid);
    assert.ok(root);
    const observed = new Map();
    const runDirs = new Set();
    const previewPids = new Set();
    const esbuildPids = new Set();
    let terminated = false;
    let passed = false;
    let monitor;
    const exited = new Promise((resolve, reject) => {
      runner.once('error', reject);
      runner.once('exit', (code, signal) => resolve({ code, signal }));
    });
    t.after(async () => {
      clearInterval(monitor);
      descendants(root, observed);
      fs.writeFileSync(path.join(evidence, 'processes.json'), JSON.stringify([...observed.values()], null, 2));
      // A failing regression must also release only the processes it observed under its runner.
      const killed = [];
      for (const previous of [...observed.values()].reverse()) {
        const current = readProcess(previous.pid);
        if (!current || current.start !== previous.start) continue;
        try {
          process.kill(current.pid, 'SIGKILL');
          killed.push(current.pid);
        } catch (error) {
          if (error.code !== 'ESRCH') throw error;
        }
      }
      await exited;
      if (passed) fs.rmSync(tempRoot, { recursive: true, force: true });
      t.diagnostic(`evidence=${evidence}; regression-owned cleanup PIDs=${killed.join(',')}`);
    });
    const observe = () => {
      descendants(root, observed);
      for (const entry of fs.readdirSync(tempRoot)) {
        if (entry.startsWith('zlink-zoneworld-')) runDirs.add(path.join(tempRoot, entry));
      }
      for (const entry of observed.values()) {
        if (/\/.+vite preview .*--outDir /.test(entry.command)) previewPids.add(entry.pid);
        if (/\/esbuild --service=/.test(entry.command)) esbuildPids.add(entry.pid);
      }
      const previewReady = [...runDirs].some((dir) => {
        const previewLog = path.join(dir, 'logs/shared-browser-preview.log');
        try {
          return /http:\/\/127\.0\.0\.1:\d+/.test(fs.readFileSync(previewLog, 'utf8'));
        } catch (error) {
          if (error.code !== 'ENOENT') throw error;
          return false;
        }
      });
      const b8Ready = mode === 'sigterm-b8' && runDirs.size >= 2
        && [...observed.values()].some((entry) => /session_route_block_proxy.py/.test(entry.command));
      if (!terminated && (b8Ready
        || mode === 'sigterm' && previewPids.size > 0 && esbuildPids.size > 0 && previewReady)) {
        terminated = true;
        runner.kill('SIGTERM');
      }
    };
    // Observe ancestry while it exists, so reparented survivors cannot escape the assertion.
    monitor = setInterval(observe, 25);
    observe();
    const result = await exited;
    clearInterval(monitor);
    observe();
    const remaining = [...observed.values()].filter((previous) => {
      const current = readProcess(previous.pid);
      return current && current.start === previous.start;
    });
    assert.deepEqual(remaining, [], `Owned processes remain; see ${logPath}`);
    assert.ok(runDirs.size >= 2, 'The full sample and isolated B8 run must both be observed');
    assert.deepEqual([...runDirs].filter((dir) => fs.existsSync(dir)), [], 'Run directories remain');
    if (mode !== 'sigterm-b8') {
      assert.ok(previewPids.size > 0, 'The sample must actually start Vite preview');
      assert.ok(esbuildPids.size > 0, 'The sample must actually start the esbuild service');
    }
    assert.deepEqual(result, { code: mode === 'normal' ? 0 : 143, signal: null }, `See ${logPath}`);
    if (mode === 'normal') assert.match(fs.readFileSync(logPath, 'utf8'), /sample ZoneWorld completed/);
    else assert.ok(terminated, 'SIGTERM must interrupt the selected stage');
    t.diagnostic(`observed=${observed.size} preview=${previewPids.size} esbuild=${esbuildPids.size} runDirs=${runDirs.size}; survivors=0`);
    passed = true;
  });
}
