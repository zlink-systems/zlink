import fs from 'node:fs';
import path from 'node:path';

export const sampleName = 'Bingo.Ts';

export async function runSample(ctx) {
  const redisKeyPrefix = `bingo:node:${process.pid}:`;
  const apiA = `tcp://127.0.0.1:${await ctx.port()}`;
  const apiB = `tcp://127.0.0.1:${await ctx.port()}`;
  const apiMatchmakingA = `tcp://127.0.0.1:${await ctx.port()}`;
  const apiMatchmakingB = `tcp://127.0.0.1:${await ctx.port()}`;
  const matchmaking = `tcp://127.0.0.1:${await ctx.port()}`;
  const playA = await bingoPlayConfig(ctx, 'a', redisKeyPrefix);
  const playB = await bingoPlayConfig(ctx, 'b', redisKeyPrefix);
  const sessionA = await bingoSessionConfig(ctx, 'a', redisKeyPrefix);
  const sessionB = await bingoSessionConfig(ctx, 'b', redisKeyPrefix);
  const common = { redisEndpoint: ctx.redisEndpoint, redisKeyPrefix };
  const flowDir = path.join(ctx.logDir, 'flow');
  fs.mkdirSync(flowDir, { recursive: true });
  const apiAConfig = ctx.writeConfig('api-a', {
    ...common, nodeId: 'api-a', apiEndpoint: apiA, apiMatchmakingEndpoint: apiMatchmakingA, logDir: flowDir
  });
  const apiBConfig = ctx.writeConfig('api-b', {
    ...common, nodeId: 'api-b', apiEndpoint: apiB, apiMatchmakingEndpoint: apiMatchmakingB, logDir: flowDir
  });
  const matchmakingConfig = ctx.writeConfig('matchmaking', {
    ...common, nodeId: 'matchmaking', matchmakingEndpoint: matchmaking, logDir: flowDir
  });

  await ctx.start('matchmaking', 'dist/Server/Matchmaking/main.js', ['--config', matchmakingConfig]);
  await ctx.waitTcp(matchmaking);
  await ctx.start('api-b', 'dist/Server/Api/main.js', ['--config', apiBConfig]);
  await ctx.waitTcp(apiB);
  await ctx.start('api-a', 'dist/Server/Api/main.js', ['--config', apiAConfig]);
  await ctx.waitTcp(apiA);
  await ctx.start('play-b', 'dist/Server/Play/main.js', ['--config', playB.path]);
  await ctx.waitTcp(playB.sample.playSpotEndpoint);
  await ctx.start('play-a', 'dist/Server/Play/main.js', ['--config', playA.path]);
  await ctx.waitTcp(playA.sample.playSpotEndpoint);
  await ctx.waitLog('play-a', 'bingo-ready kind=peer-route node=play-a peer=play-b');
  await ctx.waitLog('play-b', 'bingo-ready kind=peer-route node=play-b peer=play-a');
  await ctx.start('session-b', 'dist/Server/Session/main.js', ['--config', sessionB.path]);
  await ctx.waitTcp(sessionB.sample.sessionEndpoint);
  await ctx.start('session-a', 'dist/Server/Session/main.js', ['--config', sessionA.path]);
  await ctx.waitTcp(sessionA.sample.sessionEndpoint);
  await waitMeshReady(ctx, 'api-a', 'matchmaking');
  await waitMeshReady(ctx, 'api-a', 'room');
  await waitMeshReady(ctx, 'api-b', 'matchmaking');
  await waitMeshReady(ctx, 'api-b', 'room');
  await waitMeshReady(ctx, 'session-a', 'room');
  await waitMeshReady(ctx, 'session-b', 'room');
  ctx.runBrowser({
    timeoutMs: 90_000,
    config: {
      sessionAEndpoint: sessionB.sample.sessionEndpoint,
      sessionBEndpoint: sessionA.sample.sessionEndpoint
    },
    proxies: []
  });
  const playEvidence = [
    ['bingo-record fetched actor=player-1 wins=0 losses=0', 1],
    ['bingo-record fetched actor=player-2 wins=0 losses=0', 1],
    ['bingo-record reported actor=player-1 wins=1 losses=0', 1],
    ['bingo-record reported actor=player-2 wins=0 losses=1', 1],
    ['bingo-record reported actor=observer', 0],
    ['bingo-lifecycle room-leave actor=player-1', 1],
    ['bingo-lifecycle room-leave actor=player-2', 1],
    ['bingo-lifecycle room-leave actor=observer', 1],
    ['bingo-lifecycle entry-leave actor=player-1', 1],
    ['bingo-lifecycle entry-leave actor=player-2', 1],
    ['bingo-lifecycle entry-leave actor=observer', 1],
    ['bingo-lifecycle entry-destroy-complete actor=player-1', 1],
    ['bingo-lifecycle entry-destroy-complete actor=player-2', 1],
    ['bingo-lifecycle entry-destroy-complete actor=observer', 0]
  ];
  const sessionEvidence = [
    ['bingo-lifecycle session-disconnect actor=player-1 destroy=false', 1],
    ['bingo-lifecycle session-disconnect actor=player-2 destroy=false', 1]
  ];
  for (const [marker, expected] of playEvidence) {
    if (expected > 0) await waitCombinedLog(ctx, ['play-a', 'play-b'], marker);
  }
  for (const [marker, expected] of sessionEvidence) {
    if (expected > 0) await waitCombinedLog(ctx, ['session-a', 'session-b'], marker);
  }
  for (const [marker, expected] of playEvidence) {
    assertCombinedLogCount(ctx, ['play-a', 'play-b'], marker, expected);
  }
  for (const [marker, expected] of sessionEvidence) {
    assertCombinedLogCount(ctx, ['session-a', 'session-b'], marker, expected);
  }
  console.log('bingo-placement=completed');
}

