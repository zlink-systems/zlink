import crypto from 'node:crypto';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import process from 'node:process';

// Loopback ports for the Node sample runners and tests. Application ports come
// from 28100-29999 inside the 20000-29999 block that the test hosts reserve
// from the ephemeral range, so no outbound connection is handed one of them.
// A port is taken by creating its lock file in the lease directory that every
// runner and test on the host consults.
export const redisPortRange = { min: 28000, max: 28099 };
export const applicationPortRange = { min: 28100, max: 29999 };
export const sharedPortLeaseDir = path.join(os.tmpdir(), 'zlink-sample-port-leases');

/**
 * Leases one loopback port of `range` that can be bound now. The lease holds
 * until `releasePortLease`, so a server can stop and start again on the port
 * without another process taking it in between. `checkRunning` runs before
 * each attempt and after the bind probe; when it throws, the lease is released
 * and the error propagates.
 */
export async function leaseLoopbackPort(range, { exclude, checkRunning } = {}) {
  for (let attempt = 0; attempt < 200; attempt += 1) {
    checkRunning?.();
    const port = range.min + Math.floor(Math.random() * (range.max - range.min + 1));
    if (exclude?.has(port)) continue;
    const leasePath = acquirePortLease(port);
    if (leasePath === undefined) continue;
    const available = await canBind(port);
    try {
      checkRunning?.();
    } catch (error) {
      releasePortLease(leasePath);
      throw error;
    }
    if (available) return { port, leasePath };
    releasePortLease(leasePath);
  }
  return undefined;
}

export function releasePortLease(leasePath) {
  fs.rmSync(leasePath, { force: true });
}

/**
 * Takes the lock file of `port`, or returns undefined when a running process
 * holds it. Every step is a single atomic file operation:
 *
 * - A lock is created by hard-linking a complete temporary file to the lock
 *   path, so no reader ever sees a lock without its owner.
 * - A lock whose owner is not running is reclaimed by the one process that
 *   links it to a claim named after that lock's token. The claim is checked to
 *   hold that same lock before the lock is removed, so a lock created after the
 *   stale one was read is never removed.
 */
export function acquirePortLease(port, leaseDir = sharedPortLeaseDir) {
  fs.mkdirSync(leaseDir, { recursive: true, mode: 0o700 });
  const leasePath = path.join(leaseDir, `${port}.lock`);
  for (let attempt = 0; attempt < 2; attempt += 1) {
    if (createLock(leasePath)) return leasePath;
    const observed = readLock(leasePath);
    if (observed === undefined) continue;
    const owner = Number.parseInt(observed, 10);
    if (!Number.isInteger(owner) || isProcessRunning(owner)) return undefined;
    if (!removeStaleLock(leasePath, observed)) return undefined;
  }
  return undefined;
}

function createLock(leasePath) {
  const token = `${process.pid} ${crypto.randomUUID()}\n`;
  const draft = `${leasePath}.${crypto.randomUUID()}.draft`;
  fs.writeFileSync(draft, token, { mode: 0o600 });
  try {
    fs.linkSync(draft, leasePath);
    return true;
  } catch (error) {
    if (error?.code === 'EEXIST') return false;
    throw error;
  } finally {
    fs.rmSync(draft, { force: true });
  }
}

function removeStaleLock(leasePath, observed) {
  const claim = `${leasePath}.${observed.split(' ')[1]?.trim() ?? 'unnamed'}.claim`;
  try {
    fs.linkSync(leasePath, claim);
  } catch (error) {
    // EEXIST: another process is reclaiming this lock. ENOENT: it is gone.
    if (error?.code === 'EEXIST') return false;
    if (error?.code === 'ENOENT') return true;
    throw error;
  }
  try {
    if (readLock(claim) !== observed) return false;
    fs.rmSync(leasePath, { force: true });
    return true;
  } finally {
    fs.rmSync(claim, { force: true });
  }
}

function readLock(leasePath) {
  try {
    return fs.readFileSync(leasePath, 'utf8');
  } catch (error) {
    if (error?.code === 'ENOENT') return undefined;
    throw error;
  }
}

function isProcessRunning(pid) {
  try {
    process.kill(pid, 0);
    return true;
  } catch (error) {
    return error?.code === 'EPERM';
  }
}

export function canBind(port) {
  return new Promise((resolve) => {
    const server = net.createServer();
    server.once('error', () => resolve(false));
    server.listen(port, '127.0.0.1', () => server.close(() => resolve(true)));
  });
}
