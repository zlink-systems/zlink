import fs from 'node:fs';
import path from 'node:path';

export const sampleName = 'TicTacToe.Ts';

export async function runSample(ctx) {
  const endpoints = {
    apiHttp: [`http://127.0.0.1:${await ctx.port()}`, `http://127.0.0.1:${await ctx.port()}`],
    api: [`tcp://127.0.0.1:${await ctx.port()}`, `tcp://127.0.0.1:${await ctx.port()}`],
    apiSpot: [`tcp://127.0.0.1:${await ctx.port()}`, `tcp://127.0.0.1:${await ctx.port()}`],
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
    apiSpotEndpoint: endpoints.apiSpot[apiIndex],
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
    'apiEndpoints', 'playIndex', 'playSpotEndpoint', 'playStreamEndpoint', 'playEndpoints',
    'redisEndpoint', 'redisKeyPrefix', 'peerPlaySpotEndpoint', 'instanceName',
    'logDir'
  ];
  const apiKeys = [
    'apiHttpEndpoint', 'apiEndpoints', 'apiSpotEndpoint', 'apiIndex', 'playSpotEndpoints', 'playEndpoints',
    'redisEndpoint', 'redisKeyPrefix', 'logDir', 'instanceName'
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
  await ctx.waitLog('play-a', 'tictactoe-ready kind=peer-route node=play-a peer=play-b');
  await ctx.waitLog('play-b', 'tictactoe-ready kind=peer-route node=play-b peer=play-a');
  await ctx.start('api-a', 'dist/Server/Api/main.js', ['--config', configs.apiA]);
  await ctx.waitTcp(endpoints.apiHttp[0]);
  await ctx.start('api-b', 'dist/Server/Api/main.js', ['--config', configs.apiB]);
  await ctx.waitTcp(endpoints.apiHttp[1]);
  await ctx.waitLog('api-a', 'tictactoe-ready kind=http node=api-a');
  await ctx.waitLog('api-b', 'tictactoe-ready kind=http node=api-b');
  await ctx.waitLog('api-a', 'tictactoe-ready kind=spot-route node=api-a mesh=tictactoe');
  await ctx.waitLog('api-b', 'tictactoe-ready kind=spot-route node=api-b mesh=tictactoe');
  const browser = ctx.startBrowser({
    timeoutMs: 90_000,
    config: {
      apiHttpEndpoint: '/api/tictactoe',
      lifecycleCompletionPath: '/runner/lifecycle-complete'
    },
    proxies: [{ prefix: '/api/tictactoe', target: endpoints.apiHttp[0] }]
  });
  const playEvidence = [
    ['tictactoe-lifecycle actor-bound actor=player-x', 1],
    ['tictactoe-lifecycle leave-completed actor=player-x', 1],
    ['tictactoe-lifecycle leave-completed actor=player-o', 1],
    ['tictactoe-lifecycle actor-destroy-complete actor=player-x', 1],
    ['tictactoe-lifecycle actor-destroy-complete actor=player-o', 1],
    ['tictactoe-lifecycle actor-destroy-complete actor=observer', 0]
  ];
  for (const [marker, expected] of playEvidence) {
    if (expected > 0) await waitPlayLog(ctx, marker);
  }
  await browser.complete();
  const clientEvidence = [
    [`observer-connected endpoint=${endpoints.playStream[1]}`, 1],
    ['observer-subscription=verified subscribed=true', 1],
    ['observer-win-milestone=verified actor=player-x wins=100', 1],
    ['reconnected-game-state=verified actor=player-x room=', 1],
    ['tictactoe=completed', 1]
  ];
  for (const [marker, expected] of clientEvidence) ctx.assertLogCount('browser-client', marker, expected);
  for (const [marker, expected] of playEvidence) assertCombinedLogCount(ctx, ['play-a', 'play-b'], marker, expected);
  console.log('tictactoe-placement=completed');
}

function assertCombinedLogCount(ctx, roles, marker, expected) {
  const actual = roles.reduce((count, role) => {
    const file = path.join(ctx.logDir, `${role}.log`);
    return count + (fs.existsSync(file) ? fs.readFileSync(file, 'utf8').split(marker).length - 1 : 0);
  }, 0);
  if (actual !== expected) throw new Error(`${marker} count was ${actual}; expected ${expected}.`);
}

async function waitPlayLog(ctx, marker) {
  await ctx.waitAnyLog([
    { name: 'play-a', marker },
    { name: 'play-b', marker }
  ]);
}
