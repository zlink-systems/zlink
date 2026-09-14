'use strict';
// One dispatcher covers each documented scenario; terminal/topology/Spot count are role config values.
const catalog = Object.freeze({
  'cs-local-session-actor-echo': { cs: true, actor: true },
  'cs-remote-session-actor-echo': { cs: true, actor: true },
  'session-echo-only': { cs: true },
  'channel-echo-only': { channel: true },
  's2s-channel-to-spot-request-echo': { spot: true },
  's2s-channel-to-spot-send-send-echo': { spot: true, correlated: true },
  's2s-channel-to-spot-send': { spot: true, oneWay: true },
  's2s-spot-to-channel-request-echo': { driver: true, spot: true, channel: true },
  's2s-spot-to-channel-send-send-echo': { driver: true, spot: true, channel: true, correlated: true },
  's2s-spot-to-channel-send': { driver: true, spot: true, channel: true, oneWay: true },
  'spot-no-await-echo': { spot: true, local: true },
  'spot-worker-offload-echo': { spot: true, local: true, worker: true },
  'actor-no-bind-request-echo': { actor: true },
  'actor-no-bind-send-send-echo': { actor: true, correlated: true },
  'actor-no-bind-send': { actor: true, oneWay: true },
  'pubsub-fanout-echo': { publish: true }
});
function scenario(config) {
  const value = catalog[config.scenario];
  if (!value) throw new Error(`Unsupported scenario ${config.scenario}.`);
  if (config.terminal !== 'ordinary' && config.terminal !== 'yield') throw new Error('terminal must be ordinary/yield.');
  if (config.terminal === 'yield' && !value.worker && !(value.driver && !value.correlated && !value.oneWay)) throw new Error('Yield is not eligible for this scenario.');
  return value;
}
module.exports = { catalog, scenario };
