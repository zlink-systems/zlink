import { readConfig } from '../server-support/server-application';
import { runS2sChannelToSpotEchoTarget } from './s2s-channel-to-spot-echo-target';
import { runS2sSpotToChannelRequestEcho } from './s2s-spot-to-channel-request-echo-scenario';
import { runS2sSpotToChannelSendSendEcho } from './s2s-spot-to-channel-send-send-echo-scenario';
import { runSpotNoAwaitEcho } from './spot-no-await-echo-scenario';
import { runSpotWorkerOffloadEcho } from './spot-worker-offload-echo-scenario';

const { config } = readConfig(process.argv.slice(2));
if (config.role !== 'spot') throw new Error('SpotServer runs the spot role.');
const scenarios: Record<string, (() => Promise<void>) | undefined> = {
  's2s-channel-to-spot-request-echo': () => runS2sChannelToSpotEchoTarget(config),
  's2s-channel-to-spot-send-send-echo': () => runS2sChannelToSpotEchoTarget(config),
  's2s-spot-to-channel-request-echo': () => runS2sSpotToChannelRequestEcho(config),
  's2s-spot-to-channel-send-send-echo': () => runS2sSpotToChannelSendSendEcho(config),
  'spot-no-await-echo': () => runSpotNoAwaitEcho(config),
  'spot-worker-offload-echo': () => runSpotWorkerOffloadEcho(config)
};
const run = scenarios[config.scenario];
if (!run) throw new Error(`SpotServer does not run scenario ${config.scenario}.`);
run().catch((error: unknown) => {
  console.error(error);
  process.exit(1);
});
