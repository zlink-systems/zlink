import fs from 'node:fs';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { setTimeout as delay } from 'node:timers/promises';

export const sampleName = 'ZoneWorld';

const requiredScenarioIds = [
  'ZW-A1', 'ZW-A2', 'ZW-A3', 'ZW-A4', 'ZW-A5',
  'ZW-B1', 'ZW-B2', 'ZW-B3', 'ZW-B4', 'ZW-B5', 'ZW-B6', 'ZW-B7', 'ZW-B8',
  'ZW-C1', 'ZW-C2', 'ZW-C3', 'ZW-C4',
  'ZW-D1', 'ZW-D2',
  'ZW-E1', 'ZW-E2', 'ZW-E3', 'ZW-E4', 'ZW-E5', 'ZW-E6',
  'ZW-F1', 'ZW-F2', 'ZW-F3', 'ZW-F4',
  'ZW-G1', 'ZW-G2', 'ZW-G3', 'ZW-G4', 'ZW-G5'
];
const logicalZoneIds = ['zone-nw', 'zone-ne', 'zone-sw', 'zone-se'];

export async function runSample(ctx) {
  //  lane은 runner option(--lane)으로만 받는다. 이 sample은 환경 변수로 구성을 바꾸지
  //  않는다 — 구성 입력은 --config <path> 하나다.
  if (ctx.lane === 'b8') {
    await runB8Lane(ctx);
    return;
  }
  await runB8ChildLane(ctx);
  await runFullLane(ctx);
}

