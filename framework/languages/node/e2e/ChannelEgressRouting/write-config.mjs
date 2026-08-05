import fs from 'node:fs';

const [output, ...args] = process.argv.slice(2);
if (!output) throw new Error('Usage: node write-config.mjs <output> [--key value ...]');

const listKeys = new Set([
  'gamePeers', 'auditPeers', 'workflowPeers',
  'gameServers', 'gameClients', 'auditServers', 'auditClients'
]);
const booleanKeys = new Set(['workflowClient', 'workflowServer']);
const aliases = {
  gamePeer: 'gamePeers',
  auditPeer: 'auditPeers',
  workflowPeer: 'workflowPeers',
  gameServer: 'gameServers',
  gameClient: 'gameClients',
  auditServer: 'auditServers',
  auditClient: 'auditClients'
};

const e2e = {};
for (let index = 0; index < args.length; index += 2) {
  const flag = args[index];
  const value = args[index + 1];
  if (!flag?.startsWith('--') || value === undefined) {
    throw new Error(`Invalid configuration argument '${flag}'.`);
  }
  const parsed = flag.slice(2).replace(/-([a-z])/g, (_match, letter) => letter.toUpperCase());
  const key = aliases[parsed] ?? parsed;
  if (listKeys.has(key)) {
    (e2e[key] ??= []).push(value);
  } else if (booleanKeys.has(key)) {
    if (value !== 'true' && value !== 'false') {
      throw new Error(`Boolean option '${flag}' must be true or false.`);
    }
    e2e[key] = value === 'true';
  } else if (/^-?\d+$/.test(value)) {
    e2e[key] = Number(value);
  } else {
    e2e[key] = value;
  }
}

fs.writeFileSync(output, `${JSON.stringify({ e2e }, null, 2)}\n`, { mode: 0o600 });
