const assert = require('node:assert/strict');
const { fork } = require('node:child_process');
const net = require('node:net');
const path = require('node:path');
const test = require('node:test');
const { createClient } = require('redis');

const fixture = path.join(
  __dirname,
  'fixtures',
  'user-spot-native-process.js'
);
const redisUrl = process.env.ZLINK_TEST_REDIS_URL ?? 'redis://127.0.0.1:6379';

test(
  'public SpotManager completes command 47/48 through two native MeshNode processes',
  { timeout: 60_000 },
  async (t) => {
    const runId = `${process.pid}-${Date.now()}-${Math.random().toString(16).slice(2)}`;
    const meshName = `m6b-native-${runId}`;
    const keyPrefix = `zlink:test:m6b-node:${runId}:`;
    const targetRoutingId = `m6b-target-${runId}`;
    const [sourcePort, targetPort, proxyPort] = await Promise.all([
      reservePort(),
      reservePort(),
      reservePort()
    ]);
    const children = [];
    const proxy = new TcpReplayFaultProxy(proxyPort, targetPort);
    const redis = createClient({
      url: redisUrl,
      socket: { reconnectStrategy: false }
    });
    redis.on('error', () => {});
    try {
      await redis.connect();
      await redis.ping();
    } catch (error) {
      if (redis.isOpen) redis.disconnect();
      t.skip(`Redis is unavailable at ${redisUrl}: ${error.message}`);
      return;
    }
    t.after(async () => {
      await Promise.all(children.map(stopChild));
      await proxy.close();
      await deletePrefix(redis, keyPrefix);
      if (redis.isOpen) await redis.quit();
    });

    const target = startChild(children, {
      role: 'target',
      meshName,
      endpoint: `tcp://127.0.0.1:${targetPort}`,
      routingId: targetRoutingId,
      targetRoutingId,
      keyPrefix,
      pauseFirstClose: true
    });
    const targetReady = await target.ready;
    assert.equal(targetReady.role, 'target');
    await proxy.start();

    const source = startChild(children, {
      role: 'source',
      meshName,
      endpoint: `tcp://127.0.0.1:${sourcePort}`,
      routingId: `m6b-source-${runId}`,
      targetRoutingId,
      keyPrefix,
      targetEndpoint: `tcp://127.0.0.1:${proxyPort}`
    });
    const sourceReady = await source.ready;
    assert.equal(sourceReady.role, 'source');
    assert.notEqual(sourceReady.pid, targetReady.pid);
    const created = await source.command('create');
    assert.equal(created.state, 'created');
    assert.equal(created.spot.meshName, meshName);
    assert.equal(created.spot.nodeRid, targetRoutingId);

    const existing = await source.command('getOrCreate');
    assert.equal(existing.state, 'existing');
    assert.deepEqual(existing.spot, created.spot);

    assert.deepEqual(await source.command('find'), created.spot);
    const close = source.command('close');
    await target.waitForEvent('close-entered');
    const dropped = proxy.dropNextServerChunk();
    await target.command('releaseFirstClose');
    await dropped;
    assert.equal(await close, true);
    assert.equal(await target.command('closeExecutions'), 1);
    assert.ok(proxy.connectionCount >= 2, 'Command 48 replay did not reconnect through the proxy.');
    assert.equal(await source.command('findAfterClose'), null);
  }
);