async function runFullLane(ctx) {
  const redisKeyPrefix = `zoneworld:node:${process.pid}:`;
  const shared = {
    redisEndpoint: ctx.redisEndpoint,
    redisKeyPrefix,
    logDirectory: ctx.logDir
  };
  const botStartSignalPath = path.join(ctx.logDir, 'bots.start');
  const faultTickSignalPath = path.join(ctx.logDir, 'timer-failure.start');
  const west = await zoneNodeConfig(ctx, shared, 'zone-node-1', 'west', {
    botStartSignalPath,
    faultTickZone: 'zone-nw',
    faultTickSignalPath,
    waitForPlacementPeer: true
  });
  const ops = {
    streamEndpoint: `ws://127.0.0.1:${await ctx.port()}`,
    broadcastEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
    reportEndpoint: `tcp://127.0.0.1:${await ctx.port()}`
  };
  const gateway = {
    streamEndpoint: `ws://127.0.0.1:${await ctx.port()}`,
    spotRouterEndpoint: `tcp://127.0.0.1:${await ctx.port()}`
  };

  const opsPath = ctx.writeConfig('ops', { shared, ops });
  await ctx.start('ops', 'dist/Server/Ops/main.js', ['--config', opsPath]);
  await ctx.waitTcp(ops.streamEndpoint);

  const timerFailure = startScenarioClient(
    ctx,
    specialClientConfig(ctx, shared, gateway, ops, 'C4'),
    'timer-failure'
  );
  await timerFailure.waitFor('scenario ZW-C4 armed');

  await ctx.start('zone-node-1', 'dist/Server/ZoneNode/main.js', ['--config', west.path]);
  await ctx.waitTcp(west.value.zoneNode.spotRouterEndpoint);
  await ctx.waitLog('zone-node-1', 'topology=ready');
  await ctx.waitLog('ops', 'node status received node=zone-node-1');
  const firstLayoutProbe = startScenarioClient(
    ctx,
    specialClientConfig(ctx, shared, gateway, ops, 'LAYOUT'),
    'initial-layout-probe'
  );
  await firstLayoutProbe.waitFor('ops-layout=');
  await firstLayoutProbe.complete();
  const initiallyHosted = parseOpsLayout(firstLayoutProbe.output()).flatMap((node) => node.zones);
  const remainingZones = logicalZoneIds.filter((zoneId) => !initiallyHosted.includes(zoneId));
  if (initiallyHosted.length !== 2 || remainingZones.length !== 2) {
    throw new Error(`First ZoneNode did not fill its capacity before peer bootstrap: ${initiallyHosted.join(',')}.`);
  }
  const east = await zoneNodeConfig(ctx, shared, 'zone-node-2', 'east', {
    botStartSignalPath,
    bootstrapZones: remainingZones,
    faultTickZone: 'zone-nw',
    faultTickSignalPath,
    waitForPlacementPeer: true
  });
  await ctx.start('zone-node-2', 'dist/Server/ZoneNode/main.js', ['--config', east.path]);
  await ctx.waitTcp(east.value.zoneNode.spotRouterEndpoint);
  await ctx.waitLog('zone-node-1', 'topology=ready');
  await ctx.waitLog('zone-node-2', 'topology=ready');
  await ctx.waitLog('ops', 'node status received node=zone-node-1');
  await ctx.waitLog('ops', 'node status received node=zone-node-2');
  await ctx.waitLog('zone-node-1', 'bot-start=ready');
  await ctx.waitLog('zone-node-2', 'bot-start=ready');
  // Arm the injected timer failure only after the report RouteMesh is ready.
  // The sample must exercise timer monitoring, not the expected Unavailable
  // result for a send submitted before its selected route is connected.
  await ctx.waitLog('zone-node-1', 'mesh status node=zone-node-1 state=1');
  fs.writeFileSync(faultTickSignalPath, 'start\n', { mode: 0o600 });

  await timerFailure.waitFor('scenario ZW-C4 passed');
  await timerFailure.complete();
  const verdicts = new Set();
  recordVerdict(verdicts, 'ZW-B8');
  collectVerdicts(verdicts, timerFailure.output());

  const opsProbe = startScenarioClient(
    ctx,
    specialClientConfig(ctx, shared, gateway, ops, 'PAIR'),
    'ops-probe'
  );
  await opsProbe.waitFor('ops-probe=');
  await opsProbe.complete();
  const layout = parseOpsProbe(opsProbe.output());
  assertZoneLayout(layout);
  assertGeneratedRoutingIds(layout.pair.sourceOwnerNodeRid, layout.pair.targetOwnerNodeRid);
  recordVerdict(verdicts, 'ZW-G1');
  const sourceNode = layout.nodes.find((node) => node.zones.includes(layout.pair.sourceZoneId));
  const targetNode = layout.nodes.find((node) => node.zones.includes(layout.pair.targetZoneId));
  if (sourceNode === undefined || targetNode === undefined || sourceNode.nodeId === targetNode.nodeId) {
    throw new Error('Ops probe did not identify distinct source and target ZoneNode owners.');
  }

  const gatewayPath = ctx.writeConfig('gateway', { shared, gateway });
  await ctx.start('gateway', 'dist/Server/Gateway/main.js', ['--config', gatewayPath]);
  await ctx.waitTcp(gateway.streamEndpoint);
  await ctx.waitLog('zone-node-1', 'mesh status node=zone-node-1 state=1');
  await ctx.waitLog('zone-node-2', 'mesh status node=zone-node-2 state=1');
  await ctx.waitLog('gateway', 'gateway mesh status mesh=zoneworld.zones state=1');
  await ctx.waitLog('zone-node-1', 'mesh status node=zone-node-1 state=1 readyPeers=3');
  await ctx.waitLog('zone-node-2', 'mesh status node=zone-node-2 state=1 readyPeers=3');
  recordVerdict(verdicts, 'ZW-G2');

  const clientPath = ctx.writeConfig('client', {
    shared,
    client: { gatewayEndpoint: gateway.streamEndpoint, opsEndpoint: ops.streamEndpoint, scenarios: 'ZW-A1' }
  });
  const mainClient = startScenarioClient(ctx, clientPath, 'main', 'dist/Client/main.js');
  await mainClient.waitFor('scenario ZW-B2 passed');
  // ZW-B7 completes the A->B->A round trip right where ZW-B2's flow ends.
  await mainClient.waitFor('scenario ZW-B7 passed');
  await mainClient.waitFor('scenario ZW-B5 passed');
  await mainClient.waitFor('scenario ZW-B6 passed');
  // Server-side halves of ZW-B6/ZW-B7 the client cannot see: the probes that
  // entered the stale bound route followed to the target owner (exactly once
  // for the one-way half), and the round trip re-entered the original node.
  await ctx.waitLog(sourceNode.nodeId, 'message-follow probe handled actor=player-a1 probe=zw-b6-request');
  await ctx.waitLog(sourceNode.nodeId, 'message-follow probe one-way handled actor=player-a1 probe=zw-b6-one-way');
  await ctx.waitLog(sourceNode.nodeId, `zone player entered zone=${layout.pair.sourceZoneId} player=player-a1 initial=false`);
  await mainClient.complete();
  ctx.assertLogCount(sourceNode.nodeId, 'message-follow probe one-way handled actor=player-a1 probe=zw-b6-one-way', 1);
  process.stdout.write(mainClient.output());
  collectVerdicts(verdicts, mainClient.output());
  await ctx.waitLog('zone-node-1', 'fanout subscriber received announcement');
  await ctx.waitLog('zone-node-2', 'fanout subscriber received announcement');
  for (const node of layout.nodes) {
    for (const zoneId of node.zones) {
      await ctx.waitLog(node.nodeId, `zone spot announcement delivered zone=${zoneId}`);
    }
  }

  const extra = await zoneNodeConfig(ctx, shared, 'zone-node-3', 'extra', { disableBots: true, zoneCapacity: 0 });
  await ctx.start('zone-node-3', 'dist/Server/ZoneNode/main.js', ['--config', extra.path]);
  await ctx.waitLog('zone-node-3', 'topology=ready node=zone-node-3 zones=');
  await ctx.waitLog('zone-node-3', 'fanout subscriber=ready node=zone-node-3');
  ctx.runNode(path.join(ctx.sampleRoot, 'dist/Client/special.js'), [
    '--config', specialClientConfig(ctx, shared, gateway, ops, 'D2')
  ]);
  await ctx.waitLog('zone-node-3', 'fanout subscriber received announcement');
  recordVerdict(verdicts, 'ZW-D2');

  // The common internals do not recreate an existing User Spot after a process
  // restart. Run application operations while their original owners are ready.
  const maintenance = startScenarioClient(
    ctx,
    specialClientConfig(ctx, shared, gateway, ops, 'E'),
    'maintenance'
  );
  await maintenance.complete();
  process.stdout.write(maintenance.output());
  collectVerdicts(verdicts, maintenance.output());

  // Bots are created during startup but remain paused until this signal. This
  // keeps the normal maintenance checks deterministic while using the same
  // owner processes for the bot scenario.
  fs.writeFileSync(botStartSignalPath, 'start\n', { mode: 0o600 });
  await ctx.waitLog('zone-node-1', 'topology=ready');
  await ctx.waitLog('zone-node-2', 'topology=ready');
  await ctx.waitLog('zone-node-1', 'mesh status node=zone-node-1 state=1');
  await ctx.waitLog('zone-node-2', 'mesh status node=zone-node-2 state=1');
  for (const name of ['zone-node-1', 'zone-node-2']) {
    await ctx.waitLog(name, 'bot spawned');
  }
  await waitForCrossOwnerBot(ctx, layout.nodes);
  const botLogs = ['zone-node-1', 'zone-node-2']
    .map((name) => fs.readFileSync(path.join(ctx.logDir, `${name}.log`), 'utf8'))
    .join('\n');
  const spawned = new Set([...botLogs.matchAll(/bot spawned bot=([^ ]+)/g)].map((match) => match[1]));
  if (spawned.size !== 8) throw new Error(`ZW-F1 expected 8 spawned bots, observed ${spawned.size}.`);
  if (/No current session binding exists for actor 'bot-/.test(botLogs)) {
    throw new Error('ZW-F3 attempted to push to an unbound bot actor.');
  }
  recordVerdict(verdicts, 'ZW-F2');
  const bots = startScenarioClient(
    ctx,
    specialClientConfig(ctx, shared, gateway, ops, 'F'),
    'bots'
  );
  await bots.waitFor('scenario ZW-F4 passed');
  await bots.complete();
  process.stdout.write(bots.output());
  collectVerdicts(verdicts, bots.output());

  const transition = startScenarioClient(
    ctx,
    specialClientConfig(ctx, shared, gateway, ops, 'B4-C2-C3'),
    'transition'
  );
  await transition.waitFor('scenario ZW-B4-C2-C3 armed');
  const browser = await startSharedBrowser(ctx, gateway, ops, targetNode.nodeId);
  try {
    await waitForFile(browser.markerPath, 45_000);
    await ctx.stop(targetNode.nodeId, 'SIGKILL');
    await browser.complete();
  } finally {
    await browser.dispose();
  }
  await transition.waitFor('scenario ZW-B4 passed');
  await transition.waitFor('scenario ZW-C2 passed');
  await transition.waitFor('scenario ZW-C3 passed');
  await transition.waitFor('crash-boundary=Unavailable');
  await transition.complete();
  process.stdout.write(transition.output());
  collectVerdicts(verdicts, transition.output());
  const targetAfterFailure = await zoneNodeConfig(ctx, shared, targetNode.nodeId, 'target-after-failure', {
    disableBots: true,
    waitForPlacementPeer: true,
    allowEmptyZoneSet: true
  });
  await ctx.start('target-after-failure', 'dist/Server/ZoneNode/main.js', ['--config', targetAfterFailure.path]);
  await waitForExactLogLine(
    ctx,
    'target-after-failure',
    `topology=ready node=${targetNode.nodeId} zones=`
  );
  await ctx.waitLog('target-after-failure', `mesh status node=${targetNode.nodeId} state=1`);
  const crashRecreation = startScenarioClient(
    ctx,
    specialClientConfig(
      ctx,
      shared,
      gateway,
      ops,
      'G4',
      targetNode.nodeId
    ),
    'crash-recreation'
  );
  await crashRecreation.complete();
  process.stdout.write(crashRecreation.output());
  const crashProof = assertFreshReplacementProof(
    parseFreshActorProofs(crashRecreation.output(), 'G4'),
    targetNode.nodeId,
    layout.pair.sourceOwnerNodeRid,
    layout.pair.targetOwnerNodeRid
  );
  recordVerdict(verdicts, 'ZW-G4');
  const maintenanceArm = startScenarioClient(
    ctx,
    specialClientConfig(ctx, shared, gateway, ops, 'E5-arm', targetNode.nodeId),
    'maintenance-arm'
  );
  await maintenanceArm.complete();
  await stopAndWaitForLocationLease(
    ctx,
    'target-after-failure',
    'SIGTERM'
  );
  const targetAfterMaintenance = await zoneNodeConfig(ctx, shared, targetNode.nodeId, 'target-after-maintenance', {
    disableBots: true,
    waitForPlacementPeer: true,
    allowEmptyZoneSet: true
  });
  await ctx.start('target-after-maintenance', 'dist/Server/ZoneNode/main.js', ['--config', targetAfterMaintenance.path]);
  await waitForExactLogLine(
    ctx,
    'target-after-maintenance',
    `topology=ready node=${targetNode.nodeId} zones=`
  );
  await ctx.waitLog('target-after-maintenance', `maintenance restored node=${targetNode.nodeId} enabled=true`);
  const maintenanceRestore = startScenarioClient(
    ctx,
    specialClientConfig(ctx, shared, gateway, ops, 'E5', targetNode.nodeId),
    'maintenance-restore'
  );
  await maintenanceRestore.complete();
  process.stdout.write(maintenanceRestore.output());
  collectVerdicts(verdicts, maintenanceRestore.output());

  const normalRecreation = startScenarioClient(
    ctx,
    specialClientConfig(
      ctx,
      shared,
      gateway,
      ops,
      'G3',
      targetNode.nodeId
    ),
    'normal-recreation'
  );
  await normalRecreation.complete();
  process.stdout.write(normalRecreation.output());
  assertFreshReplacementProof(
    parseFreshActorProofs(normalRecreation.output(), 'G3'),
    targetNode.nodeId,
    layout.pair.sourceOwnerNodeRid,
    crashProof.nodeRid
  );
  recordVerdict(verdicts, 'ZW-G3');

  await stopAndWaitForLocationLease(
    ctx,
    'target-after-maintenance',
    'SIGTERM'
  );
  recordVerdict(verdicts, 'ZW-G5');
  assertCompleteVerdicts(verdicts);
  console.log('topology=ready');
  console.log('zoneworld-transfer=completed');
  console.log('zoneworld-border-sync=completed');
  console.log('zoneworld-ops-observe=completed');
  console.log('zoneworld-ops-announce=completed');
  console.log('zoneworld-ops-maintenance=completed');
  console.log('zoneworld=completed');
  console.log('PASS ZoneWorld');
}

