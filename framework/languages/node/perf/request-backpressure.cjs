'use strict';

// Public typed JSON request path versus the binding, in one process.
// No application in-flight window: only admission and an event-loop yield every 256 submits.
const assert = require('node:assert/strict');
const { createHook } = require('node:async_hooks');
const { performance } = require('node:perf_hooks');
const { Session } = require('node:inspector');
const { setImmediate: immediate } = require('node:timers/promises');
const net = require('node:net');
require('reflect-metadata');
const { Module, Injectable } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const nest = require('@zlink-systems/nestjs');
const zlink = require('@zlink-systems/zlink');
const BODY = 'x'.repeat(1024);
const COUNT = Number(process.env.BP_COUNT ?? 16384);
const DEADLINE_MS = 30_000;
const CORE_BUDGET = BigInt(process.env.BP_CORE_BUDGET ?? 8 * 1024 * 1024);

function pressureMetrics() {
  const callbacks = [];
  const counter = { add() {}, record() {} };
  const observable = (name) => ({
    addCallback(callback) {
      callbacks.push({ name, callback });
    }
  });
  return {
    provider: {
      getMeter: () => ({
        createCounter: () => counter,
        createUpDownCounter: () => counter,
        createHistogram: () => counter,
        createObservableCounter: observable,
        createObservableGauge: observable
      })
    },
    snapshot() {
      const values = [];
      for (const { name, callback } of callbacks) {
        if (name === 'zlink.host.application_job_queue.pressure_transitions') {
          callback({ observe: (value, attributes) => values.push({ ...attributes, value }) });
        }
      }
      return values;
    }
  };
}

async function endpoint() {
  const server = net.createServer();
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  const port = server.address().port;
  await new Promise((resolve) => server.close(resolve));
  return `tcp://127.0.0.1:${port}`;
}

async function until(ready) {
  const deadline = performance.now() + DEADLINE_MS;
  while (!ready()) {
    if (performance.now() >= deadline) throw new Error('readiness deadline expired');
    await immediate();
  }
}

async function frameworkPair(messageFlow) {
  class BenchPayload {
    constructor(body) {
      this.body = body;
    }
  }
  class Echo {
    handle(request) {
      return request;
    }
  }
  Injectable()(Echo);
  const address = await endpoint();
  const meters = [];
  async function host(server) {
    class Host {}
    Module({
      imports: [
        nest.ZLinkModule.forRootFactory({
          inject: [],
          useFactory: () => {
            const builder = nest.zlinkFramework();
            if (messageFlow !== undefined) builder.configureDispatch().messageFlow(messageFlow);
            const meter = pressureMetrics();
            meters.push(meter);
            builder.options({ metrics: { meterProvider: meter.provider } });
            builder.configureInboundDispatch().coreHwmBudgetBytes(CORE_BUDGET);
            const mesh = builder
              .addRouteMesh('bp')
              .listen(server ? address : 'tcp://127.0.0.1:0')
              .routingId(server ? 'bp-server' : 'bp-client');
            if (server) mesh.addRequestHandler('BenchPayload', Echo);
            else mesh.peerConnections().connect('bp-server', address);
            return builder.build();
          }
        })
      ],
      providers: server ? [Echo] : []
    })(Host);
    return NestFactory.createApplicationContext(Host, { logger: false, abortOnError: false });
  }
  const server = await host(true);
  const client = await host(false);
  const topology = client.get(nest.ZLINK_ROUTE_MESH_RUNTIME, { strict: false });
  await until(() => topology.snapshot('bp').readyPeerCount === 1);
  const route = client.get(nest.ZLINK_ROUTE_CLIENT, { strict: false });
  const status = () => ({
    capacity: [client, server].map(
      (app) =>
        app.get(nest.ZLINK_FRAMEWORK_RUNTIME, { strict: false }).status.capacity.applicationJobQueue
    ),
    transitions: meters.map((meter) => meter.snapshot())
  });
  return {
    submit() {
      const reply = route.requestToNode('bp', 'bp-server', new BenchPayload(BODY)).submit();
      return { reply, admitted: reply.admitted };
    },
    validate(reply) {
      assert.equal(reply.body, BODY);
    },
    status,
    close: async () => {
      await client.close();
      await server.close();
    }
  };
}

async function bindingPair() {
  const context = zlink.createContext();
  const serverContext = zlink.createContext();
  context.options.coreHwmBudgetBytes = CORE_BUDGET;
  serverContext.options.coreHwmBudgetBytes = CORE_BUDGET;
  const server = zlink.createRouterSocket(serverContext);
  const client = zlink.createRouterSocket(context);
  const peer = zlink.RoutingId.from(Buffer.from('bp-server'));
  server.setRoutingId(peer);
  client.setRoutingId(zlink.RoutingId.from(Buffer.from('bp-client')));
  client.options.mandatory = true;
  client.options.setConnectRoutingId(peer);
  const address = await endpoint();
  server.bind(address);
  client.connect(address);
  const received = new zlink.Received();
  const poller = zlink.createPoller();
  const events = zlink.createPollEvents(1);
  poller.add(client, [zlink.PollEventFlag.PollCompletion], 0);
  let stopping = false;
  let active = false;
  let wake;
  const pump = (async () => {
    while (!stopping) {
      if (!active) {
        await new Promise((resolve) => {
          wake = resolve;
        });
        continue;
      }
      while (server.recv(received, zlink.RecvFlags.DontWait)) {
        serverReply(received);
      }
      poller.wait(events, 0);
      await immediate();
    }
  })();
  function serverReply(record) {
    const reply = record.reply();
    for (const part of record.parts) reply.message(part);
    reply.submit();
  }
  await until(() => client.routesSnapshot().length > 0);
  const payload = Buffer.from(BODY);
  return {
    begin() {
      active = true;
      wake?.();
      wake = undefined;
    },
    end() {
      active = false;
    },
    submit() {
      const submitted = client
        .request(peer)
        .message(zlink.Message.from(payload))
        .timeout(DEADLINE_MS)
        .submit();
      return {
        reply: submitted.reply,
        admitted: submitted.result === zlink.SubmitResult.Backpressured ? submitted.admitted : null
      };
    },
    validate(parts) {
      try {
        assert.equal(Buffer.from(parts[0].data()).toString(), BODY);
      } finally {
        for (const part of parts) part.close();
      }
    },
    status: () => null,
    close: async () => {
      stopping = true;
      wake?.();
      await pump;
      received.close();
      events.close();
      poller.close();
      client.close();
      server.close();
      context.close();
      serverContext.close();
    }
  };
}

