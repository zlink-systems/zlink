const crypto = require('node:crypto');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

function acquireNodeTestGateLock(nodeRoot, gateName) {
  const rootHash = crypto.createHash('sha256').update(nodeRoot).digest('hex').slice(0, 16);
  const lockPath = path.join(os.tmpdir(), `zlink-node-test-gate-${rootHash}`);
  const ownerPath = path.join(lockPath, 'owner.json');
  const owner = {
    pid: process.pid,
    gateName,
    token: crypto.randomUUID(),
    startedAt: new Date().toISOString()
  };

  for (let attempt = 0; attempt < 2; attempt += 1) {
    try {
      fs.mkdirSync(lockPath);
      fs.writeFileSync(ownerPath, JSON.stringify(owner));
      return registerLockRelease(lockPath, ownerPath, owner);
    } catch (error) {
      if (error?.code !== 'EEXIST') throw error;
      const existing = readOwner(ownerPath);
      if (existing !== undefined && processIsAlive(existing.pid)) {
        throw new Error(
          `Node test gate '${existing.gateName}' is already running with pid ${existing.pid} since ${existing.startedAt}.`
        );
      }
      fs.rmSync(lockPath, { recursive: true, force: true });
    }
  }

  throw new Error(`Could not acquire Node test gate lock '${lockPath}'.`);
}

function registerLockRelease(lockPath, ownerPath, owner) {
  let released = false;
  const release = () => {
    if (released) return;
    released = true;
    const current = readOwner(ownerPath);
    if (current?.token === owner.token) {
      fs.rmSync(lockPath, { recursive: true, force: true });
    }
  };
  process.once('exit', release);
  for (const [signal, exitCode] of [['SIGINT', 130], ['SIGTERM', 143]]) {
    process.once(signal, () => {
      release();
      process.exit(exitCode);
    });
  }
  return release;
}

function readOwner(ownerPath) {
  try {
    return JSON.parse(fs.readFileSync(ownerPath, 'utf8'));
  } catch {
    return undefined;
  }
}

function processIsAlive(pid) {
  if (!Number.isInteger(pid) || pid <= 0) return false;
  try {
    process.kill(pid, 0);
    return true;
  } catch (error) {
    return error?.code === 'EPERM';
  }
}

module.exports = { acquireNodeTestGateLock };
