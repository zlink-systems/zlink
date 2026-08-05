#!/usr/bin/env node
'use strict';

const pubEndpointMetadataKey = 'pub-endpoint';

const autoConnectTypes = new Set(['route-mesh', 'fanout']);
const roles = new Set(['router', 'pub', 'sub']);

function parseArgs(argv) {
  const options = {
    timeoutMs: 3000,
    intervalMs: 100,
    peers: []
  };
  for (let i = 0; i < argv.length;) {
    const arg = argv[i++];
    switch (arg) {
      case '--redis-endpoint':
        options.redisEndpoint = requireValue(argv, i++, arg);
        break;
      case '--key-prefix':
        options.keyPrefix = requireValue(argv, i++, arg);
        break;
      case '--timeout-ms':
        options.timeoutMs = Number(requireValue(argv, i++, arg));
        break;
      case '--interval-ms':
        options.intervalMs = Number(requireValue(argv, i++, arg));
        break;
      case '--peer': {
        const autoConnectType = parseSetValue(autoConnectTypes, requireValue(argv, i++, arg), 'auto-connect type');
        const meshName = requireValue(argv, i++, arg);
        const role = parseSetValue(roles, requireValue(argv, i++, arg), 'location role');
        const endpoints = [];
        while (i < argv.length && !argv[i].startsWith('--')) {
          endpoints.push(argv[i++]);
        }
        if (endpoints.length === 0) {
          throw new Error('--peer requires at least one endpoint.');
        }
        throw new Error('--peer is no longer supported; use --peer-http with a role server URL.');
      }
      case '--peer-http': {
        const autoConnectType = parseSetValue(autoConnectTypes, requireValue(argv, i++, arg), 'auto-connect type');
        const meshName = requireValue(argv, i++, arg);
        const role = parseSetValue(roles, requireValue(argv, i++, arg), 'location role');
        const httpUrl = requireValue(argv, i++, arg);
        const endpoints = [];
        while (i < argv.length && !argv[i].startsWith('--')) {
          endpoints.push(argv[i++]);
        }
        if (endpoints.length === 0) {
          throw new Error('--peer-http requires at least one endpoint.');
        }
        options.peers.push({ autoConnectType, meshName, role, httpUrl, endpoints });
        break;
      }
      default:
        throw new Error(`Unknown argument '${arg}'.`);
    }
  }
  if (options.redisEndpoint === undefined) {
    throw new Error('--redis-endpoint is required.');
  }
  if (options.keyPrefix === undefined) {
    throw new Error('--key-prefix is required.');
  }
  if (options.peers.length === 0) {
    throw new Error('At least one --peer entry is required.');
  }
  return options;
}

function requireValue(argv, index, flag) {
  const value = argv[index];
  if (value === undefined || value.startsWith('--')) {
    throw new Error(`${flag} requires a value.`);
  }
  return value;
}

function parseSetValue(values, text, label) {
  if (!values.has(text)) {
    throw new Error(`Unknown ${label} '${text}'.`);
  }
  return text;
}

async function main() {
  const options = parseArgs(process.argv.slice(2));
  await waitReady(options);
}

async function waitReady(options) {
  const deadline = Date.now() + options.timeoutMs;
  let lastState = { missing: [], rows: [], leases: [] };
  while (Date.now() < deadline) {
    lastState = await readinessState(options.peers);
    if (lastState.missing.length === 0) {
      console.log(JSON.stringify({ topologyReady: true, rows: lastState.rows }));
      return;
    }
    await delay(options.intervalMs);
  }
  console.error('Timed out waiting for Redis location topology readiness.');
  console.error(JSON.stringify(lastState, (_key, value) =>
    typeof value === 'bigint' ? value.toString() : value, 2));
  process.exitCode = 1;
}

async function readinessState(expectedPeers) {
  const missing = [];
  const rows = [];
  for (const expected of expectedPeers) {
    const response = await fetch(`${expected.httpUrl}/location/topology`);
    if (!response.ok) {
      throw new Error(`Location topology endpoint '${expected.httpUrl}' returned HTTP ${response.status}.`);
    }
    const document = await response.json();
    const currentRows = Array.isArray(document?.items) ? document.items : [];
    rows.push({
      httpUrl: expected.httpUrl,
      topology: currentRows,
      route: document?.route
    });
    // ZLinkLocationTopologyState.Ready is the public numeric enum value 3.
    const liveRows = currentRows.filter((row) => row?.state === 3 && row?.draining !== true);
    for (const endpoint of expected.endpoints) {
      if (!liveRows.some((row) => row.endpoint === endpoint || row.metadata?.[pubEndpointMetadataKey] === endpoint)) {
        missing.push(`${expected.httpUrl}/${expected.meshName}@${endpoint}`);
      }
    }
  }
  return { missing, rows };
}

function delay(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