async function run(pair, count, instrument) {
  pair.begin?.();
  let completed = 0;
  let inFlight = 0;
  let peak = 0;
  let admissionWaits = 0;
  let admissionResumes = 0;
  let promises = 0;
  let bufferBytes = 0;
  let buffers = 0;
  const samples = [];
  const pending = [];
  const originalFrom = Buffer.from;
  const hook = createHook({
    init(_id, type) {
      if (type === 'PROMISE') promises++;
    }
  });
  if (instrument) {
    hook.enable();
    Buffer.from = function (...args) {
      const result = originalFrom.apply(this, args);
      buffers++;
      bufferBytes += result.length;
      return result;
    };
  }
  const cpu = process.cpuUsage();
  const start = performance.now();
  const initial = pair.status();
  try {
    for (let index = 0; index < count; index++) {
      const request = pair.submit();
      peak = Math.max(peak, ++inFlight);
      pending.push(
        request.reply.then((value) => {
          try {
            pair.validate(value);
          } finally {
            completed++;
            inFlight--;
          }
        })
      );
      if (request.admitted !== null) {
        // A pending admission across a microtask is an observed wait;
        // immediate settled admission still has a public Promise on Framework.
        let admitted = false;
        request.admitted.then(() => {
          admitted = true;
        });
        await Promise.resolve();
        if (!admitted) admissionWaits++;
        await request.admitted;
        if (!admitted) throw new Error('admission observer did not run');
        admissionResumes = admissionWaits;
      }
      if ((index + 1) % 256 === 0) {
        samples.push({
          submitted: index + 1,
          completed,
          inFlight,
          elapsedMs: performance.now() - start
        });
        await immediate();
      }
    }
    await Promise.all(pending);
    const elapsedMs = performance.now() - start;
    const usage = process.cpuUsage(cpu);
    return {
      count,
      completed,
      peak,
      admissionWaits,
      admissionResumes,
      completionPerSecond: completed / (elapsedMs / 1000),
      cpuNsPerMessage: ((usage.user + usage.system) * 1000) / count,
      promisesPerMessage: instrument ? promises / count : null,
      bufferFromBytesPerMessage: instrument ? bufferBytes / count : null,
      bufferFromCallsPerMessage: instrument ? buffers / count : null,
      initial,
      final: pair.status(),
      samples
    };
  } finally {
    hook.disable();
    Buffer.from = originalFrom;
    pair.end?.();
  }
}

async function main() {
  const framework = await frameworkPair();
  const binding = await bindingPair();
  try {
    await run(framework, 256, false);
    await run(binding, 256, false);
    const rows = [];
    for (let round = 0; round < 3; round++) {
      for (const name of round % 2 === 0 ? ['binding', 'framework'] : ['framework', 'binding']) {
        rows.push({
          round,
          name,
          instrument: false,
          ...(await run(name === 'framework' ? framework : binding, COUNT, false))
        });
      }
    }
    for (const name of ['binding', 'framework']) {
      rows.push({
        name,
        instrument: true,
        ...(await run(name === 'framework' ? framework : binding, COUNT, true))
      });
    }
    const heap = {};
    const session = new Session();
    session.connect();
    const post = (method, parameters) =>
      new Promise((resolve, reject) =>
        session.post(method, parameters, (error, value) => (error ? reject(error) : resolve(value)))
      );
    try {
      for (const name of ['binding', 'framework']) {
        await post('HeapProfiler.startSampling', {
          samplingInterval: 1,
          includeObjectsCollectedByMajorGC: true,
          includeObjectsCollectedByMinorGC: true
        });
        await run(name === 'framework' ? framework : binding, 1024, false);
        const { profile } = await post('HeapProfiler.stopSampling');
        let bytes = 0;
        const sources = {};
        const visit = (node) => {
          bytes += node.selfSize;
          const source = node.callFrame.url;
          sources[source] = (sources[source] ?? 0) + node.selfSize;
          for (const child of node.children) visit(child);
        };
        visit(profile.head);
        heap[name] = {
          sampledBytesPerMessage: bytes / 1024,
          samplesPerMessage: profile.samples.length / 1024,
          sources
        };
      }
    } finally {
      session.disconnect();
    }
    console.log(
      JSON.stringify(
        {
          node: process.version,
          payloadBytes: BODY.length,
          coreBudget: CORE_BUDGET,
          count: COUNT,
          rows,
          heap
        },
        (_key, value) => (typeof value === 'bigint' ? value.toString() : value),
        2
      )
    );
  } finally {
    await binding.close();
    await framework.close();
  }
}

module.exports = { frameworkPair, bindingPair, run };

if (require.main === module) {
  main().catch((error) => {
    console.error(error);
    process.exitCode = 1;
  });
}