function startChild(children, options) {
  const child = fork(fixture, [], {
    cwd: path.resolve(__dirname, '../..'),
    env: {
      ...process.env,
      ZLINK_TEST_ROLE: options.role,
      ZLINK_TEST_MESH: options.meshName,
      ZLINK_TEST_ENDPOINT: options.endpoint,
      ZLINK_TEST_ROUTING_ID: options.routingId,
      ZLINK_TEST_TARGET_ROUTING_ID: options.targetRoutingId,
      ZLINK_TEST_REDIS_URL: redisUrl,
      ZLINK_TEST_REDIS_PREFIX: options.keyPrefix,
      ...(options.targetEndpoint === undefined
        ? {}
        : { ZLINK_TEST_TARGET_ENDPOINT: options.targetEndpoint }),
      ...(options.pauseFirstClose === true
        ? { ZLINK_TEST_PAUSE_FIRST_CLOSE: '1' }
        : {})
    },
    stdio: ['ignore', 'pipe', 'pipe', 'ipc']
  });
  children.push(child);
  let stderr = '';
  child.stderr.setEncoding('utf8');
  child.stderr.on('data', (chunk) => {
    stderr += chunk;
  });
  let nextId = 1;
  const pending = new Map();
  const queuedEvents = new Map();
  const eventWaiters = new Map();
  const ready = new Promise((resolve, reject) => {
    const timeout = setTimeout(() => {
      reject(new Error(`${options.role} readiness timed out.\n${stderr}`));
    }, 10_000);
    const onMessage = (message) => {
      if (message?.type === 'ready') {
        clearTimeout(timeout);
        resolve(message);
      } else if (message?.type === 'fatal') {
        clearTimeout(timeout);
        reject(new Error(message.message));
      }
    };
    child.on('message', onMessage);
    child.once('exit', (code, signal) => {
      clearTimeout(timeout);
      reject(new Error(
        `${options.role} exited before ready: code=${code} signal=${signal}\n${stderr}`
      ));
    });
  });
  child.on('message', (message) => {
    if (
      typeof message?.type === 'string'
      && message.type !== 'result'
      && message.type !== 'error'
      && message.type !== 'ready'
      && message.type !== 'fatal'
    ) {
      const waiter = eventWaiters.get(message.type);
      if (waiter !== undefined) {
        eventWaiters.delete(message.type);
        waiter(message);
      } else {
        const queue = queuedEvents.get(message.type) ?? [];
        queue.push(message);
        queuedEvents.set(message.type, queue);
      }
    }
    if (
      (message?.type !== 'result' && message?.type !== 'error')
      || !pending.has(message.id)
    ) {
      return;
    }
    const operation = pending.get(message.id);
    pending.delete(message.id);
    if (message.type === 'error') {
      operation.reject(new Error(message.message));
    } else {
      operation.resolve(message.value);
    }
  });
  child.once('exit', (code, signal) => {
    const failure = new Error(
      `${options.role} exited: code=${code} signal=${signal}\n${stderr}`
    );
    for (const operation of pending.values()) operation.reject(failure);
    pending.clear();
  });
  return {
    child,
    ready,
    command(action) {
      const id = nextId++;
      return new Promise((resolve, reject) => {
        const timeout = setTimeout(() => {
          pending.delete(id);
          reject(new Error(`${options.role} command '${action}' timed out.`));
        }, 35_000);
        pending.set(id, {
          resolve(value) {
            clearTimeout(timeout);
            resolve(value);
          },
          reject(error) {
            clearTimeout(timeout);
            reject(error);
          }
        });
        child.send({ id, action }, (error) => {
          if (error === null) return;
          const operation = pending.get(id);
          pending.delete(id);
          operation?.reject(error);
        });
      });
    },
    waitForEvent(type) {
      const queue = queuedEvents.get(type);
      if (queue !== undefined && queue.length > 0) {
        return Promise.resolve(queue.shift());
      }
      return new Promise((resolve, reject) => {
        const timeout = setTimeout(() => {
          eventWaiters.delete(type);
          reject(new Error(`${options.role} event '${type}' timed out.`));
        }, 10_000);
        eventWaiters.set(type, (message) => {
          clearTimeout(timeout);
          resolve(message);
        });
      });
    }
  };
}

class TcpReplayFaultProxy {
  constructor(listenPort, targetPort) {
    this.listenPort = listenPort;
    this.targetPort = targetPort;
    this.server = net.createServer((client) => this.accept(client));
    this.sockets = new Set();
    this.connectionCount = 0;
    this.drop = undefined;
  }

  async start() {
    await new Promise((resolve, reject) => {
      this.server.once('error', reject);
      this.server.listen(this.listenPort, '127.0.0.1', resolve);
    });
  }

  dropNextServerChunk() {
    if (this.drop !== undefined) {
      throw new Error('A proxy drop is already armed.');
    }
    return new Promise((resolve) => {
      this.drop = resolve;
    });
  }

  accept(client) {
    this.connectionCount++;
    const upstream = net.connect({
      host: '127.0.0.1',
      port: this.targetPort
    });
    this.sockets.add(client);
    this.sockets.add(upstream);
    client.on('data', (chunk) => {
      if (!upstream.destroyed) upstream.write(chunk);
    });
    upstream.on('data', (chunk) => {
      if (this.drop !== undefined) {
        const dropped = this.drop;
        this.drop = undefined;
        dropped();
        client.destroy();
        upstream.destroy();
        return;
      }
      if (!client.destroyed) client.write(chunk);
    });
    const closePair = () => {
      client.destroy();
      upstream.destroy();
      this.sockets.delete(client);
      this.sockets.delete(upstream);
    };
    client.on('error', closePair);
    upstream.on('error', closePair);
    client.on('close', closePair);
    upstream.on('close', closePair);
  }

  async close() {
    for (const socket of this.sockets) socket.destroy();
    this.sockets.clear();
    if (!this.server.listening) return;
    await new Promise((resolve) => this.server.close(resolve));
  }
}

async function stopChild(entry) {
  const child = entry.child ?? entry;
  if (child.exitCode !== null || child.signalCode !== null) return;
  const exited = new Promise((resolve) => child.once('exit', resolve));
  if (child.connected) child.send({ type: 'stop' }, () => {});
  else child.kill('SIGTERM');
  const timer = setTimeout(() => child.kill('SIGKILL'), 5_000);
  await exited;
  clearTimeout(timer);
}

async function reservePort() {
  const server = net.createServer();
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  const { port } = server.address();
  await new Promise((resolve, reject) => {
    server.close((error) => error === undefined ? resolve() : reject(error));
  });
  return port;
}

async function deletePrefix(redis, prefix) {
  const keys = [];
  for await (const key of redis.scanIterator({
    MATCH: `${prefix}*`,
    COUNT: 100
  })) {
    keys.push(...(Array.isArray(key) ? key : [key]));
  }
  if (keys.length > 0) await redis.del(keys);
}
