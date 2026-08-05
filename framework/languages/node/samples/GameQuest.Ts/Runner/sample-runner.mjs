export const sampleName = 'GameQuest.Ts';

export async function runSample(ctx) {
  const sample = {
    apiAHttpUrl: `http://127.0.0.1:${await ctx.port()}`,
    apiBHttpUrl: `http://127.0.0.1:${await ctx.port()}`,
    apiAStreamEndpoint: `ws://127.0.0.1:${await ctx.port()}`,
    apiBStreamEndpoint: `ws://127.0.0.1:${await ctx.port()}`,
    apiAActorSpotEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
    apiBActorSpotEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
    missionAEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
    missionBEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
    missionASpotEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
    missionBSpotEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
    missionASpotRouterEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
    missionBSpotRouterEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
    missionAHttpUrl: `http://127.0.0.1:${await ctx.port()}`,
    missionBHttpUrl: `http://127.0.0.1:${await ctx.port()}`,
    redisEndpoint: ctx.redisEndpoint,
    redisKeyPrefix: `gamequest:node:${process.pid}:`,
    logDir: ctx.logDir,
    workDir: ctx.workDir
  };
  const roleConfig = (name, keys) => ctx.writeConfig(name,
    Object.fromEntries(keys.map((key) => [key, sample[key]])));
  for (const [role, entry, ready, configPath] of [
    ['mission-a', 'dist/Server/MissionA/main.js', sample.missionAHttpUrl,
      roleConfig('mission-a', ['missionAEndpoint', 'missionASpotEndpoint', 'missionASpotRouterEndpoint', 'missionAHttpUrl', 'redisEndpoint', 'redisKeyPrefix', 'logDir', 'workDir'])],
    ['mission-b', 'dist/Server/MissionB/main.js', sample.missionBHttpUrl,
      roleConfig('mission-b', ['missionBEndpoint', 'missionBSpotEndpoint', 'missionBSpotRouterEndpoint', 'missionBHttpUrl', 'redisEndpoint', 'redisKeyPrefix', 'logDir', 'workDir'])],
    ['api-a', 'dist/Server/ApiA/main.js', sample.apiAHttpUrl,
      roleConfig('api-a', ['apiAHttpUrl', 'apiAStreamEndpoint', 'apiAActorSpotEndpoint', 'redisEndpoint', 'redisKeyPrefix', 'logDir', 'workDir'])],
    ['api-b', 'dist/Server/ApiB/main.js', sample.apiBHttpUrl,
      roleConfig('api-b', ['apiBHttpUrl', 'apiBStreamEndpoint', 'apiBActorSpotEndpoint', 'redisEndpoint', 'redisKeyPrefix', 'logDir', 'workDir'])]
  ]) {
    await ctx.start(role, entry, ['--config', configPath]);
    await ctx.waitHttp(ready);
  }
  ctx.runBrowser({
    timeoutMs: 120_000,
    config: {
      apiAHttpUrl: '/api/gamequest/api-a',
      apiBHttpUrl: '/api/gamequest/api-b',
      apiAStreamEndpoint: sample.apiAStreamEndpoint,
      apiBStreamEndpoint: sample.apiBStreamEndpoint,
      missionAHttpUrl: '/api/gamequest/mission-a',
      missionBHttpUrl: '/api/gamequest/mission-b'
    },
    proxies: [
      { prefix: '/api/gamequest/api-a', target: sample.apiAHttpUrl },
      { prefix: '/api/gamequest/api-b', target: sample.apiBHttpUrl },
      { prefix: '/api/gamequest/mission-a', target: sample.missionAHttpUrl },
      { prefix: '/api/gamequest/mission-b', target: sample.missionBHttpUrl }
    ]
  });
}
