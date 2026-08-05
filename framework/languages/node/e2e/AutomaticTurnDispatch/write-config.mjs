import fs from 'node:fs';

const [output, ...args] = process.argv.slice(2);
if (!output) throw new Error('Usage: node write-config.mjs <output> [--key value ...]');

const listKeys = new Set([
  'peerSpotRouteEndpoints',
  'playControlEndpoints',
  'playSpotRouteEndpoints'
]);
const peerKeys = new Set(['spotRouterPeers']);
const aliases = {
  peerSpotRouteEndpoint: 'peerSpotRouteEndpoints',
  playControlEndpoint: 'playControlEndpoints',
  playSpotRouteEndpoint: 'playSpotRouteEndpoints',
  spotRouterPeer: 'spotRouterPeers'
};
const e2e = {};
for (let index = 0; index < args.length; index += 2) {
  const flag = args[index];
  const value = args[index + 1];
  if (!flag?.startsWith('--') || value === undefined) throw new Error(`Invalid configuration argument '${flag}'.`);
  const parsedKey = flag.slice(2).replace(/-([a-z])/g, (_match, letter) => letter.toUpperCase());
  const key = aliases[parsedKey] ?? parsedKey;
  if (listKeys.has(key)) {
    e2e[key] = value.split(',').map((entry) => entry.trim()).filter(Boolean);
  } else if (peerKeys.has(key)) {
    e2e[key] = value.split(',').map((entry) => entry.trim()).filter(Boolean).map((entry) => {
      const separator = entry.indexOf('@');
      if (separator <= 0 || separator === entry.length - 1) throw new Error(`${flag} requires rid@endpoint entries.`);
      return { rid: entry.slice(0, separator), endpoint: entry.slice(separator + 1) };
    });
  } else {
    e2e[key] = value;
  }
}

fs.writeFileSync(output, `${JSON.stringify({ e2e }, null, 2)}\n`, { mode: 0o600 });
