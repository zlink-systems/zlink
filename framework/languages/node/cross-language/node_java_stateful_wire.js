/* SPDX-License-Identifier: FSL-1.1-ALv2 */

const assert = require('node:assert/strict');
const fs = require('node:fs');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');
const { spawn } = require('node:child_process');

const repo = path.resolve(__dirname, '../../../..');
const logs = process.env.ZLINK_NODE_WIRE_LOG_DIR ?? os.tmpdir();
const runDir = fs.mkdtempSync(path.join(logs, 'node-java-stateful-'));
const children = [];

async function port() {
  const server = net.createServer();
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const value = server.address().port;
  await new Promise((resolve) => server.close(resolve));
  return value;
}

function start(command, args, name, env = process.env) {
  const output = fs.openSync(path.join(runDir, `${name}.log`), 'w');
  const child = spawn(command, args, { cwd: repo, env, stdio: ['ignore', output, output] });
  fs.closeSync(output);
  children.push(child);
  return child;
}

async function line(file, marker, timeoutMs = 90_000) {
  if (fs.readFileSync(file, 'utf8').includes(marker)) return;
  await new Promise((resolve, reject) => {
    const changed = () => {
      if (fs.readFileSync(file, 'utf8').includes('user-spot-join-failed|')) {
        clearTimeout(deadline);
        fs.unwatchFile(file, changed);
        reject(new Error(fs.readFileSync(file, 'utf8')));
        return;
      }
      if (!fs.readFileSync(file, 'utf8').includes(marker)) return;
      clearTimeout(deadline);
      fs.unwatchFile(file, changed);
      resolve();
    };
    fs.watchFile(file, { interval: 50 }, changed);
    const deadline = setTimeout(() => {
      fs.unwatchFile(file, changed);
      reject(new Error(`${file} did not report ${marker}`));
    }, timeoutMs);
  });
}

async function main() {
  const ports = new Set();
  while (ports.size < 3) ports.add(await port());
  const [redisPort, javaPort, nodePort] = [...ports];
  const sourceEvents = path.join(runDir, 'node.events');
  const targetEvents = path.join(runDir, 'java-user-spot-target.events');
  const redisLog = path.join(runDir, 'redis.log');
  const startFile = path.join(runDir, 'start');
  fs.writeFileSync(sourceEvents, '');
  fs.writeFileSync(targetEvents, '');
  fs.writeFileSync(redisLog, '');

  start(
    'redis-server',
    [
      '--port',
      String(redisPort),
      '--bind',
      '127.0.0.1',
      '--save',
      '',
      '--appendonly',
      'no',
      '--daemonize',
      'no'
    ],
    'redis'
  );
  await line(redisLog, 'Ready to accept connections', 20_000);

  const java = path.join(
    repo,
    'framework/languages/java/cross-language/Host/build/install/zlink-cross-language-host/bin/zlink-cross-language-host'
  );
  start(
    java,
    [
      'user-spot-target',
      '--mesh-name',
      'cross.user-spot-join',
      '--node-rid',
      'java-user-spot-join-target',
      '--peer-rid',
      'node-user-spot-join-source',
      '--bind-endpoint',
      `tcp://127.0.0.1:${javaPort}`,
      '--redis-endpoint',
      `127.0.0.1:${redisPort}`,
      '--redis-key-prefix',
      'zlink-cross-user-spot-join',
      '--spot-id',
      'cross-lang-user-spot',
      '--actor-id',
      'cross-lang-user-spot-actor',
      '--event-file',
      targetEvents,
      '--ready-file',
      path.join(runDir, 'java.ready'),
      '--stop-file',
      path.join(runDir, 'java.stop')
    ],
    'java'
  );
  await line(targetEvents, 'user-spot-created|spot=cross-lang-user-spot');

  start(
    process.execPath,
    [
      path.join(__dirname, 'user_spot_join_host.js'),
      'user-spot-join-source',
      '--mesh-name',
      'cross.user-spot-join',
      '--node-rid',
      'node-user-spot-join-source',
      '--bind-endpoint',
      `tcp://127.0.0.1:${nodePort}`,
      '--redis-endpoint',
      `127.0.0.1:${redisPort}`,
      '--redis-key-prefix',
      'zlink-cross-user-spot-join',
      '--spot-id',
      'cross-lang-user-spot',
      '--actor-id',
      'cross-lang-user-spot-actor',
      '--event-file',
      sourceEvents,
      '--start-file',
      startFile,
      '--ready-file',
      path.join(runDir, 'node.ready')
    ],
    'node',
    { ...process.env, ZLINK_NODE_JAVA_STATEFUL_PROBE: '1' }
  );
  await Promise.all([
    line(sourceEvents, 'user-spot-source-peer-ready|ready=true'),
    line(targetEvents, 'user-spot-source-peer-ready|ready=true')
  ]);
  fs.writeFileSync(startFile, 'start\n');
  await line(sourceEvents, 'stateful-22-wire|target=java-user-spot-join-target|bytes=');
  await line(sourceEvents, 'stateful-22|kind=InternalFailure|target=java-user-spot-join-target');
  await line(sourceEvents, 'stateful-25-wire|target=java-user-spot-join-target|bytes=');
  await line(sourceEvents, 'stateful-25|nodeRid=java-user-spot-join-target|actor=');
  assert.match(
    fs.readFileSync(targetEvents, 'utf8'),
    /user-spot-probe\|nodeRid=java-user-spot-join-target/
  );
  process.stdout.write(`Node↔Java 22/25 passed; runDir=${runDir}\n`);
}

main()
  .catch((error) => {
    process.stderr.write(`${error.stack ?? error}\nrunDir=${runDir}\n`);
    process.exitCode = 1;
  })
  .finally(() => {
    for (const child of children) child.kill('SIGTERM');
  });