async function runB8ChildLane(ctx) {
  const runner = path.join(ctx.sampleRoot, process.platform === 'win32' ? 'run_sample.ps1' : 'run_sample.sh');
  if (process.platform === 'win32') {
    await runCommand('powershell', ['-NoProfile', '-File', runner, '-B8Child'], ctx.sampleRoot);
  } else {
    await runCommand('bash', [runner, '--b8-child'], ctx.sampleRoot);
  }
}

async function runB8Lane(ctx) {
  const redisKeyPrefix = `zoneworld:node:b8:${process.pid}:`;
  const shared = {
    redisEndpoint: ctx.redisEndpoint,
    redisKeyPrefix,
    logDirectory: ctx.logDir,
    sessionRelocationSealTimeoutMs: 30_000
  };
  const armFile = path.join(ctx.runDir, 'b8-block-command-44');
  const westRouter = await proxiedRouter(ctx);
  const eastRouter = await proxiedRouter(ctx);
  const gatewayRouter = await proxiedRouter(ctx);
  const ops = {
    streamEndpoint: `ws://127.0.0.1:${await ctx.port()}`,
    broadcastEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
    reportEndpoint: `tcp://127.0.0.1:${await ctx.port()}`
  };
  const gateway = {
    streamEndpoint: `ws://127.0.0.1:${await ctx.port()}`,
    spotRouterEndpoint: gatewayRouter.bindEndpoint,
    spotRouterAdvertiseHost: '127.0.0.1'
  };
  const proxies = [
    startSessionRouteProxy(ctx, 'zone-node-1', westRouter.port, armFile),
    startSessionRouteProxy(ctx, 'zone-node-2', eastRouter.port, armFile),
    startSessionRouteProxy(ctx, 'gateway', gatewayRouter.port, armFile)
  ];
  let b8Client;
  try {
    await Promise.all(proxies.map((proxy) => waitForLogFile(proxy.logPath, 'proxy-ready', 30_000)));
    const opsPath = ctx.writeConfig('ops', { shared, ops });
    await ctx.start('ops', 'dist/Server/Ops/main.js', ['--config', opsPath]);
    await ctx.waitTcp(ops.streamEndpoint);

    const west = await zoneNodeConfig(ctx, shared, 'zone-node-1', 'west', {
      disableBots: true,
      waitForPlacementPeer: true,
      spotRouterEndpoint: westRouter.bindEndpoint,
      spotRouterAdvertiseHost: '127.0.0.1'
    });
    await ctx.start('zone-node-1', 'dist/Server/ZoneNode/main.js', ['--config', west.path]);
    await ctx.waitTcp(west.value.zoneNode.spotRouterEndpoint);
    await ctx.waitLog('zone-node-1', 'topology=ready');
    await ctx.waitLog('ops', 'node status received node=zone-node-1');

    const layoutProbe = startScenarioClient(
      ctx,
      specialClientConfig(ctx, shared, gateway, ops, 'LAYOUT'),
      'b8-layout-probe'
    );
    await layoutProbe.waitFor('ops-layout=');
    await layoutProbe.complete();
    const initiallyHosted = parseOpsLayout(layoutProbe.output()).flatMap((node) => node.zones);
    const remainingZones = logicalZoneIds.filter((zoneId) => !initiallyHosted.includes(zoneId));
    if (initiallyHosted.length !== 2 || remainingZones.length !== 2) {
      throw new Error(`ZW-B8 first ZoneNode did not fill its capacity: ${initiallyHosted.join(',')}.`);
    }

    const east = await zoneNodeConfig(ctx, shared, 'zone-node-2', 'east', {
      disableBots: true,
      bootstrapZones: remainingZones,
      waitForPlacementPeer: true,
      spotRouterEndpoint: eastRouter.bindEndpoint,
      spotRouterAdvertiseHost: '127.0.0.1'
    });
    await ctx.start('zone-node-2', 'dist/Server/ZoneNode/main.js', ['--config', east.path]);
    await ctx.waitTcp(east.value.zoneNode.spotRouterEndpoint);
    await ctx.waitLog('zone-node-1', 'topology=ready');
    await ctx.waitLog('zone-node-2', 'topology=ready');
    await ctx.waitLog('ops', 'node status received node=zone-node-2');

    const pairProbe = startScenarioClient(
      ctx,
      specialClientConfig(ctx, shared, gateway, ops, 'PAIR'),
      'b8-pair-probe'
    );
    await pairProbe.waitFor('ops-probe=');
    await pairProbe.complete();
    const layout = parseOpsProbe(pairProbe.output());
    assertZoneLayout(layout);

    const gatewayPath = ctx.writeConfig('gateway', { shared, gateway });
    await ctx.start('gateway', 'dist/Server/Gateway/main.js', ['--config', gatewayPath]);
    await ctx.waitTcp(gateway.streamEndpoint);
    await ctx.waitLog('gateway', 'gateway mesh status mesh=zoneworld.zones state=1');
    await ctx.waitLog('zone-node-1', 'mesh status node=zone-node-1 state=1 readyPeers=3');
    await ctx.waitLog('zone-node-2', 'mesh status node=zone-node-2 state=1 readyPeers=3');

    const clientPath = ctx.writeConfig('client-b8', {
      shared,
      client: {
        gatewayEndpoint: gateway.streamEndpoint,
        opsEndpoint: ops.streamEndpoint,
        scenarios: 'ZW-B8',
        faultArmFile: armFile
      }
    });
    b8Client = startScenarioClient(ctx, clientPath, 'b8', 'dist/Client/main.js');
    await b8Client.waitFor('scenario ZW-B8 armed actor=');
    const armed = b8Client.output().match(/scenario ZW-B8 armed actor=([^ ]+) target=([^\s]+)/);
    if (armed === null) throw new Error('ZW-B8 client arm evidence was malformed.');
    const [, actorId, targetZoneId] = armed;
    fs.writeFileSync(armFile, 'block\n', { mode: 0o600 });

    await requireB8Evidence(
      b8Client,
      proxies.map((proxy) => proxy.logPath),
      'blocked-command-44',
      'ZW-B8 precondition unmet: fault proxy did not intercept command 44.'
    );
    await requireB8Evidence(
      b8Client,
      ['zone-node-1', 'zone-node-2'].map((name) => path.join(ctx.logDir, `${name}.log`)),
      `zone player entered zone=${targetZoneId} player=${actorId} initial=false`,
      `ZW-B8 precondition unmet: target relocation commit was not observed for actor ${actorId} in ${targetZoneId}.`
    );
    fs.rmSync(armFile, { force: true });
    try {
      await b8Client.complete();
    } catch (error) {
      throw new Error(
        `ZW-B8 post-boundary reconnect did not rebind the existing relocated Actor.\n${String(error)}`
      );
    }
    process.stdout.write(b8Client.output());
    console.log('verdict ZW-B8=passed');
    console.log('PASS ZoneWorld ZW-B8');
  } finally {
    fs.rmSync(armFile, { force: true });
    await b8Client?.dispose();
    for (const proxy of proxies) {
      if (proxy.child.exitCode === null && proxy.child.signalCode === null) proxy.child.kill('SIGKILL');
    }
    await Promise.allSettled(proxies.map((proxy) => proxy.exited));
  }
}

