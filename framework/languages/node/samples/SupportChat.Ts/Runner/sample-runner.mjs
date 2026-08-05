import fs from 'node:fs';
import path from 'node:path';

export const sampleName = 'SupportChat.Ts';

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
  await ctx.waitTcp(supportSpotEndpoint);
  await ctx.start('api', 'dist/Server/Api/main.js', ['--config', apiConfig]);
  await ctx.waitTcp(apiChannelEndpoint);
  await ctx.start('session', 'dist/Server/Session/main.js', ['--config', sessionConfig]);
  await ctx.waitTcp(sessionStreamEndpoint);
  ctx.runBrowser({
    timeoutMs: 90_000,
    config: { sessionStreamEndpoint },
    proxies: []
  });
}
