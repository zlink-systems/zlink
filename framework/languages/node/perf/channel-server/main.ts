import { readConfig } from '../server-support/server-application';
import { runChannelEchoOnly } from './channel-echo-only-host';
import { runS2sChannelToSpotRequestEcho } from './s2s-channel-to-spot-request-echo-scenario';
import { runS2sChannelToSpotSendSendEcho } from './s2s-channel-to-spot-send-send-echo-scenario';
import { runS2sSpotToChannelRequestEchoTarget } from './s2s-spot-to-channel-request-echo-target';
import { runS2sSpotToChannelSendSendEchoTarget } from './s2s-spot-to-channel-send-send-echo-target';

const { config } = readConfig(process.argv.slice(2));
const scenarios: Record<string, (() => Promise<void>) | undefined> = {
  'channel-echo-only': () => runChannelEchoOnly(config),
  's2s-channel-to-spot-request-echo': () => runS2sChannelToSpotRequestEcho(config),
  's2s-channel-to-spot-send-send-echo': () => runS2sChannelToSpotSendSendEcho(config),
  's2s-spot-to-channel-request-echo': () => runS2sSpotToChannelRequestEchoTarget(config),
  's2s-spot-to-channel-send-send-echo': () => runS2sSpotToChannelSendSendEchoTarget(config)
};
const run = scenarios[config.scenario];
if (!run) throw new Error(`ChannelServer does not run scenario ${config.scenario}.`);
run().catch((error: unknown) => {
  console.error(error);
  process.exit(1);
});
