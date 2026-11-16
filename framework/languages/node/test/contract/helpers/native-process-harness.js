const { fork } = require('node:child_process');
const path = require('node:path');

module.exports = function nativeProcessHarness(fixture) {
  function startChild(children, options) {
    const child = fork(fixture, [], {
      cwd: path.resolve(__dirname, '../../..'),
      env: {
        ...process.env,
        ZLINK_TEST_ROLE: options.role,
        ...(options.spotId === undefined ? {} : { ZLINK_TEST_SPOT: options.spotId }),
        ...(options.nextRoutingId === undefined
          ? {}
          : { ZLINK_TEST_NEXT_ROUTING_ID: options.nextRoutingId }),
        ...(options.flowDirectory === undefined
          ? {}
          : { ZLINK_NODE_BOOTSTRAP_FLOW_DIR: options.flowDirectory }),
        ZLINK_TEST_MESH: options.meshName,
        ZLINK_TEST_ENDPOINT: options.endpoint,
        ZLINK_TEST_ROUTING_ID: options.routingId,
        ZLINK_TEST_TARGET_ROUTING_ID: options.targetRoutingId,
        ZLINK_TEST_REDIS_PREFIX: options.keyPrefix,
        ...(options.targetEndpoint === undefined
          ? {}
          : { ZLINK_TEST_TARGET_ENDPOINT: options.targetEndpoint }),
        ...(options.pauseFirstClose === true ? { ZLINK_TEST_PAUSE_FIRST_CLOSE: '1' } : {})
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
        reject(
          new Error(`${options.role} exited before ready: code=${code} signal=${signal}\n${stderr}`)
        );
      });
    });
    child.on('message', (message) => {
      if (
        typeof message?.type === 'string' &&
        message.type !== 'result' &&
        message.type !== 'error' &&
        message.type !== 'ready' &&
        message.type !== 'fatal'
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
      if ((message?.type !== 'result' && message?.type !== 'error') || !pending.has(message.id)) {
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
      const failure = new Error(`${options.role} exited: code=${code} signal=${signal}\n${stderr}`);
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

  async function stopChild(entry) {
    const child = entry.child ?? entry;
    if (child.exitCode !== null || child.signalCode !== null) return;
    const exited = new Promise((resolve) => child.once('exit', resolve));
    if (child.connected) child.send({ type: 'stop' }, () => {});
    await exited;
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

  return { startChild, stopChild, deletePrefix };
};