async function bingoPlayConfig(ctx, suffix, redisKeyPrefix) {
  const sample = {
    nodeId: `play-${suffix}`,
    peerNodeId: `play-${suffix === 'a' ? 'b' : 'a'}`,
    redisEndpoint: ctx.redisEndpoint,
    redisKeyPrefix,
    logDir: path.join(ctx.logDir, 'flow'),
    playEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
    playSpotEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
    playSpotPubSubEndpoint: `tcp://127.0.0.1:${await ctx.port()}`
  };
  return { sample, path: ctx.writeConfig(`play-${suffix}`, sample) };
}

async function bingoSessionConfig(ctx, suffix, redisKeyPrefix) {
  const sample = {
    nodeId: `session-${suffix}`,
    redisEndpoint: ctx.redisEndpoint,
    redisKeyPrefix,
    sessionEndpoint: `ws://127.0.0.1:${await ctx.port()}`,
    sessionSpotEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
    sessionSpotPubSubEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
    logDir: path.join(ctx.logDir, 'flow')
  };
  return { sample, path: ctx.writeConfig(`session-${suffix}`, sample) };
}

async function waitMeshReady(ctx, nodeId, meshName) {
  await ctx.waitLog(nodeId, `bingo-ready kind=mesh-route node=${nodeId} mesh=${meshName}`);
}

async function waitCombinedLog(ctx, roles, marker) {
  await ctx.waitAnyLog(roles.map((name) => ({ name, marker })));
}

function assertCombinedLogCount(ctx, roles, marker, expected) {
  const actual = roles.reduce((count, role) => {
    const file = path.join(ctx.logDir, `${role}.log`);
    if (!fs.existsSync(file)) return count;
    return count + fs.readFileSync(file, 'utf8').split(marker).length - 1;
  }, 0);
  if (actual !== expected) {
    throw new Error(`${roles.join('/')} logs marker '${marker}' count was ${actual}; expected ${expected}.`);
  }
}