async function proxiedRouter(ctx) {
  const port = await ctx.port();
  return { port, bindEndpoint: `tcp://127.0.0.2:${port}` };
}

function startSessionRouteProxy(ctx, name, port, armFile) {
  const logPath = path.join(ctx.logDir, `session-route-proxy-${name}.log`);
  const processState = startCommand(
    'python3',
    [
      path.join(ctx.sampleRoot, 'Support/session_route_block_proxy.py'),
      '--listen-host', '127.0.0.1', '--listen-port', String(port),
      '--target-host', '127.0.0.2', '--target-port', String(port),
      '--arm-file', armFile
    ],
    ctx.sampleRoot,
    logPath
  );
  return { ...processState, logPath };
}

async function requireB8Evidence(client, logPaths, marker, failure) {
  const deadline = Date.now() + 60_000;
  while (Date.now() < deadline) {
    if (logPaths.some((target) => fs.existsSync(target) && fs.readFileSync(target, 'utf8').includes(marker))) return;
    if (!client.isRunning()) {
      await client.complete().catch(() => undefined);
      throw new Error(failure);
    }
    await delay(50);
  }
  throw new Error(failure);
}

async function waitForLogFile(target, marker, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (fs.existsSync(target) && fs.readFileSync(target, 'utf8').includes(marker)) return;
    await delay(50);
  }
  throw new Error(`Timed out waiting for '${marker}' in ${target}.`);
}

