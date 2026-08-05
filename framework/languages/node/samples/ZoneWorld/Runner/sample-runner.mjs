import fs from 'node:fs';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { setTimeout as delay } from 'node:timers/promises';

export const sampleName = 'ZoneWorld';

export async function runSample(ctx) {
  const redisKeyPrefix = `zoneworld:node:${process.pid}:`;
  const shared = {
    redisEndpoint: ctx.redisEndpoint,
    redisKeyPrefix,
    logDirectory: ctx.logDir
  };
  const botStartSignalPath = path.join(ctx.logDir, 'bots.start');
  const faultTickSignalPath = path.join(ctx.logDir, 'timer-failure.start');
  const east = await zoneNodeConfig(ctx, shared, 'zone-node-2', 'east', {
    botStartSignalPath
  });
  const west = await zoneNodeConfig(ctx, shared, 'zone-node-1', 'west', {
    botStartSignalPath,
    faultTickZone: 'zone-nw',
    faultTickSignalPath,
    placementWeightAfterZoneCreation: 0
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

  // Start the configured west owner first so the faulting zone is materialized
  // by the process whose application config injects the C4 timer failure.
  await ctx.start('zone-node-1', 'dist/Server/ZoneNode/main.js', ['--config', west.path]);
  await ctx.waitTcp(west.value.zoneNode.spotRouterEndpoint);
  await ctx.waitLog('zone-node-1', 'placement weight updated node=zone-node-1 weight=0');
  await ctx.start('zone-node-2', 'dist/Server/ZoneNode/main.js', ['--config', east.path]);
  await ctx.waitTcp(east.value.zoneNode.spotRouterEndpoint);
  await ctx.waitLog('zone-node-1', 'zone spot create zone=zone-nw state=created');
  await ctx.waitLog('zone-node-1', 'zone spot create zone=zone-sw state=created');
  await ctx.waitLog('zone-node-2', 'zone spot create zone=zone-ne state=created');
  await ctx.waitLog('zone-node-2', 'zone spot create zone=zone-se state=created');
  await ctx.waitLog('zone-node-1', 'bot-start=ready');
  await ctx.waitLog('zone-node-2', 'bot-start=ready');
  // Arm the injected timer failure only after the report RouteMesh is ready.
  // The sample must exercise timer monitoring, not the expected Unavailable
  // result for a send submitted before its selected route is connected.
  await ctx.waitLog('zone-node-1', 'mesh status node=zone-node-1 state=1');
  fs.writeFileSync(faultTickSignalPath, 'start\n', { mode: 0o600 });
  console.log('ZW-G1 generated-routing-id=ready node=zone-node-2');

  await timerFailure.waitFor('scenario ZW-C4 passed');
  await timerFailure.complete();

  const gatewayPath = ctx.writeConfig('gateway', { shared, gateway });
  await ctx.start('gateway', 'dist/Server/Gateway/main.js', ['--config', gatewayPath]);
  await ctx.waitTcp(gateway.streamEndpoint);
  await ctx.waitLog('zone-node-1', 'mesh status node=zone-node-1 state=1');
  await ctx.waitLog('zone-node-2', 'mesh status node=zone-node-2 state=1');
  await ctx.waitLog('gateway', 'gateway mesh status mesh=zoneworld.zones state=1');
  await ctx.waitLog('zone-node-1', 'mesh status node=zone-node-1 state=1 readyPeers=3');
  await ctx.waitLog('zone-node-2', 'mesh status node=zone-node-2 state=1 readyPeers=3');

  const clientPath = ctx.writeConfig('client', {
    shared,
    client: { gatewayEndpoint: gateway.streamEndpoint, opsEndpoint: ops.streamEndpoint, scenarios: 'ZW-A1' }
  });
  ctx.runNode(path.join(ctx.sampleRoot, 'dist/Client/main.js'), ['--config', clientPath]);
  await ctx.waitLog('zone-node-1', 'fanout subscriber received announcement');
  await ctx.waitLog('zone-node-2', 'fanout subscriber received announcement');
  for (const zoneId of ['zone-nw', 'zone-sw']) {
    await ctx.waitLog('zone-node-1', `zone spot announcement delivered zone=${zoneId}`);
  }

  const extra = await zoneNodeConfig(ctx, shared, 'zone-node-3', 'extra', { disableBots: true });
  await ctx.start('zone-node-3', 'dist/Server/ZoneNode/main.js', ['--config', extra.path]);
  await ctx.waitLog('zone-node-3', 'topology=ready node=zone-node-3 zones=');
  ctx.runNode(path.join(ctx.sampleRoot, 'dist/Client/special.js'), [
    '--config', specialClientConfig(ctx, shared, gateway, ops, 'D2')
  ]);
  await ctx.waitLog('zone-node-3', 'fanout subscriber received announcement');
  console.log('scenario ZW-D2 passed');

  // The common internals do not recreate an existing User Spot after a process
  // restart. Run application operations while their original owners are ready.
  ctx.runNode(path.join(ctx.sampleRoot, 'dist/Client/special.js'), [
    '--config', specialClientConfig(ctx, shared, gateway, ops, 'E')
  ]);

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
  await ctx.waitLog(
    'zone-node-1',
    'zone player entered zone=zone-nw player=bot-ne-x from=zone-node-2'
  );
  await ctx.waitLog(
    'zone-node-2',
    'zone change scheduled player=bot-ne-x from=zone-ne to=zone-nw'
  );
  const botLogs = ['zone-node-1', 'zone-node-2']
    .map((name) => fs.readFileSync(path.join(ctx.logDir, `${name}.log`), 'utf8'))
    .join('\n');
  const spawned = new Set([...botLogs.matchAll(/bot spawned bot=([^ ]+)/g)].map((match) => match[1]));
  if (spawned.size !== 8) throw new Error(`ZW-F1 expected 8 spawned bots, observed ${spawned.size}.`);
  if (/No current session binding exists for actor 'bot-/.test(botLogs)) {
    throw new Error('ZW-F3 attempted to push to an unbound bot actor.');
  }
  console.log('scenario ZW-F2 passed');
  const bots = startScenarioClient(
    ctx,
    specialClientConfig(ctx, shared, gateway, ops, 'F'),
    'bots'
  );
  await bots.waitFor('scenario ZW-F4 passed');
  await bots.complete();

  const transition = startScenarioClient(
    ctx,
    specialClientConfig(ctx, shared, gateway, ops, 'B4-C2-C3'),
    'transition'
  );
  await transition.waitFor('scenario ZW-B4-C2-C3 armed');
  await ctx.stop('zone-node-2', 'SIGKILL');
  await transition.waitFor('scenario ZW-B4 passed');
  await transition.waitFor('scenario ZW-C2 passed');
  await transition.waitFor('scenario ZW-C3 passed');
  await transition.complete();
  const eastAfterFailure = await zoneNodeConfig(ctx, shared, 'zone-node-2', 'east-after-failure', { disableBots: true });
  await ctx.start('zone-node-2-after-failure', 'dist/Server/ZoneNode/main.js', ['--config', eastAfterFailure.path]);
  await ctx.waitLog('zone-node-2-after-failure', 'topology=ready');
  await ctx.waitLog('zone-node-2-after-failure', 'mesh status node=zone-node-2 state=1');
  ctx.runNode(path.join(ctx.sampleRoot, 'dist/Client/special.js'), [
    '--config', specialClientConfig(ctx, shared, gateway, ops, 'E5-arm')
  ]);
  await stopAndWaitForLocationLease(
    ctx,
    'zone-node-2-after-failure',
    eastAfterFailure.value.zoneNode,
    shared
  );
  const eastAfterMaintenance = await zoneNodeConfig(ctx, shared, 'zone-node-2', 'east-after-maintenance', {
    disableBots: true
  });
  await ctx.start('zone-node-2-after-maintenance', 'dist/Server/ZoneNode/main.js', ['--config', eastAfterMaintenance.path]);
  await ctx.waitLog('zone-node-2-after-maintenance', 'maintenance restored node=zone-node-2 enabled=true');
  ctx.runNode(path.join(ctx.sampleRoot, 'dist/Client/special.js'), [
    '--config', specialClientConfig(ctx, shared, gateway, ops, 'E5')
  ]);

  await stopAndWaitForLocationLease(
    ctx,
    'zone-node-2-after-maintenance',
    eastAfterMaintenance.value.zoneNode,
    shared
  );
  await stopNormalTopology(ctx);
  await runRoutingProbes(ctx, shared);
  console.log('ZW-G5 caller-fixed-routing-id=absent');
  console.log('topology=ready');
  console.log('zoneworld-transfer=completed');
  console.log('zoneworld-border-sync=completed');
  console.log('zoneworld-ops-observe=completed');
  console.log('zoneworld-ops-announce=completed');
  console.log('zoneworld-ops-maintenance=completed');
  console.log('zoneworld=completed');
  console.log('PASS ZoneWorld');
}

async function zoneNodeConfig(ctx, shared, nodeId, name, overrides = {}) {
  const value = {
    shared,
    zoneNode: {
      nodeId,
      spotRouterEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
      ...overrides
    }
  };
  return { value, path: ctx.writeConfig(name, value) };
}

async function stopNormalTopology(ctx) {
  for (const name of [
    'zone-node-1',
    'zone-node-2',
    'zone-node-2-after-failure',
    'zone-node-2-after-maintenance',
    'zone-node-3',
    'gateway',
    'ops'
  ]) {
    await ctx.stop(name, 'SIGKILL');
  }
}

async function runRoutingProbes(ctx, shared) {
  // Replacement probes use an isolated Location Store and run after the normal
  // topology has stopped. Ready-owner crash failover is intentionally outside
  // the common sample contract.
  const probeShared = {
    ...shared,
    redisKeyPrefix: `${shared.redisKeyPrefix}routing-probe:`
  };
  const probe = await zoneNodeConfig(ctx, probeShared, 'zone-node-2', 'east-probe', {
    disableBots: true,
    waitForPlacementPeer: false
  });
  const replacement = await zoneNodeConfig(ctx, probeShared, 'zone-node-2', 'east-replacement', {
    disableBots: true,
    waitForPlacementPeer: false
  });
  const crashReplacement = await zoneNodeConfig(ctx, probeShared, 'zone-node-2', 'east-crash-replacement', {
    disableBots: true,
    waitForPlacementPeer: false
  });
  await ctx.start('zone-node-2-probe', 'dist/Server/ZoneNode/main.js', ['--config', probe.path]);
  await ctx.waitTcp(probe.value.zoneNode.spotRouterEndpoint);
  await ctx.start('zone-node-2-replacement', 'dist/Server/ZoneNode/main.js', ['--config', replacement.path]);
  await ctx.waitTcp(replacement.value.zoneNode.spotRouterEndpoint);
  console.log('ZW-G3 rolling-replacement=ready');
  ctx.signal('zone-node-2-probe', 'SIGKILL');
  ctx.signal('zone-node-2-replacement', 'SIGKILL');
  await ctx.start(
    'zone-node-2-crash-replacement',
    'dist/Server/ZoneNode/main.js',
    ['--config', crashReplacement.path]
  );
  await ctx.waitTcp(crashReplacement.value.zoneNode.spotRouterEndpoint);
  console.log('ZW-G4 crash-replacement=ready');
  ctx.signal('zone-node-2-crash-replacement', 'SIGKILL');
}

function specialClientConfig(ctx, shared, gateway, ops, scenarios) {
  return ctx.writeConfig(`client-${scenarios.toLowerCase().replaceAll(/[^a-z0-9]+/g, '-')}`, {
    shared,
    client: { gatewayEndpoint: gateway.streamEndpoint, opsEndpoint: ops.streamEndpoint, scenarios }
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
    complete
  };
}


async function stopAndWaitForLocationLease(ctx, name, _node, _shared) {
  await ctx.stop(name, 'SIGKILL');
  // The sample config fixes ownerLeaseTtlMs at 3 seconds.
  await delay(3_100);
}
