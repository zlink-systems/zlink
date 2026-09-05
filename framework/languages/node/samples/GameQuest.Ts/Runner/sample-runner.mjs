import fs from 'node:fs';

export const sampleName = 'GameQuest.Ts';

const attempts = 300;
const pollIntervalMs = 100;

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
  for (const [role, entry, configPath] of [
    ['mission-a', 'dist/Server/MissionA/main.js',
      roleConfig('mission-a', ['missionAEndpoint', 'missionASpotEndpoint', 'missionASpotRouterEndpoint', 'missionAHttpUrl', 'redisEndpoint', 'redisKeyPrefix', 'logDir', 'workDir'])],
    ['mission-b', 'dist/Server/MissionB/main.js',
      roleConfig('mission-b', ['missionBEndpoint', 'missionBSpotEndpoint', 'missionBSpotRouterEndpoint', 'missionBHttpUrl', 'redisEndpoint', 'redisKeyPrefix', 'logDir', 'workDir'])],
    ['api-a', 'dist/Server/ApiA/main.js',
      roleConfig('api-a', ['apiAHttpUrl', 'apiAStreamEndpoint', 'apiAActorSpotEndpoint', 'redisEndpoint', 'redisKeyPrefix', 'logDir', 'workDir'])],
    ['api-b', 'dist/Server/ApiB/main.js',
      roleConfig('api-b', ['apiBHttpUrl', 'apiBStreamEndpoint', 'apiBActorSpotEndpoint', 'redisEndpoint', 'redisKeyPrefix', 'logDir', 'workDir'])]
  ]) {
    await ctx.start(role, entry, ['--config', configPath]);
  }

  await waitForLog(ctx, 'mission-a', 'gamequest-ready kind=instance-factory node=mission-a');
  await waitForLog(ctx, 'mission-b', 'gamequest-ready kind=instance-factory node=mission-b');
  await waitForLog(ctx, 'api-a', 'gamequest-ready kind=stream node=api-a');
  await waitForLog(ctx, 'api-b', 'gamequest-ready kind=stream node=api-b');
  await waitForLog(
    ctx,
    'api-a',
    'gamequest-ready kind=spot-route node=api-a mesh=gamequest.player-quest.spot'
  );
  await waitForLog(
    ctx,
    'api-b',
    'gamequest-ready kind=spot-route node=api-b mesh=gamequest.player-quest.spot'
  );

  const browser = ctx.startBrowser({
    timeoutMs: 120_000,
    config: {
      apiAHttpUrl: '/api/gamequest/api-a',
      apiBHttpUrl: '/api/gamequest/api-b',
      apiAStreamEndpoint: sample.apiAStreamEndpoint,
      apiBStreamEndpoint: sample.apiBStreamEndpoint,
      missionAHttpUrl: '/api/gamequest/mission-a',
      missionBHttpUrl: '/api/gamequest/mission-b',
      lifecycleCompletionPath: '/runner/owner-termination'
    },
    proxies: [
      { prefix: '/api/gamequest/api-a', target: sample.apiAHttpUrl },
      { prefix: '/api/gamequest/api-b', target: sample.apiBHttpUrl },
      { prefix: '/api/gamequest/mission-a', target: sample.missionAHttpUrl },
      { prefix: '/api/gamequest/mission-b', target: sample.missionBHttpUrl }
    ]
  });
  await ctx.waitLog('browser-client', 'gamequest-owner awaiting-termination player=player-alice');
  await waitCombinedLog(ctx, ['mission-a', 'mission-b'], 'gamequest-owner ready player=player-alice node=');
  const owner = ownerRole(ctx, 'player-alice');
  await ctx.stop(owner, 'SIGKILL');
  await browser.complete();

  await verifyEvidence(ctx);
  console.log('gamequest-placement=completed');
}