async function zoneNodeConfig(ctx, shared, nodeId, name, overrides = {}) {
  const value = {
    shared,
    zoneNode: {
      nodeId,
      spotRouterEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
      zoneCapacity: 2,
      ...overrides
    }
  };
  return { value, path: ctx.writeConfig(name, value) };
}

function specialClientConfig(ctx, shared, gateway, ops, scenarios, targetNodeId) {
  return ctx.writeConfig(`client-${scenarios.toLowerCase().replaceAll(/[^a-z0-9]+/g, '-')}`, {
    shared,
    client: {
      gatewayEndpoint: gateway.streamEndpoint,
      opsEndpoint: ops.streamEndpoint,
      scenarios,
      ...(targetNodeId === undefined ? {} : { targetNodeId })
    }
  });
}

function startScenarioClient(ctx, configPath, name, relativeExecutable = 'dist/Client/special.js') {
  const executable = path.join(ctx.sampleRoot, relativeExecutable);
  const child = spawn(process.execPath, [executable, '--config', configPath], {
    cwd: ctx.sampleRoot,
    stdio: ['ignore', 'pipe', 'pipe']
  });
  let output = '';
  child.stdout.on('data', (chunk) => { output += chunk.toString(); });
  child.stderr.on('data', (chunk) => { output += chunk.toString(); });
  let failure;
  const exited = new Promise((resolve) => child.once('exit', (code) => {
    if (code !== 0) failure = new Error(`ZoneWorld ${name} client exited with ${code}.\n${output}`);
    resolve();
  }));
  const complete = async () => {
    await exited;
    if (failure !== undefined) throw failure;
  };
  return {
    async waitFor(marker) {
      const deadline = Date.now() + 60_000;
      while (!output.includes(marker)) {
        if (child.exitCode !== null) await complete();
        if (Date.now() >= deadline) throw new Error(`Timed out waiting for '${marker}'.\n${output}`);
        await delay(50);
      }
    },
    complete,
    output: () => output,
    isRunning: () => child.exitCode === null && child.signalCode === null,
    async dispose() {
      if (child.exitCode === null && child.signalCode === null) child.kill('SIGKILL');
      await exited;
    }
  };
}


