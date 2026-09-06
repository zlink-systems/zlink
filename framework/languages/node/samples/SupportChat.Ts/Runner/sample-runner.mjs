import fs from 'node:fs';
import path from 'node:path';

export const sampleName = 'SupportChat.Ts';

const waitIntervalMs = 100;
const waitAttempts = 300;

export async function runSample(ctx) {
  const logDir = path.join(ctx.logDir, 'flow');
  fs.mkdirSync(logDir, { recursive: true });
  const apiChannelEndpoint = `tcp://127.0.0.1:${await ctx.port()}`;
  const supportSpotEndpoint = `tcp://127.0.0.1:${await ctx.port()}`;
  const sessionSpotEndpoint = `tcp://127.0.0.1:${await ctx.port()}`;
  const sessionStreamEndpoint = `ws://127.0.0.1:${await ctx.port()}`;
  const redisKeyPrefix = `supportchat:node:${process.pid}:`;
  const common = { redisEndpoint: ctx.redisEndpoint, redisKeyPrefix, logDir };
  const supportConfig = ctx.writeConfig('support', {
    ...common, supportSpotEndpoint
  });
  const apiConfig = ctx.writeConfig('api', { ...common, apiChannelEndpoint });
  const sessionConfig = ctx.writeConfig('session', {
    ...common, sessionSpotEndpoint, sessionStreamEndpoint
  });
  await ctx.start('support', 'dist/Server/Support/main.js', ['--config', supportConfig]);
  await waitForLog(ctx, 'support', 'supportchat-ready kind=public node=support');
  await ctx.start('api', 'dist/Server/Api/main.js', ['--config', apiConfig]);
  await waitForLog(ctx, 'api', 'supportchat-ready kind=public node=api');
  await waitForLog(
    ctx,
    'api',
    'supportchat-ready kind=spot-route node=api mesh=supportchat-conversations'
  );
  await ctx.start('session', 'dist/Server/Session/main.js', ['--config', sessionConfig]);
  await waitForLog(ctx, 'session', 'supportchat-ready kind=stream node=session');
  await waitForLog(
    ctx,
    'session',
    'supportchat-ready kind=spot-route node=session mesh=supportchat-conversations'
  );
  await ctx.runBrowser({
    timeoutMs: 90_000,
    config: { sessionStreamEndpoint },
    proxies: []
  });
  const clientMarkers = [
    'supportchat=completed',
    'supportchat-closed-typing-ignore=verified'
  ];
  for (const marker of clientMarkers) await waitForLog(ctx, 'browser-client', marker);
  for (const marker of clientMarkers) ctx.assertLogCount('browser-client', marker, 1);

  const serverMarkers = [
    'supportchat-conversation created conversation=',
    'supportchat-conversation agent-joined conversation=',
    'supportchat-conversation status=WaitingForAgent conversation=',
    'supportchat-conversation status=Active conversation=',
    'supportchat-conversation status=WaitingForClose conversation=',
    'supportchat-conversation status=Closed conversation='
  ];
  for (const marker of serverMarkers) await waitForCombinedLog(ctx, ['api', 'support'], marker);
  for (const marker of serverMarkers) assertCombinedLogAtLeast(ctx, ['api', 'support'], marker, 1);
  console.log('supportchat-placement=completed');
}

async function waitForLog(ctx, role, marker) {
  const target = path.join(ctx.logDir, `${role}.log`);
  for (let attempt = 0; attempt < waitAttempts; attempt += 1) {
    if (fs.existsSync(target) && fs.readFileSync(target, 'utf8').includes(marker)) return;
    await new Promise((resolve) => setTimeout(resolve, waitIntervalMs));
  }
  throw new Error(`Timed out waiting for ${role} log marker '${marker}' after ${waitAttempts} attempts.`);
}

async function waitForCombinedLog(ctx, roles, marker) {
  for (let attempt = 0; attempt < waitAttempts; attempt += 1) {
    if (combinedLogCount(ctx, roles, marker) > 0) return;
    await new Promise((resolve) => setTimeout(resolve, waitIntervalMs));
  }
  throw new Error(`Timed out waiting for combined log marker '${marker}' after ${waitAttempts} attempts.`);
}

function assertCombinedLogAtLeast(ctx, roles, marker, minimum) {
  const actual = combinedLogCount(ctx, roles, marker);
  if (actual < minimum) {
    throw new Error(`Combined log marker '${marker}' count was ${actual}; expected at least ${minimum}.`);
  }
}

function combinedLogCount(ctx, roles, marker) {
  return roles.reduce((count, role) => {
    const target = path.join(ctx.logDir, `${role}.log`);
    const content = fs.existsSync(target) ? fs.readFileSync(target, 'utf8') : '';
    return count + content.split(marker).length - 1;
  }, 0);
}