async function verifyEvidence(ctx) {
  //  Section 10.1: counted across both node logs against a lower bound. An actor send handler runs
  //  on the node where the actor lives, not where the stream arrived, so per-node counts are
  //  unsatisfiable. Summing (rather than "either file matches") still catches a lost flow.
  await waitCombinedLog(ctx, ['api-a', 'api-b'], 'gamequest-api event-routed player=');
  assertCombinedMinimum(ctx, ['api-a', 'api-b'], 'gamequest-api event-routed player=', 4);
  await waitCombinedLog(ctx, ['mission-a', 'mission-b'], 'gamequest-mission processed player=');
  assertCombinedMinimum(ctx, ['mission-a', 'mission-b'], 'gamequest-mission processed player=', 4);
  for (const playerId of ['player-alice', 'player-bob']) {
    const marker = `gamequest-mission reconciled player=${playerId} quest=first-hunt`;
    await waitCombinedLog(ctx, ['mission-a', 'mission-b'], marker);
    assertCombinedCount(ctx, ['mission-a', 'mission-b'], marker, 1);
  }
  const replay = 'gamequest-mission replayed player=player-alice generation=';
  await waitCombinedLog(ctx, ['mission-a', 'mission-b'], replay);
  assertCombinedCount(ctx, ['mission-a', 'mission-b'], replay, 1);
  //  Which surviving Api node receives the post-kill call is placement-dependent, so count across
  //  both Api logs rather than pinning one.
  await waitCombinedLog(ctx, ['api-a', 'api-b'], 'gamequest-owner unavailable player=player-alice');
  assertCombinedCount(ctx, ['api-a', 'api-b'], 'gamequest-owner unavailable player=player-alice', 1);
  assertCombinedCount(ctx, ['mission-a', 'mission-b'], 'gamequest-owner replacement-handler-invoked player=player-alice', 0);
  ctx.assertLogCount('browser-client', 'gamequest=completed', 1);
  ctx.assertLogCount('browser-client', 'gamequest-server-evidence=completed', 1);
}

function ownerRole(ctx, playerId) {
  const marker = `gamequest-owner ready player=${playerId} node=`;
  const owners = ['mission-a', 'mission-b'].filter((role) => logText(ctx, role).includes(`${marker}${role}`));
  if (owners.length !== 1) throw new Error(`Expected exactly one ready Mission owner for '${playerId}', found ${owners.join(', ') || 'none'}.`);
  return owners[0];
}

async function waitCombinedLog(ctx, roles, marker) {
  for (let attempt = 0; attempt < attempts; attempt += 1) {
    if (roles.some((role) => logText(ctx, role).includes(marker))) return;
    await new Promise((resolve) => setTimeout(resolve, pollIntervalMs));
  }
  throw new Error(`Timed out waiting for '${marker}'.`);
}

async function waitForLog(ctx, role, marker) {
  for (let attempt = 0; attempt < attempts; attempt += 1) {
    if (logText(ctx, role).includes(marker)) return;
    await new Promise((resolve) => setTimeout(resolve, pollIntervalMs));
  }
  throw new Error(`Timed out waiting for ${role} log marker '${marker}' after ${attempts} attempts.`);
}

function assertMinimum(ctx, role, marker, minimum) {
  const actual = count(logText(ctx, role), marker);
  if (actual < minimum) throw new Error(`${role} log marker '${marker}' count was ${actual}; expected at least ${minimum}.`);
}

function assertCombinedMinimum(ctx, roles, marker, minimum) {
  const actual = roles.reduce((countValue, role) => countValue + count(logText(ctx, role), marker), 0);
  if (actual < minimum) throw new Error(`Combined log marker '${marker}' count was ${actual}; expected at least ${minimum}.`);
}

function assertCombinedCount(ctx, roles, marker, expected) {
  const actual = roles.reduce((countValue, role) => countValue + count(logText(ctx, role), marker), 0);
  if (actual !== expected) throw new Error(`Combined log marker '${marker}' count was ${actual}; expected ${expected}.`);
}

function logText(ctx, role) {
  const file = `${ctx.logDir}/${role}.log`;
  return fs.existsSync(file) ? fs.readFileSync(file, 'utf8') : '';
}

function count(text, marker) {
  return text.split(marker).length - 1;
}