async function stopAndWaitForLocationLease(ctx, name, signal) {
  await ctx.stop(name, signal);
  // The sample config fixes ownerLeaseTtlMs at 3 seconds.
  await delay(3_100);
}

function collectVerdicts(verdicts, output) {
  for (const match of output.matchAll(/scenario (ZW-(?:[A-G][1-7]|B8)) passed/g)) {
    recordVerdict(verdicts, match[1]);
  }
}

function recordVerdict(verdicts, scenarioId) {
  if (!requiredScenarioIds.includes(scenarioId)) {
    throw new Error(`Unknown ZoneWorld scenario verdict '${scenarioId}'.`);
  }
  if (verdicts.has(scenarioId)) return;
  verdicts.add(scenarioId);
  console.log(`verdict ${scenarioId}=passed`);
}

function assertCompleteVerdicts(verdicts) {
  const missing = requiredScenarioIds.filter((scenarioId) => !verdicts.has(scenarioId));
  if (missing.length > 0) {
    throw new Error(`ZoneWorld completion is missing per-ID verdicts: ${missing.join(', ')}.`);
  }
}

function parseOpsProbe(output) {
  const match = output.match(/^ops-probe=(\{.+\})$/m);
  if (match === null) throw new Error(`ZoneWorld Ops probe output was missing.\n${output}`);
  return JSON.parse(match[1]);
}

