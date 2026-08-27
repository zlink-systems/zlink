import fs from 'node:fs';
import path from 'node:path';

export const sampleName = 'DeliveryDispatch.Ts';

const waitIntervalMs = Number(process.env.DELIVERYDISPATCH_WAIT_INTERVAL_MS ?? '100');
const waitAttempts = Number(process.env.DELIVERYDISPATCH_WAIT_ATTEMPTS ?? '300');

export async function runSample(ctx) {
  const flowDir = path.join(ctx.logDir, 'flow');
  fs.mkdirSync(flowDir, { recursive: true });
  const sample = {
    dispatchApiHttpUrl: `http://127.0.0.1:${await ctx.port()}`,
    dispatchEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
    dispatchSpotEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
    courierStreamEndpoint: `ws://127.0.0.1:${await ctx.port()}`,
    courierActorNode1SpotEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
    courierActorNode2SpotEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
    trackingEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
    trackingSpotEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
    sessionStreamEndpoint: `ws://127.0.0.1:${await ctx.port()}`,
    sessionSpotRouterEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
    courierSessionSpotEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
    redisEndpoint: ctx.redisEndpoint,
    redisKeyPrefix: `deliverydispatch:node:${process.pid}:`,
    logDir: flowDir,
    workDir: ctx.workDir
  };
  const roleConfig = (name, keys) => ctx.writeConfig(name,
    Object.fromEntries(keys.map((key) => [key, sample[key]])));
  for (const [role, entry, ready, configPath] of [
    ['tracking', 'dist/Server/Tracking/main.js', sample.trackingSpotEndpoint,
      roleConfig('tracking', ['trackingEndpoint', 'trackingSpotEndpoint', 'redisEndpoint', 'redisKeyPrefix', 'logDir', 'workDir'])],
    ['customer-gateway', 'dist/Server/Session/main.js', sample.sessionStreamEndpoint,
      roleConfig('customer-gateway', ['sessionSpotRouterEndpoint', 'sessionStreamEndpoint', 'redisEndpoint', 'redisKeyPrefix', 'logDir'])],
    ['courier-session', 'dist/Server/CourierSession/main.js', sample.courierStreamEndpoint,
      roleConfig('courier-session', ['courierStreamEndpoint', 'courierSessionSpotEndpoint', 'redisEndpoint', 'redisKeyPrefix', 'logDir'])],
    ['courier-node-1', 'dist/Server/Courier/node1-main.js', sample.courierActorNode1SpotEndpoint,
      roleConfig('courier-node-1', ['courierActorNode1SpotEndpoint', 'redisEndpoint', 'redisKeyPrefix', 'logDir'])],
    ['courier-node-2', 'dist/Server/Courier/node2-main.js', sample.courierActorNode2SpotEndpoint,
      roleConfig('courier-node-2', ['courierActorNode2SpotEndpoint', 'redisEndpoint', 'redisKeyPrefix', 'logDir'])],
    ['dispatch', 'dist/Server/Dispatch/main.js', sample.dispatchSpotEndpoint,
      roleConfig('dispatch', ['dispatchApiHttpUrl', 'dispatchEndpoint', 'dispatchSpotEndpoint', 'redisEndpoint', 'redisKeyPrefix', 'logDir', 'workDir'])]
  ]) {
    await ctx.start(role, entry, ['--config', configPath]);
    await ctx.waitTcp(ready);
  }
  await ctx.waitHttp(sample.dispatchApiHttpUrl);
  const routeReadiness = [
    ['tracking', 'tracking'],
    ['customer-gateway', 'customer-gateway'],
    ['courier-session', 'courier-session'],
    ['courier-node-1', 'courier-node-1'],
    ['courier-node-2', 'courier-node-2'],
    ['dispatch', 'dispatch']
  ];
  for (const [role, nodeId] of routeReadiness) {
    await waitForLog(ctx, role, `deliverydispatch-ready kind=route node=${nodeId}`);
  }
  await waitForLog(ctx, 'dispatch', 'deliverydispatch-ready kind=actor-route node=dispatch target=courier-node-1');
  await waitForLog(ctx, 'dispatch', 'deliverydispatch-ready kind=actor-route node=dispatch target=courier-node-2');
  ctx.runBrowser({
    timeoutMs: 90_000,
    config: {
      dispatchApiHttpUrl: '/api/delivery',
      sessionStreamEndpoint: sample.sessionStreamEndpoint,
      courierStreamEndpoint: sample.courierStreamEndpoint
    },
    proxies: [{ prefix: '/api/delivery', target: sample.dispatchApiHttpUrl }]
  });

  const clientMarkers = [
    'deliverydispatch=completed',
    'deliverydispatch-reassignment=completed',
    'deliverydispatch-server-evidence=completed'
  ];

  const serverEvidence = [
    ['courier-session', 'deliverydispatch-courier bound courier=courier-a', 1],
    ['courier-session', 'deliverydispatch-courier bound courier=courier-b', 1],
    //  Which courier node relays which courier is placement-dependent - the actor handler runs
    //  where the actor lives. Count across both courier node logs rather than pinning one.
    [['courier-node-1', 'courier-node-2'], 'deliverydispatch-courier bind-relayed courier=courier-a', 1],
    [['courier-node-1', 'courier-node-2'], 'deliverydispatch-courier bind-relayed courier=courier-b', 1],
    ['customer-gateway', 'deliverydispatch-customer bound customer=customer-1', 1],
    ['customer-gateway', 'deliverydispatch-customer pushed status=Delivered delivery=', 2],
    ['tracking', 'deliverydispatch-tracking status=Delivered delivery=', 2],
    ['dispatch', 'deliverydispatch-dispatch stale-decision-ignored delivery=delivery-reassign courier=courier-a attempt=1', 1],
    ['dispatch', 'deliverydispatch-dispatch failed delivery=delivery-exhausted reason=candidates-exhausted', 1]
  ];
  for (const [role, marker, expected] of serverEvidence) {
    if (expected > 0) await waitForLogAnyRole(ctx, role, marker);
  }
  //  runBrowser() runs the scenario to completion, so its log is final by now.
  for (const marker of clientMarkers) await waitForLog(ctx, 'browser-client', marker);
  for (const marker of clientMarkers) ctx.assertLogCount('browser-client', marker, 1);
  for (const [role, marker, expected] of serverEvidence) {
    assertLogCountAnyRole(ctx, role, marker, expected);
  }
  console.log('deliverydispatch-placement=completed');
}


