const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const net = require('node:net');
const { spawn } = require('node:child_process');
const { createClient } = require('redis');
const { Injectable, Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');

const zlink = require('@zlink-systems/zlink');
const framework = require('../packages/framework/dist');
const nestjs = require('../packages/nestjs/dist');
const { ZLinkRedisLocationStore } = require('../packages/framework-locations-redis/dist');
const repoRoot = path.resolve(__dirname, '../../../..');
const dotnetTestHostProject = path.join(
  repoRoot,
  'framework/languages/dotnet/testapps/Zlink.Framework.TestHost/Zlink.Framework.TestHost.csproj'
);
const dotnetRedisTestsProject = path.join(
  repoRoot,
  'framework/languages/dotnet/tests/Zlink.Framework.Locations.Redis.Tests/Zlink.Framework.Locations.Redis.Tests.csproj'
);

async function main() {
  console.log(await crossLanguageManifest());
  const results = [];
  await runInTempDir(async (tempDir) => {
    results.push(...await runStage('Node client -> dotnet channel server', () => nodeClientToDotnetChannelServer(tempDir)));
    results.push(await runStage('Node publisher -> dotnet fanout subscriber', () => nodePublisherToDotnetFanoutSubscriber(tempDir)));
    results.push(await runStage('dotnet publisher -> Node fanout subscriber', () => dotnetPublisherToNodeFanoutSubscriber(tempDir)));
    results.push(await runStage('dotnet client -> Node channel server', () => dotnetClientToNodeChannelServer(tempDir)));
    results.push(await runStage('Node route client -> dotnet route server', () => nodeRouteClientToDotnetRouteServer(tempDir)));
    results.push(await runStage('dotnet route client -> Node route server', () => dotnetRouteClientToNodeRouteServer(tempDir)));
    results.push(await runStage('Browser TypeScript connector -> dotnet stream server', () => nodeConnectorToDotnetStreamServer(tempDir)));
    results.push(await runStage('dotnet connector -> Node stream server', () => dotnetConnectorToNodeStreamServer(tempDir)));
    results.push(await runStage('dotnet stream drain -> Browser TypeScript connector', () => nodeConnectorObservesDotnetSessionClosing(tempDir)));
    results.push(await runStage('Node stream drain -> dotnet connector', () => dotnetConnectorObservesNodeSessionClosing(tempDir)));
    results.push(...await runStage('Node/dotnet Redis rows', () => nodeDotnetRedisLocationRows(tempDir)));
  });

  for (const result of results) {
    console.log(`ok - ${result}`);
  }
}

async function crossLanguageManifest() {
  const nodeFrameworkPackage = JSON.parse(await fs.readFile(
    path.join(__dirname, '../packages/framework/package.json'), 'utf8'));
  const nodeBindingPackage = JSON.parse(await fs.readFile(
    path.join(__dirname, '../node_modules/@zlink-systems/zlink/package.json'), 'utf8'));
  const dotnetBuildProps = await fs.readFile(
    path.join(repoRoot, 'framework/languages/dotnet/Directory.Build.props'), 'utf8');
  const dotnetVersion = dotnetBuildProps.match(/<VersionPrefix>([^<]+)<\/VersionPrefix>/)?.[1] ?? 'source-tree';
  return [
    'cross-manifest',
    `node-framework=${nodeFrameworkPackage.version}`,
    `node-binding=${nodeBindingPackage.version}`,
    `dotnet-framework=${dotnetVersion}`,
    'topology=direct-channel,fanout,route-mesh,stream,redis-store',
    'payload=TestHostProfileRequest,TestHostPublishedEvent,TestHostRouteRequest,RawPing',
    'codec=typed-json,stream-json,opaque-store-golden',
    'store-key=per-run-zlink:cross:node:<pid>:<timestamp>'
  ].join(' ');
}

async function createBrowserConnectorDriver() {
  const module = await import('../scripts/browser-e2e/connector-driver.mjs');
  return await module.createBrowserConnectorDriver();
}

async function nodeDotnetRedisLocationRows(tempDir) {
  const redis = await startRedisContainer();
  const prefix = `zlink:cross:node:${process.pid}:${Date.now()}`;
  try {
    await nodeOpaqueStoreRoundTrip(redis.endpoint, `${prefix}:node`);
    await runDotnetRedisProviderTest(
      'FullyQualifiedName~RedisProviderSmokeTests.Dotnet_Opaque_Store_Round_Trip',
      redis.endpoint,
      prefix,
      tempDir
    );
    return [
      'Node Redis opaque Location Store round trip',
      'dotnet Redis opaque Location Store round trip'
    ];
  } finally {
    await removeRedisPrefix(redis.endpoint, prefix);
    await redis.stop();
  }
}

async function nodeOpaqueStoreRoundTrip(redisEndpoint, keyPrefix) {
  const store = new ZLinkRedisLocationStore({
    url: `redis://${redisEndpoint}`,
    keyPrefix
  });
  const alpha = opaqueStoreKey('golden/node/alpha');
  const beta = opaqueStoreKey('golden/node/beta');
  try {
    const result = await store.write({
      conditions: [
        { kind: 'missing', key: alpha },
        { kind: 'missing', key: beta }
      ],
      mutations: [
        { kind: 'put', key: alpha, bytes: Uint8Array.from([0, 1, 255]) },
        { kind: 'put', key: beta, bytes: Buffer.from('node-opaque-value') }
      ]
    });
    assert.equal(result.kind, 'applied');

    const alphaRead = await store.read(alpha);
    assert.equal(alphaRead.kind, 'found');
    assert.deepEqual([...alphaRead.value.bytes], [0, 1, 255]);
    const betaRead = await store.read(beta);
    assert.equal(betaRead.kind, 'found');
    assert.equal(Buffer.from(betaRead.value.bytes).toString(), 'node-opaque-value');

    const conflict = await store.write({
      conditions: [{ kind: 'missing', key: alpha }],
      mutations: [{ kind: 'put', key: alpha, bytes: Buffer.from('must-not-commit') }]
    });
    assert.equal(conflict.kind, 'conflict');
    assert.deepEqual([...((await store.read(alpha)).value.bytes)], [0, 1, 255]);
  } finally {
    await store.dispose();
  }
}

function opaqueStoreKey(value) {
  return { value };
}

async function nodeClientToDotnetChannelServer(tempDir) {
  const port = await reservePort();
  const localPort = await reservePort();
  const endpoint = `tcp://127.0.0.1:${port}`;
  const localEndpoint = `tcp://127.0.0.1:${localPort}`;
  const eventFile = path.join(tempDir, 'node-client-dotnet-channel.events');
  const host = startDotnetHost(tempDir, 'node-client-dotnet-channel', [
    'channel-server',
    '--channel-name', 'profiles',
    '--server-endpoint', endpoint,
    '--event-file', eventFile
  ]);
  class TestHostProfileRequest {
    constructor(value) { this.value = value; }
  }
  class TestHostProfileSend {
    constructor(value) { this.value = value; }
  }
  class ClientModule {}
  Module({
    imports: [nestjs.ZLinkModule.forRootFactory({
      useFactory: () => {
        const builder = nestjs.zlinkFramework();
        const mesh = builder.addRouteMesh('profiles').listen(localEndpoint);
        mesh.channel('profiles').client();
        mesh.peerConnections().connect(rid('dotnet-channel-server'), endpoint);
        return builder.build();
      }
    })]
  })(ClientModule);
  let app;

  try {
    await host.ready;
    app = await NestFactory.createApplicationContext(ClientModule, { logger: false, abortOnError: false });
    const routeMeshRuntime = app.get(nestjs.ZLINK_ROUTE_MESH_RUNTIME, { strict: false });
    try {
      await waitForCondition(
        () => routeMeshRuntime.snapshot('profiles').channels.some(
          channel => channel.channelName === 'profiles' && channel.isReady
        ),
        7000,
        'Node RouteMesh profiles channel readiness'
      );
    } catch (error) {
      error.message += `\nNode RouteMesh snapshot:\n${JSON.stringify(
        routeMeshRuntime.snapshot('profiles'),
        (_key, value) => typeof value === 'bigint' ? value.toString() : value
      )}\nDotnet host output:\n${host.output()}`;
      throw error;
    }
    const client = app.get(nestjs.ZLINK_ROUTE_CLIENT, { strict: false });
    let reply;
    try {
      reply = await withTimeout(
        client
          .requestToChannel('profiles', new TestHostProfileRequest('node-to-dotnet'))
          .timeout(5000)
          .submit(),
        7000,
        'Node client -> dotnet channel server'
      );
    } catch (error) {
      error.message += `\nDotnet host output:\n${host.output()}`;
      throw error;
    }

    assert.deepEqual(reply, { value: 'node-to-dotnet' });
    await client
      .sendToChannel('profiles', new TestHostProfileSend('node-send-to-dotnet'))
      .submit();
    await waitForFileText(eventFile, (text) => text.includes('channel-server-send|node-send-to-dotnet'), 7000);
    return [
      'Node client -> dotnet channel server request/reply',
      'Node client -> dotnet channel server one-way send'
    ];
  } finally {
    await app?.close();
    await host.stop();
  }
}

async function nodePublisherToDotnetFanoutSubscriber(tempDir) {
  const port = await reservePort();
  const endpoint = `tcp://127.0.0.1:${port}`;
  const eventFile = path.join(tempDir, 'node-publisher-dotnet-subscriber.events');
  class TestHostPublishedEvent {
    constructor(value) { this.value = value; }
  }
  class PublisherModule {}
  Module({
    imports: [nestjs.ZLinkModule.forRootFactory({
      useFactory: () => nestjs.zlinkFramework()
        .addFanoutChannel('profiles')
          .enablePublisher(endpoint)
        .build()
    })]
  })(PublisherModule);
  let app;

  try {
    app = await NestFactory.createApplicationContext(PublisherModule, { logger: false, abortOnError: false });
    const publisher = app.get(nestjs.ZLINK_FANOUT_CLIENT, { strict: false });
    const host = startDotnetHost(tempDir, 'node-publisher-dotnet-subscriber', [
      'channel-subscriber',
      '--channel-name', 'profiles',
      '--publisher-endpoint', endpoint,
      '--event-file', eventFile
    ]);
    try {
      await host.ready;
      await publishUntilFileText(
        () => publisher.publish('profiles', new TestHostPublishedEvent('node-publish-to-dotnet')).submit(),
        eventFile,
        (text) => text.includes('node-publish-to-dotnet'),
        7000
      );
    } finally {
      await host.stop();
    }
    return 'Node publisher -> dotnet fanout subscriber';
  } finally {
    await app?.close();
  }
}

async function dotnetPublisherToNodeFanoutSubscriber(tempDir) {
  const port = await reservePort();
  const endpoint = `tcp://127.0.0.1:${port}`;
  const topic = 'profile.changed';
  const received = [];
  class TestHostPublishedEventHandler {
    async handle(event, context) {
      received.push(`${context.topic}:${event?.Value ?? event?.value}`);
    }
  }
  Injectable()(TestHostPublishedEventHandler);
  class SubscriberModule {}
  Module({
    imports: [nestjs.ZLinkModule.forRootFactory({
      useFactory: () => {
        const builder = nestjs.zlinkFramework();
        builder.addFanoutChannel('profiles')
          .enableSubscriber(endpoint)
          .addPublishHandler('TestHostPublishedEvent', TestHostPublishedEventHandler);
        return builder.build();
      }
    })],
    providers: [TestHostPublishedEventHandler]
  })(SubscriberModule);
  let app;
  let host;
  try {
    app = await NestFactory.createApplicationContext(SubscriberModule, { logger: false, abortOnError: false });
    host = startDotnetHost(tempDir, 'dotnet-publisher-node-subscriber', [
      'channel-publisher',
      '--channel-name', 'profiles',
      '--publisher-endpoint', endpoint,
      '--publish-topic', topic,
      '--publish-value', 'dotnet-publish-to-node'
    ]);
    await host.ready;
    await waitForCondition(
      () => received.includes(`${topic}:dotnet-publish-to-node`),
      7000,
      `Node subscriber events were '${received.join(', ')}'`
    );
    return 'dotnet publisher -> Node fanout subscriber';
  } finally {
    await host?.stop();
    await app?.close();
  }
}

async function dotnetClientToNodeChannelServer(tempDir) {
  const port = await reservePort();
  const endpoint = `tcp://127.0.0.1:${port}`;
  const eventFile = path.join(tempDir, 'dotnet-client-node-channel.events');
  class TestHostProfileRequestHandler {
    async handle(payload) {
      const value = payload?.Value ?? payload?.value;
      return { Value: `${value}|node` };
    }
  }
  Injectable()(TestHostProfileRequestHandler);
  class ServerModule {}
  Module({
    imports: [nestjs.ZLinkModule.forRootFactory({
      useFactory: () => {
        const builder = nestjs.zlinkFramework();
        builder.addRouteMesh('profiles')
          .listen(endpoint)
          .channel('profiles').server()
          .addRequestHandler('TestHostProfileRequest', TestHostProfileRequestHandler);
        return builder.build();
      }
    })],
    providers: [TestHostProfileRequestHandler]
  })(ServerModule);
  let app;

  try {
    app = await NestFactory.createApplicationContext(ServerModule, { logger: false, abortOnError: false });
    const host = startDotnetHost(tempDir, 'dotnet-client-node-channel', [
      'channel-client',
      '--channel-name', 'profiles',
      '--server-endpoint', endpoint,
      '--event-file', eventFile,
      '--publish-value', 'dotnet-to-node'
    ]);
    try {
      await host.ready;
      await waitForFileText(eventFile, (text) => text.includes('channel-client|dotnet-to-node|node'), 7000);
    } finally {
      await host.stop();
    }
    return 'dotnet client -> Node channel server';
  } finally {
    await app?.close();
  }
}

async function nodeRouteClientToDotnetRouteServer(tempDir) {
  const port = await reservePort();
  const endpoint = `tcp://127.0.0.1:${port}`;
  const eventFile = path.join(tempDir, 'node-route-dotnet-route.events');
  class TestHostRouteRequest {
    constructor(value) { this.value = value; }
  }
  class RouteClientModule {}
  Module({
    imports: [nestjs.ZLinkModule.forRootFactory({
      useFactory: () => {
        const builder = nestjs.zlinkFramework();
        const mesh = builder.addRouteMesh('cross.route')
          .listen('inproc://cross-route-node-client')
          .routingId('node-route-client');
        mesh.channel('cross.route').server();
        mesh.peerConnections().connect(endpoint);
        return builder.build();
      }
    })]
  })(RouteClientModule);
  let app;
  let host;
  try {
    host = startDotnetHost(tempDir, 'node-route-dotnet-route', [
      'route-server', '--channel-name', 'cross.route', '--server-endpoint', endpoint, '--event-file', eventFile
    ]);
    await host.ready;
    app = await NestFactory.createApplicationContext(RouteClientModule, { logger: false, abortOnError: false });
    const client = app.get(nestjs.ZLINK_ROUTE_CLIENT, { strict: false });
    const reply = await client.requestToNode('cross.route', 'dotnet-route', new TestHostRouteRequest('node-route-to-dotnet'))
      .timeout(5000).submit();
    assert.deepEqual(reply, { value: 'node-route-to-dotnet|dotnet' });
    await waitForFileText(eventFile, (text) => text.includes('route-server|node-route-to-dotnet|'), 7000);
    return 'Node route client -> dotnet route server request/reply';
  } finally {
    await app?.close();
    await host?.stop();
  }
}

async function dotnetRouteClientToNodeRouteServer(tempDir) {
  const port = await reservePort();
  const endpoint = `tcp://127.0.0.1:${port}`;
  const eventFile = path.join(tempDir, 'dotnet-route-node-route.events');
  class TestHostRouteRequestHandler {
    async handle(payload) {
      const value = payload?.Value ?? payload?.value;
      return { Value: `${value}|node` };
    }
  }
  Injectable()(TestHostRouteRequestHandler);
  class RouteServerModule {}
  Module({
    imports: [nestjs.ZLinkModule.forRootFactory({
      useFactory: () => {
        const builder = nestjs.zlinkFramework();
        builder.addRouteMesh('cross.route')
          .listen(endpoint)
          .routingId('node-route')
          .addRequestHandler('TestHostRouteRequest', TestHostRouteRequestHandler)
          .channel('cross.route').server();
        return builder.build();
      }
    })],
    providers: [TestHostRouteRequestHandler]
  })(RouteServerModule);
  let app;
  let host;
  try {
    app = await NestFactory.createApplicationContext(RouteServerModule, { logger: false, abortOnError: false });
    host = startDotnetHost(tempDir, 'dotnet-route-node-route', [
      'route-client', '--channel-name', 'cross.route', '--server-endpoint', endpoint,
      '--event-file', eventFile, '--publish-value', 'dotnet-route-to-node'
    ]);
    await host.ready;
    await waitForFileText(eventFile, (text) => text.includes('route-client|dotnet-route-to-node|node'), 7000);
    return 'dotnet route client -> Node route server request/reply';
  } finally {
    await host?.stop();
    await app?.close();
  }
}

async function nodeConnectorToDotnetStreamServer(tempDir) {
  const port = await reservePort();
  const endpoint = `ws://127.0.0.1:${port}`;
  const eventFile = path.join(tempDir, 'node-connector-dotnet-stream.events');
  const host = startDotnetHost(tempDir, 'node-connector-dotnet-stream', [
    'stream-raw',
    '--stream-endpoint', endpoint,
    '--event-file', eventFile
  ]);
  const instance = await createBrowserConnectorDriver();

  try {
    await host.ready;
    await instance.connect(endpoint);
    const reply = await withTimeout(
      instance.request('RawPing', 'ping', true),
      7000,
      'Browser TypeScript connector -> dotnet stream server'
    );
    assert.equal(reply, 'pong');

    await waitForFileText(eventFile, (text) => text.includes('raw|ping'), 5000);
    await assertFlowLog(`${eventFile}.flow`, 'RawPing', 'Browser TypeScript -> dotnet');
    return 'Browser TypeScript connector -> dotnet stream server flow-wire and JSON codec';
  } finally {
    await instance.close();
    await host.stop();
  }
}

async function dotnetConnectorToNodeStreamServer(tempDir) {
  const port = await reservePort();
  const endpoint = `tcp://127.0.0.1:${port}`;
  const eventFile = path.join(tempDir, 'dotnet-connector-node-stream.events');
  const flowFile = path.join(tempDir, 'dotnet-connector-node-stream.flow');
  const streamEvents = [];
  class NodeStreamSessionFactory {
    async create(sessionContext) {
      return {
        context: sessionContext,
        async onDispatch(_dispatch, payload) {
          const value = payload.decode();
          streamEvents.push(`dispatch:${value}`);
          assert.equal(value, 'dotnet-to-node');
          sessionContext.client.reply('node-pong').compress().submit();
          streamEvents.push('reply:sent');
        }
      };
    }
  }
  Injectable()(NodeStreamSessionFactory);
  class StreamServerModule {}
  Module({
    imports: [nestjs.ZLinkModule.forRootFactory({
      useFactory: () => {
        const builder = nestjs.zlinkFramework();
        builder.configureDispatch()
          .messageFlow(framework.ZLinkMessageFlowLogMode.KeyTransitions)
          .traceLogFile(flowFile)
          .traceLabel('node-test-host');
        builder.addStreamNode('cross-language-stream')
          .bind(endpoint)
          .registerSession(NodeStreamSessionFactory);
        return builder.build();
      }
    })],
    providers: [NodeStreamSessionFactory]
  })(StreamServerModule);
  let app;

  try {
    app = await NestFactory.createApplicationContext(StreamServerModule, { logger: false, abortOnError: false });
    const host = startDotnetHost(tempDir, 'dotnet-connector-node-stream', [
      'stream-client',
      '--stream-endpoint', endpoint,
      '--event-file', eventFile,
      '--publish-value', 'dotnet-to-node'
    ]);
    try {
      await host.ready;
      await waitForFileText(eventFile, (text) => text.includes('stream-client|') && text.includes('node-pong'), 7000);
      await assertFlowLog(flowFile, 'RawPing', 'dotnet -> Node');
    } catch (error) {
      if (streamEvents.length > 0) {
        error.message += `\nNode stream events: ${streamEvents.join(', ')}`;
      }
      throw error;
    } finally {
      try {
        await host.stop();
      } catch (error) {
        if (streamEvents.length > 0) {
          error.message += `\nNode stream events: ${streamEvents.join(', ')}`;
        }
        throw error;
      }
    }
    return 'dotnet connector -> Node stream server flow-wire and JSON codec';
  } finally {
    await app?.close();
  }
}

async function nodeConnectorObservesDotnetSessionClosing(tempDir) {
  const port = await reservePort();
  const endpoint = `ws://127.0.0.1:${port}`;
  const eventFile = path.join(tempDir, 'dotnet-drain-node-connector.events');
  const host = startDotnetHost(tempDir, 'dotnet-drain-node-connector', [
    'stream-raw',
    '--stream-endpoint', endpoint,
    '--event-file', eventFile
  ]);
  const instance = await createBrowserConnectorDriver();
  try {
    await host.ready;
    await instance.connect(endpoint);
    const reply = await instance.request('RawPing', 'drain-probe');
    assert.equal(reply, 'pong');
    await waitForFileText(eventFile, (text) => text.includes('connected|'), 5000);
    await host.stop();
    await instance.waitForCloseReason('ServerDrain', 7000);
    return 'dotnet stream server drain -> Browser TypeScript connector session-closing';
  } finally {
    await instance.close();
    await host.stop();
  }
}

async function dotnetConnectorObservesNodeSessionClosing(tempDir) {
  const port = await reservePort();
  const endpoint = `tcp://127.0.0.1:${port}`;
  const eventFile = path.join(tempDir, 'node-drain-dotnet-connector.events');
  class NodeStreamSessionFactory {
    async create(sessionContext) {
      return {
        context: sessionContext,
        async onDispatch(_dispatch, payload) {
          assert.equal(payload.decode(), 'node-drain-probe');
          sessionContext.client.reply('node-drain-pong').compress().submit();
        }
      };
    }
  }
  Injectable()(NodeStreamSessionFactory);
  class StreamServerModule {}
  Module({
    imports: [nestjs.ZLinkModule.forRootFactory({
      useFactory: () => {
        const builder = nestjs.zlinkFramework();
        builder.addStreamNode('cross-language-drain-stream')
          .bind(endpoint)
          .registerSession(NodeStreamSessionFactory);
        return builder.build();
      }
    })],
    providers: [NodeStreamSessionFactory]
  })(StreamServerModule);
  let app;
  let host;
  try {
    app = await NestFactory.createApplicationContext(StreamServerModule, { logger: false, abortOnError: false });
    host = startDotnetHost(tempDir, 'node-drain-dotnet-connector', [
      'stream-client',
      '--stream-endpoint', endpoint,
      '--event-file', eventFile,
      '--publish-value', 'node-drain-probe'
    ]);
    await host.ready;
    await waitForFileText(eventFile, (text) => text.includes('stream-client|') && text.includes('node-drain-pong'), 7000);
    const runtime = app.get(nestjs.ZLINK_FRAMEWORK_RUNTIME, { strict: false });
    const shutdownResult = await runtime.shutdown({ deadlineMs: 5000 });
    assert.equal(
      shutdownResult.outcome,
      framework.ZLinkFrameworkTerminationOutcome.Stopped
    );
    await app.close();
    app = undefined;
    await waitForFileText(eventFile, (text) => text.includes('stream-disconnected|ServerDrain'), 7000);
    return 'Node stream server drain -> dotnet connector session-closing';
  } finally {
    await host?.stop();
    await app?.close();
  }
}

function startDotnetHost(tempDir, name, args) {
  const readyFile = path.join(tempDir, `${name}.ready.json`);
  const stopFile = path.join(tempDir, `${name}.stop`);
  const child = spawn('dotnet', [
    'run',
    '--project', dotnetTestHostProject,
    '--framework', 'net8.0',
    ...(process.env.ZLINK_DOTNET_TESTHOST_NO_BUILD === '1' ? ['--no-build'] : []),
    '--',
    '--ready-file', readyFile,
    '--stop-file', stopFile,
    ...args
  ], {
    cwd: repoRoot,
    stdio: ['ignore', 'pipe', 'pipe']
  });
  const output = [];
  child.stdout.setEncoding('utf8');
  child.stderr.setEncoding('utf8');
  child.stdout.on('data', (chunk) => output.push(chunk));
  child.stderr.on('data', (chunk) => output.push(chunk));

  const exit = new Promise((resolve) => {
    child.on('exit', (code, signal) => resolve({ code, signal }));
  });

  return {
    ready: waitForReadyFile(readyFile, exit, output, 30000),
    output: () => output.join(''),
    async stop() {
      if (child.exitCode !== null) {
        return;
      }
      await fs.writeFile(stopFile, 'STOP');
      const result = await withTimeout(exit, 10000, `stop ${name}`);
      if (result.code !== 0) {
        throw new Error(`${name} exited with ${result.code ?? result.signal}\n${output.join('')}`);
      }
    }
  };
}

async function waitForReadyFile(readyFile, exit, output, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try {
      await fs.access(readyFile);
      return;
    } catch {
      const exited = await pollExit(exit);
      if (exited !== undefined) {
        throw new Error(`dotnet test host exited before ready: ${exited.code ?? exited.signal}\n${output.join('')}`);
      }
      await new Promise((resolve) => setTimeout(resolve, 50));
    }
  }
  throw new Error(`dotnet test host did not become ready\n${output.join('')}`);
}