function parseOpsLayout(output) {
  const match = output.match(/^ops-layout=(\[.+\])$/m);
  if (match === null) throw new Error(`ZoneWorld initial Ops layout output was missing.\n${output}`);
  return JSON.parse(match[1]);
}

function parseFreshActorProofs(output, expectedScenario) {
  const proofs = [...output.matchAll(/^fresh-actor-proof=(\{.+\})$/gm)]
    .map((match) => JSON.parse(match[1]));
  const matching = proofs.filter((candidate) => candidate.scenario === expectedScenario);
  if (matching.length === 0) {
    throw new Error(`ZoneWorld ${expectedScenario} fresh Actor proofs were missing.\n${output}`);
  }
  return matching;
}

function assertFreshReplacementProof(proofs, expectedNodeId, sourceNodeRid, previousNodeRid) {
  const canonical = /^zn-[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
  const proof = proofs.find((candidate) => candidate.nodeId === expectedNodeId
    && canonical.test(candidate.nodeRid)
    && candidate.nodeRid !== sourceNodeRid
    && candidate.nodeRid !== previousNodeRid
    && /^[1-9][0-9]*$/.test(candidate.objectGeneration));
  if (proof === undefined) {
    throw new Error(
      `Replacement '${expectedNodeId}' did not accept a fresh Actor on a new RID: ${JSON.stringify(proofs)}.`
    );
  }
  return proof;
}

function assertZoneLayout(layout) {
  if (!Array.isArray(layout.nodes) || layout.nodes.length !== 2) {
    throw new Error('Ops probe must report exactly two ZoneNodes.');
  }
  const zones = layout.nodes.flatMap((node) => node.zones);
  if (layout.nodes.some((node) => node.zones.length !== 2) || new Set(zones).size !== 4) {
    throw new Error(`Zone Spot capacity 2 did not produce a 2/2 layout: ${JSON.stringify(layout.nodes)}.`);
  }
  if (layout.pair?.error !== null || layout.pair.sourceZoneId === layout.pair.targetZoneId) {
    throw new Error(`Ops probe did not return a valid cross-owner boundary: ${JSON.stringify(layout.pair)}.`);
  }
}

function assertGeneratedRoutingIds(source, target) {
  const canonical = /^zn-[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
  if (!canonical.test(source) || !canonical.test(target)) {
    throw new Error(`ZW-G1 observed non-canonical ZoneNode RIDs '${source}' and '${target}'.`);
  }
  if (source === target) throw new Error(`ZW-G1 observed the same RID for both ZoneNodes: '${source}'.`);
}

async function waitForCrossOwnerBot(ctx, nodes) {
  const ownerByZone = new Map(nodes.flatMap((node) => node.zones.map((zoneId) => [zoneId, node.nodeId])));
  const deadline = Date.now() + 60_000;
  while (Date.now() < deadline) {
    const logs = nodes.map((node) => {
      const target = path.join(ctx.logDir, `${node.nodeId}.log`);
      return fs.existsSync(target) ? fs.readFileSync(target, 'utf8') : '';
    }).join('\n');
    for (const match of logs.matchAll(/zone change scheduled player=(bot-[^ ]+) from=([^ ]+) to=([^\s]+)/g)) {
      if (ownerByZone.get(match[2]) !== ownerByZone.get(match[3])) return;
    }
    await delay(50);
  }
  throw new Error('ZW-F2 did not observe a bot crossing an Ops-discovered owner boundary.');
}

async function startSharedBrowser(ctx, gateway, ops, lifecycleNodeId) {
  const browserRoot = path.resolve(ctx.nodeRoot, '../shared_sample/zoneworld/client');
  const outputDirectory = path.join(ctx.workDir, 'zoneworld-browser-dist');
  const previewPort = await ctx.port();
  const markerPath = path.join(ctx.runDir, 'browser-lifecycle-armed');
  const playwrightConfig = path.join(ctx.runDir, 'zoneworld-playwright.live.mjs');
  await runCommand('npm', ['exec', 'vite', 'build', '--', '--outDir', outputDirectory], browserRoot);
  fs.writeFileSync(
    path.join(outputDirectory, 'config.json'),
    `${JSON.stringify({ gateway: gateway.streamEndpoint, ops: ops.streamEndpoint })}\n`,
    { mode: 0o600 }
  );
  fs.writeFileSync(playwrightConfig, `export default ${JSON.stringify({
    testDir: path.join(browserRoot, 'tests/live'),
    timeout: 45_000,
    workers: 1,
    use: { baseURL: `http://127.0.0.1:${previewPort}`, headless: true },
    metadata: { lifecycleMarker: markerPath, lifecycleNodeId }
  })}\n`, { mode: 0o600 });
  const preview = startCommand(
    'npm',
    ['exec', 'vite', 'preview', '--', '--host', '127.0.0.1', '--port', String(previewPort), '--outDir', outputDirectory],
    browserRoot,
    path.join(ctx.logDir, 'shared-browser-preview.log')
  );
  await ctx.waitTcp(`tcp://127.0.0.1:${previewPort}`);
  const playwright = startCommand(
    'npm',
    ['exec', 'playwright', 'test', '--', '--config', playwrightConfig],
    browserRoot,
    path.join(ctx.logDir, 'shared-browser-playwright.log')
  );
  return {
    markerPath,
    async complete() {
      const status = await playwright.exited;
      if (status !== 0) throw new Error(`Shared ZoneWorld browser lane exited with ${status}.`);
      console.log('shared-browser=completed');
    },
    async dispose() {
      if (playwright.child.exitCode === null && playwright.child.signalCode === null) playwright.child.kill('SIGKILL');
      if (preview.child.exitCode === null && preview.child.signalCode === null) preview.child.kill('SIGKILL');
      await Promise.allSettled([playwright.exited, preview.exited]);
    }
  };
}