//  A role may be a single name or a list of names to count across.
async function waitForLogAnyRole(ctx, role, marker) {
  const roles = Array.isArray(role) ? role : [role];
  for (let attempt = 0; attempt < 300; attempt++) {
    if (roles.some((name) => readRoleLog(ctx, name).includes(marker))) return;
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`Timed out waiting for ${roles.join('/')} log marker '${marker}'.`);
}

function assertLogCountAnyRole(ctx, role, marker, expected) {
  const roles = Array.isArray(role) ? role : [role];
  const actual = roles.reduce(
    (total, name) => total + readRoleLog(ctx, name).split(marker).length - 1,
    0
  );
  if (actual !== expected) {
    throw new Error(`Log marker '${marker}' count was ${actual} across ${roles.join('/')}; expected ${expected}.`);
  }
}

function readRoleLog(ctx, role) {
  const target = path.join(ctx.logDir, `${role}.log`);
  return fs.existsSync(target) ? fs.readFileSync(target, 'utf8') : '';
}

async function waitForLog(ctx, role, marker) {
  const target = path.join(ctx.logDir, `${role}.log`);
  for (let attempt = 0; attempt < waitAttempts; attempt += 1) {
    if (fs.existsSync(target) && fs.readFileSync(target, 'utf8').includes(marker)) return;
    await new Promise((resolve) => setTimeout(resolve, waitIntervalMs));
  }
  throw new Error(`Timed out waiting for ${role} log marker '${marker}' after ${waitAttempts} attempts.`);
}