async function waitForFileText(filePath, predicate, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  let lastText = '';
  while (Date.now() < deadline) {
    try {
      const text = await fs.readFile(filePath, 'utf8');
      lastText = text;
      if (predicate(text)) {
        return text;
      }
    } catch {}
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  throw new Error(`expected event text did not appear in ${filePath}\nLast text:\n${lastText}`);
}

async function waitForCondition(predicate, timeoutMs, message) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (predicate()) return;
    await new Promise((resolve) => setTimeout(resolve, 25));
  }
  throw new Error(message);
}

async function assertFlowLog(filePath, packetName, direction) {
  const line = await waitForFileText(
    filePath,
    (text) => text.split(/\r?\n/).some((entry) => entry.includes(`packet=${packetName} `)),
    5000
  );
  const packetLine = line.split(/\r?\n/).find((entry) => entry.includes(`packet=${packetName} `));
  assert.match(packetLine, /flow=[0-9a-f]{8}-[0-9a-f]{4}-7[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}/,
    `${direction} flow id must be UUIDv7.`);
  assert.match(packetLine, /origin=(application|inbound)/i,
    `${direction} flow origin must use the shared wire enum.`);
}

async function publishUntilFileText(publish, filePath, predicate, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  let lastText = '';
  while (Date.now() < deadline) {
    await publish();
    try {
      lastText = await fs.readFile(filePath, 'utf8');
      if (predicate(lastText)) return lastText;
    } catch {}
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  throw new Error(`expected event text did not appear in ${filePath}\nLast text:\n${lastText}`);
}

async function waitForMonitorEvent(monitor, eventType, timeoutMs, label) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const event = monitor.recv(zlink.RecvFlags.DontWait);
    if (event !== null && event.event === eventType) {
      return event;
    }
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  throw new Error(`${label} monitor event timed out`);
}

