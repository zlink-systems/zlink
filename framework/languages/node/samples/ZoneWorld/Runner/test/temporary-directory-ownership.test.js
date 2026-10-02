const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');

test('ZoneWorld parent removes nested temporary resources after forced child termination', {
  skip: process.platform !== 'linux', timeout: 60_000
}, async (t) => {
  const evidence = fs.mkdtempSync(path.join(os.tmpdir(), 'node-reloc-temp-owner-'));
  const childFile = path.join(evidence, 'child.mjs');
  const definitionFile = path.join(evidence, 'runner.mjs');
  fs.writeFileSync(childFile, `
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
const runDir = fs.mkdtempSync(path.join(os.tmpdir(), 'zlink-zoneworld-child-'));
process.on('SIGTERM', () => {});
net.createServer().listen(0, '127.0.0.1', () => {
  process.stdout.write('r4-temp-owner=' + JSON.stringify({ pid: process.pid, runDir }) + '\\n');
});
`);
  fs.writeFileSync(definitionFile, `
export const sampleName = 'ZoneWorld';
export async function runSample(ctx) {
  await ctx.runCommand(process.execPath, [${JSON.stringify(childFile)}]);
}
`);
  const runner = spawn(process.execPath, [
    path.resolve(__dirname, '../../scripts/run-sample.mjs'), definitionFile
  ], { env: { ...process.env, TMPDIR: evidence }, stdio: ['ignore', 'pipe', 'pipe'] });
  let child;
  let output = '';
  const exited = new Promise((resolve, reject) => {
    runner.once('error', reject);
    runner.once('exit', (code, signal) => resolve({ code, signal }));
  });
  runner.stdout.on('data', (chunk) => {
    output += chunk.toString();
    const marker = output.match(/r4-temp-owner=(\{[^\n]+\})\n/);
    if (child === undefined && marker !== null) {
      child = JSON.parse(marker[1]);
      assert.ok(fs.existsSync(child.runDir));
      runner.kill('SIGTERM');
    }
  });
  runner.stderr.on('data', (chunk) => { output += chunk.toString(); });
  t.after(async () => {
    if (runner.exitCode === null && runner.signalCode === null) runner.kill('SIGKILL');
    await exited;
    if (child !== undefined) {
      try { process.kill(child.pid, 'SIGKILL'); }
      catch (error) { if (error.code !== 'ESRCH') throw error; }
    }
    fs.writeFileSync(path.join(evidence, 'run.log'), output);
    t.diagnostic(`evidence=${evidence}`);
  });
  const result = await exited;
  assert.deepEqual(result, { code: 143, signal: null });
  assert.ok(child, 'the actual child must reach its temporary resource creation');
  assert.match(output, /nested|command-1/);
  assert.match(output, /SIGKILL/);
  assert.equal(fs.existsSync(child.runDir), false, 'the parent must remove the child temporary directory');
  assert.throws(() => process.kill(child.pid, 0), { code: 'ESRCH' });
});
