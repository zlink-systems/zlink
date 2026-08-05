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
    ...common, apiEndpoint: apiA, apiMatchmakingEndpoint: apiMatchmakingA, logDir: flowDir
  });
  const apiBConfig = ctx.writeConfig('api-b', {
    ...common, apiEndpoint: apiB, apiMatchmakingEndpoint: apiMatchmakingB, logDir: flowDir
  });
  const matchmakingConfig = ctx.writeConfig('matchmaking', {
    ...common, matchmakingEndpoint: matchmaking, logDir: flowDir
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
  const replacement = await verifyPlaySlotHandoff(
    ctx,
    redisKeyPrefix,
    playA.sample.playSpotEndpoint
  );
  await ctx.start('session-b', 'dist/Server/Session/main.js', ['--config', sessionB.path]);
  await ctx.waitTcp(sessionB.sample.sessionEndpoint);
  await ctx.start('session-a', 'dist/Server/Session/main.js', ['--config', sessionA.path]);
  await ctx.waitTcp(sessionA.sample.sessionEndpoint);
  await waitForRouteReadiness(ctx, 'play-replacement', true);
  await waitForRouteReadiness(ctx, 'play-b', true);
  await waitForRouteReadiness(ctx, 'session-b');
  await waitForRouteReadiness(ctx, 'session-a');
  await waitForRouteReadiness(ctx, 'api-b');
  await waitForRouteReadiness(ctx, 'api-a');
  ctx.runBrowser({
    timeoutMs: 90_000,
    config: {
      sessionAEndpoint: sessionB.sample.sessionEndpoint,
      sessionBEndpoint: sessionA.sample.sessionEndpoint
    },
    proxies: []
  });
  // Object placement is intentionally dynamic. A replacement node may be
  // ready without owning the room, so lifecycle evidence must be accepted
  // from the current play owner rather than a process name.
  await waitPlayLog(ctx, 'bingo-record fetched actor=player-1 wins=0 losses=0');
  await waitPlayLog(ctx, 'bingo-record fetched actor=player-2 wins=0 losses=0');
  await waitPlayLog(ctx, 'bingo-record reported actor=player-1 wins=1 losses=0');
  await waitPlayLog(ctx, 'bingo-record reported actor=player-2 wins=0 losses=1');
  await waitPlayLog(ctx, 'bingo-lifecycle room-leave actor=player-1');
  await waitPlayLog(ctx, 'bingo-lifecycle room-leave actor=player-2');
  await waitPlayLog(ctx, 'bingo-lifecycle entry-destroy-complete actor=player-1');
  await waitPlayLog(ctx, 'bingo-lifecycle entry-destroy-complete actor=player-2');
  await waitPlayLog(ctx, 'bingo-lifecycle entry-leave actor=player-1');
  await waitPlayLog(ctx, 'bingo-lifecycle entry-leave actor=player-2');
  await waitPlayLog(ctx, 'bingo-lifecycle entry-leave actor=observer');
  await ctx.waitLog('session-b', 'bingo-lifecycle session-disconnect actor=player-1 destroy=false');
  await ctx.waitLog('session-a', 'bingo-lifecycle session-disconnect actor=player-2 destroy=false');
  assertPlayLogCount(ctx, 'bingo-lifecycle room-leave actor=observer', 1);
  assertPlayLogCount(ctx, 'bingo-lifecycle room-leave actor=player-1', 1);
  assertPlayLogCount(ctx, 'bingo-lifecycle room-leave actor=player-2', 1);
  assertPlayLogCount(ctx, 'bingo-lifecycle entry-destroy-complete actor=player-1', 1);
  assertPlayLogCount(ctx, 'bingo-lifecycle entry-destroy-complete actor=player-2', 1);
  assertPlayLogCount(ctx, 'bingo-lifecycle entry-leave actor=player-1', 1);
  assertPlayLogCount(ctx, 'bingo-lifecycle entry-leave actor=player-2', 1);
  assertPlayLogCount(ctx, 'bingo-lifecycle entry-leave actor=observer', 1);
  assertPlayLogCount(ctx, 'bingo-record reported actor=observer', 0);

}

async function bingoPlayConfig(ctx, suffix, redisKeyPrefix) {
  const sample = {
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
    redisEndpoint: ctx.redisEndpoint,
    redisKeyPrefix,
    sessionEndpoint: `ws://127.0.0.1:${await ctx.port()}`,
    sessionSpotEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
    sessionSpotPubSubEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
    logDir: path.join(ctx.logDir, 'flow')
  };
  return { sample, path: ctx.writeConfig(`session-${suffix}`, sample) };
}

async function verifyPlaySlotHandoff(ctx, redisKeyPrefix, oldRoomEndpoint) {
  const replacement = await bingoPlayConfig(ctx, 'replacement', redisKeyPrefix);
  await ctx.start('play-replacement', 'dist/Server/Play/main.js', ['--config', replacement.path]);
  await ctx.waitTcp(replacement.sample.playSpotEndpoint);
  await waitForRouteReadiness(ctx, 'play-replacement', true);
  ctx.signal('play-a', 'SIGUSR2');
  await ctx.waitLog('play-a', 'bingo-drain result=drained');
  await ctx.stop('play-a', 'SIGTERM');
  console.log(`BINGO-ROLLING replacement=play-replacement retired=${oldRoomEndpoint}`);
  return replacement;
}

async function waitForRouteReadiness(ctx, role, requiresPlacement = false) {
  await ctx.waitLog(role, 'bingo-room-status state=1 readyPeers=');
  if (requiresPlacement) await ctx.waitLog(role, 'placement=true');
}

async function waitPlayLog(ctx, marker) {
  await ctx.waitAnyLog([
    { name: 'play-b', marker },
    { name: 'play-replacement', marker }
  ]);
}

function assertPlayLogCount(ctx, marker, expected) {
  const actual = ['play-b', 'play-replacement'].reduce((count, role) => {
    const file = path.join(ctx.logDir, `${role}.log`);
    if (!fs.existsSync(file)) return count;
    return count + fs.readFileSync(file, 'utf8').split(marker).length - 1;
  }, 0);
  if (actual !== expected) {
    throw new Error(`play logs marker '${marker}' count was ${actual}; expected ${expected}.`);
  }
}