async function runStage(label, operation) {
  try {
    return await operation();
  } catch (error) {
    throw new Error(`${label} failed`, { cause: error });
  }
}

async function pollExit(exit) {
  const marker = Symbol('running');
  const result = await Promise.race([
    exit,
    Promise.resolve(marker)
  ]);
  return result === marker ? undefined : result;
}

async function reservePort() {
  const server = net.createServer();
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  const { port } = server.address();
  await new Promise((resolve, reject) => server.close((error) => error ? reject(error) : resolve()));
  return port;
}

async function runInTempDir(callback) {
  const tempDir = await fs.mkdtemp(path.join(os.tmpdir(), 'zlink-node-cross-'));
  try {
    await callback(tempDir);
  } finally {
    await fs.rm(tempDir, { recursive: true, force: true });
  }
}

async function startRedisContainer() {
  const name = `zlink-node-dotnet-location-${process.pid}-${Date.now()}`;
  const containerId = (await runProcess('docker', [
    'run', '-d', '--rm', '--tmpfs', '/data',
    '--name', name,
    '-p', '127.0.0.1::6379',
    'redis:7.2-alpine'
  ], { cwd: repoRoot })).trim();
  const portLine = (await runProcess('docker', ['port', containerId, '6379/tcp'], { cwd: repoRoot })).trim();
  const port = portLine.split(':').at(-1);
  const endpoint = `127.0.0.1:${port}`;
  await waitTcp(endpoint, 10000);
  return {
    endpoint,
    async stop() {
      await runProcess('docker', ['rm', '-f', '-v', containerId], { cwd: repoRoot, allowFailure: true });
    }
  };
}

