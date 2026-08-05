import path from 'node:path';

export const sampleName = 'ShoppingMall.Ts';

export async function runSample(ctx) {
  const sample = {
    apiAHttpUrl: `http://127.0.0.1:${await ctx.port()}`,
    apiBHttpUrl: `http://127.0.0.1:${await ctx.port()}`,
    workflowAHttpUrl: `http://127.0.0.1:${await ctx.port()}`,
    workflowBHttpUrl: `http://127.0.0.1:${await ctx.port()}`,
    workflowAChannelEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
    workflowBChannelEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
    workflowASpotEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
    workflowBSpotEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
    workflowASpotPubEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
    workflowBSpotPubEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
    redisEndpoint: ctx.redisEndpoint,
    redisKeyPrefix: `shoppingmall:node:${process.pid}:`,
    logDir: ctx.logDir,
    workDir: ctx.workDir
  };
  const roleConfig = (name, keys) => ctx.writeConfig(name,
    Object.fromEntries(keys.map((key) => [key, sample[key]])));
  for (const [role, entry, ready, configPath] of [
    ['workflow-a', 'dist/Server/WorkflowA/main.js', sample.workflowAHttpUrl,
      roleConfig('workflow-a', ['workflowAHttpUrl', 'workflowAChannelEndpoint', 'workflowASpotEndpoint', 'workflowASpotPubEndpoint', 'redisEndpoint', 'redisKeyPrefix', 'logDir', 'workDir'])],
    ['workflow-b', 'dist/Server/WorkflowB/main.js', sample.workflowBHttpUrl,
      roleConfig('workflow-b', ['workflowBHttpUrl', 'workflowBChannelEndpoint', 'workflowBSpotEndpoint', 'workflowBSpotPubEndpoint', 'redisEndpoint', 'redisKeyPrefix', 'logDir', 'workDir'])],
    ['api-a', 'dist/Server/ApiA/main.js', sample.apiAHttpUrl,
      roleConfig('api-a', ['apiAHttpUrl', 'redisEndpoint', 'redisKeyPrefix', 'logDir', 'workDir'])],
    ['api-b', 'dist/Server/ApiB/main.js', sample.apiBHttpUrl,
      roleConfig('api-b', ['apiBHttpUrl', 'redisEndpoint', 'redisKeyPrefix', 'logDir', 'workDir'])]
  ]) {
    await ctx.start(role, entry, ['--config', configPath]);
    await ctx.waitHttp(ready);
  }
  ctx.runNode(path.join(ctx.sampleRoot, 'dist/Client/main.js'), [
    '--api-a-http', sample.apiAHttpUrl,
    '--api-b-http', sample.apiBHttpUrl
  ]);
}
