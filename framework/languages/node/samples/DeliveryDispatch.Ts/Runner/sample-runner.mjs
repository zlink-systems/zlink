import fs from 'node:fs';
import path from 'node:path';

export const sampleName = 'DeliveryDispatch.Ts';

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
    ['courier-spot-node1', 'dist/Server/Courier/node1-main.js', sample.courierActorNode1SpotEndpoint,
      roleConfig('courier-spot-node1', ['courierActorNode1SpotEndpoint', 'redisEndpoint', 'redisKeyPrefix', 'logDir'])],
    ['courier-spot-node2', 'dist/Server/Courier/node2-main.js', sample.courierActorNode2SpotEndpoint,
      roleConfig('courier-spot-node2', ['courierActorNode2SpotEndpoint', 'redisEndpoint', 'redisKeyPrefix', 'logDir'])],
    ['dispatch', 'dist/Server/Dispatch/main.js', sample.dispatchSpotEndpoint,
      roleConfig('dispatch', ['dispatchApiHttpUrl', 'dispatchEndpoint', 'dispatchSpotEndpoint', 'redisEndpoint', 'redisKeyPrefix', 'logDir', 'workDir'])]
  ]) {
    await ctx.start(role, entry, ['--config', configPath]);
    await ctx.waitTcp(ready);
  }
  await ctx.waitHttp(sample.dispatchApiHttpUrl);
  ctx.runBrowser({
    timeoutMs: 90_000,
    config: {
      dispatchApiHttpUrl: '/api/delivery',
      sessionStreamEndpoint: sample.sessionStreamEndpoint,
      courierStreamEndpoint: sample.courierStreamEndpoint
    },
    proxies: [{ prefix: '/api/delivery', target: sample.dispatchApiHttpUrl }]
  });
  await ctx.waitLog('dispatch', 'ignored stale decision delivery=delivery-reassign courier=courier-a attempt=1');
}