async function removeRedisPrefix(endpoint, prefix) {
  const client = createClient({ url: `redis://${endpoint}` });
  await client.connect();
  try {
    for await (const keys of client.scanIterator({ MATCH: `${prefix}:*`, COUNT: 100 })) {
      if (keys.length > 0) await client.del(keys);
    }
  } finally {
    await client.quit();
  }
}

async function runDotnetRedisProviderTest(filter, redisEndpoint, prefix, tempDir) {
  await runProcess('dotnet', [
    'test',
    dotnetRedisTestsProject,
    '--framework', 'net8.0',
    '--no-restore',
    '--filter', filter,
    '-m:1',
    '-p:UseSharedCompilation=false',
    '-p:BuildInParallel=false',
    '--logger', `trx;LogFileName=${path.basename(filter).replace(/[^A-Za-z0-9_.-]/g, '_')}.trx`,
    '--results-directory', tempDir
  ], {
    cwd: repoRoot,
    env: {
      ...process.env,
      ZLINK_REDIS_TEST_ENDPOINT: redisEndpoint,
      ZLINK_REDIS_CROSS_LANGUAGE_PREFIX: prefix
    }
  });
}

async function runProcess(command, args, options = {}) {
  const child = spawn(command, args, {
    cwd: options.cwd ?? repoRoot,
    env: options.env ?? process.env,
    stdio: ['ignore', 'pipe', 'pipe']
  });
  const chunks = [];
  child.stdout.setEncoding('utf8');
  child.stderr.setEncoding('utf8');
  child.stdout.on('data', (chunk) => chunks.push(chunk));
  child.stderr.on('data', (chunk) => chunks.push(chunk));
  const result = await new Promise((resolve) => {
    child.on('exit', (code, signal) => resolve({ code, signal }));
  });
  const output = chunks.join('');
  if (result.code !== 0 && options.allowFailure !== true) {
    throw new Error(`${command} ${args.join(' ')} failed with ${result.code ?? result.signal}\n${output}`);
  }
  return output;
}

async function waitTcp(endpoint, timeoutMs) {
  const [host, portText] = endpoint.split(':');
  const port = Number(portText);
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (await canConnectTcp(host, port)) {
      return;
    }
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  throw new Error(`Redis did not accept TCP connections at ${endpoint}`);
}

function canConnectTcp(host, port) {
  return new Promise((resolve) => {
    const socket = net.createConnection({ host, port });
    socket.once('connect', () => {
      socket.destroy();
      resolve(true);
    });
    socket.once('error', () => {
      socket.destroy();
      resolve(false);
    });
  });
}

function rid(value) {
  return zlink.RoutingId.from(value);
}

function withTimeout(promise, timeoutMs, label) {
  let timeout;
  const guard = new Promise((_, reject) => {
    timeout = setTimeout(() => reject(new Error(`${label} timed out after ${timeoutMs}ms`)), timeoutMs);
  });
  return Promise.race([promise, guard]).finally(() => clearTimeout(timeout));
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
