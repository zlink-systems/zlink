import { parseArgs } from 'node:util';
import { execFile, spawn } from 'node:child_process';
import { promisify } from 'node:util';
import { mkdir, open, readFile, readdir, rm, rename, writeFile } from 'node:fs/promises';
import { createServer } from 'node:net';
import { randomInt } from 'node:crypto';
import { LIMITS } from './constants.mjs';

export const delay = (ms) => new Promise((r) => setTimeout(r, ms));
export function args(options, tokens = process.argv.slice(2)) {
  const { values, positionals } = parseArgs({ args: tokens, allowPositionals: true,
    options: Object.fromEntries(Object.entries(options).map(([key, value]) => [key, { type: value }])) });
  return { ...values, command: positionals[0], positionals };
}
export function integer(value, name, min = 1, max = Number.MAX_SAFE_INTEGER) {
  if (!/^\d+$/.test(String(value)) || !Number.isSafeInteger(Number(value)) || Number(value) < min || Number(value) > max)
    throw new Error(`${name} must be an integer in ${min}..${max}`);
  return Number(value);
}
export function revision(value) {
  if (!/^[1-9][0-9]*$/.test(String(value)) || BigInt(value) > BigInt(LIMITS.maximumRevision))
    throw new Error(`revision must be a decimal integer in 1..${LIMITS.maximumRevision}`);
  return String(value);
}
export const readJson = async (path) => JSON.parse(await readFile(path, 'utf8'));
export async function writeJson(path, value) {
  const temporary = `${path}.${process.pid}.tmp`;
  await writeFile(temporary, `${JSON.stringify(value, null, 2)}\n`);
  await rename(temporary, path);
}
const execute = promisify(execFile);
export async function command(file, argv, options = {}) {
  try { return (await execute(file, argv, { timeout: LIMITS.commandMs, maxBuffer: LIMITS.commandOutputBytes, ...options })).stdout.trim(); }
  catch (error) { throw new Error(`${file} ${argv.join(' ')}: ${error.stderr || error.message}`, { cause: error }); }
}
export async function commandToLog(file, argv, path) {
  const log = await open(path, 'w');
  try {
    await new Promise((resolve, reject) => {
      const child = spawn(file, argv, { stdio: ['ignore', log.fd, log.fd] });
      const timeout = setTimeout(() => { child.kill(); reject(new Error(`${file}: log capture timed out`)); }, LIMITS.commandMs);
      child.once('error', (error) => { clearTimeout(timeout); reject(error); });
      child.once('exit', (code) => { clearTimeout(timeout); code === 0 ? resolve() : reject(new Error(`${file} log capture exited ${code}`)); });
    });
  } finally { await log.close(); }
}
export async function background(file, argv, logPath) {
  const log = await open(logPath, 'a');
  try {
    const child = spawn(file, argv, { detached: true, stdio: ['ignore', log.fd, log.fd] });
    await new Promise((resolve, reject) => { child.once('spawn', resolve); child.once('error', reject); });
    child.unref();
    return child.pid;
  } finally { await log.close(); }
}
export async function rpc(endpoint, path, body = {}, timeout = LIMITS.requestMs) {
  const response = await fetch(new URL(path, endpoint), { method: 'POST',
    headers: { 'content-type': 'application/json' }, body: JSON.stringify(body), signal: AbortSignal.timeout(timeout) });
  const text = await response.text();
  let result;
  try { result = JSON.parse(text); } catch { throw new Error(`${endpoint}${path}: HTTP ${response.status}: ${text}`); }
  if (!response.ok || result.error || result.code) throw new Error(`${endpoint}${path}: HTTP ${response.status}: ${text}`);
  return result;
}
export const portLocks = new URL('./.runs/.ports/', import.meta.url);
export async function releasePorts(runId) {
  await mkdir(portLocks, { recursive: true });
  for (const name of await readdir(portLocks)) {
    const path = new URL(name, portLocks);
    let owner;
    try { owner = await readFile(path, 'utf8'); }
    catch (error) { if (error.code === 'ENOENT') continue; throw error; }
    if (owner === runId) await rm(path);
  }
}
export async function reservePorts(count, runId) {
  await mkdir(portLocks, { recursive: true });
  const ports = [];
  try {
    for (let i = 0; i < count; i++) {
      let selected;
      for (let attempt = 0; attempt < LIMITS.portAttempts; attempt++) {
        const port = randomInt(LIMITS.portMin, LIMITS.portMax);
        const path = new URL(String(port), portLocks);
        let lock;
        try { lock = await open(path, 'wx'); }
        catch (error) { if (error.code === 'EEXIST') continue; throw error; }
        try { await lock.writeFile(runId); } finally { await lock.close(); }
        const server = createServer();
        try {
          await new Promise((resolve, reject) => {
            server.once('error', reject);
            server.listen({ host: '127.0.0.1', port }, resolve);
          });
          await new Promise((resolve) => server.close(resolve));
          selected = port; break;
        } catch (error) {
          // A port held by a process outside the harness is skipped while choosing; nothing has started yet.
          await rm(path);
          if (error.code !== 'EADDRINUSE' && error.code !== 'EACCES') throw error;
        }
      }
      if (!selected) throw new Error('cannot acquire run port locks');
      ports.push(selected);
    }
    return ports;
  } catch (error) { await releasePorts(runId); throw error; }
}
export const main = (fn) => fn().catch((error) => { console.error(error); process.exitCode = 1; });
