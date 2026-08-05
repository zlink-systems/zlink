import fs from 'node:fs';
import path from 'node:path';

export const sampleName = 'TicTacToe.Ts';

export async function runSample(ctx) {
  const endpoints = {
    apiHttp: [`http://127.0.0.1:${await ctx.port()}`, `http://127.0.0.1:${await ctx.port()}`],
    api: [`tcp://127.0.0.1:${await ctx.port()}`, `tcp://127.0.0.1:${await ctx.port()}`],
    playStream: [`ws://127.0.0.1:${await ctx.port()}`, `ws://127.0.0.1:${await ctx.port()}`],
    playSpot: [`tcp://127.0.0.1:${await ctx.port()}`, `tcp://127.0.0.1:${await ctx.port()}`]
  };
  const redisKeyPrefix = `tictactoe:node:${process.pid}:`;
  const config = (instanceName, apiIndex, playIndex, peerPlayIndex) => ({
    instanceName,
    apiIndex,
    playIndex,
    apiHttpEndpoint: endpoints.apiHttp[apiIndex],
    apiEndpoints: endpoints.api,
    apiHttpEndpoints: endpoints.apiHttp,
    playEndpoints: endpoints.playStream,
    playSpotEndpoint: endpoints.playSpot[playIndex],
    playSpotEndpoints: endpoints.playSpot,
    playStreamEndpoint: endpoints.playStream[playIndex],
    redisEndpoint: ctx.redisEndpoint,
    redisKeyPrefix,
    logDir: path.join(ctx.logDir, 'flow'),
    peerPlaySpotEndpoint: endpoints.playSpot[peerPlayIndex]
  });
  const roleConfig = (name, value, keys) => ctx.writeConfig(name,
    Object.fromEntries(keys.map((key) => [key, value[key]])));
  const playA = config('play-a', 0, 0, 1);
  const playB = config('play-b', 0, 1, 0);
  const apiA = config('api-a', 0, 0, 1);
  const apiB = config('api-b', 1, 0, 1);
  const playKeys = [
    'apiEndpoints', 'playSpotEndpoint', 'playStreamEndpoint', 'playEndpoints',
    'redisEndpoint', 'redisKeyPrefix', 'peerPlaySpotEndpoint', 'instanceName',
    'logDir'
  ];
  const apiKeys = [
    'apiHttpEndpoint', 'apiEndpoints', 'apiIndex', 'playSpotEndpoints', 'playEndpoints',
    'redisEndpoint', 'redisKeyPrefix', 'logDir'
  ];
  const configs = {
    playA: roleConfig('play-a', playA, playKeys),
    playB: roleConfig('play-b', playB, playKeys),
    apiA: roleConfig('api-a', apiA, apiKeys),
    apiB: roleConfig('api-b', apiB, apiKeys)
  };
  fs.mkdirSync(path.join(ctx.logDir, 'flow'), { recursive: true });
  await ctx.start('play-b', 'dist/Server/Play/main.js', ['--config', configs.playB]);
  await ctx.waitTcp(endpoints.playStream[1]);
  await ctx.start('play-a', 'dist/Server/Play/main.js', ['--config', configs.playA]);
  await ctx.waitTcp(endpoints.playStream[0]);
  await ctx.waitLog('play-a', 'spotPeerReady');
  await ctx.waitLog('play-b', 'spotPeerReady');
  await ctx.start('api-a', 'dist/Server/Api/main.js', ['--config', configs.apiA]);
  await ctx.waitTcp(endpoints.apiHttp[0]);
  await ctx.start('api-b', 'dist/Server/Api/main.js', ['--config', configs.apiB]);
  await ctx.waitTcp(endpoints.apiHttp[1]);
  ctx.runBrowser({
    timeoutMs: 90_000,
    config: { apiHttpEndpoint: '/api/tictactoe' },
    proxies: [{ prefix: '/api/tictactoe', target: endpoints.apiHttp[0] }]
  });
}