function startCommand(command, args, cwd, logPath) {
  const output = fs.openSync(logPath, 'a');
  const child = spawn(command, args, { cwd, stdio: ['ignore', output, output] });
  fs.closeSync(output);
  const exited = new Promise((resolve) => child.once('exit', (code, signal) => resolve(code ?? (signal ? 1 : 0))));
  return { child, exited };
}

async function runCommand(command, args, cwd) {
  const child = spawn(command, args, { cwd, stdio: 'inherit' });
  const status = await new Promise((resolve) => child.once('exit', (code, signal) => resolve(code ?? (signal ? 1 : 0))));
  if (status !== 0) throw new Error(`Command '${command} ${args.join(' ')}' exited with ${status}.`);
}

async function waitForFile(target, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  while (!fs.existsSync(target)) {
    if (Date.now() >= deadline) throw new Error(`Timed out waiting for '${target}'.`);
    await delay(50);
  }
}

async function waitForExactLogLine(ctx, name, expected) {
  const target = path.join(ctx.logDir, `${name}.log`);
  const deadline = Date.now() + 30_000;
  while (Date.now() < deadline) {
    if (fs.existsSync(target)) {
      const lines = fs.readFileSync(target, 'utf8').split(/\r?\n/);
      if (lines.includes(expected)) return;
    }
    await delay(50);
  }
  throw new Error(`Timed out waiting for exact ${name} log line '${expected}'.`);
}
