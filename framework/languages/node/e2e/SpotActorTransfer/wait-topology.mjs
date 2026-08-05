#!/usr/bin/env node

const args = process.argv.slice(2);
let timeoutMs = 5_000;
let intervalMs = 100;
const urls = [];
for (let index = 0; index < args.length;) {
  const value = args[index++];
  if (value === '--timeout-ms') {
    timeoutMs = Number(args[index++]);
  } else if (value === '--interval-ms') {
    intervalMs = Number(args[index++]);
  } else {
    urls.push(value);
  }
}

if (urls.length < 2 || !Number.isFinite(timeoutMs) || !Number.isFinite(intervalMs)) {
  throw new Error('Usage: wait-topology.mjs [--timeout-ms N] [--interval-ms N] URL...');
}

const expectedPeerCount = urls.length - 1;
const deadline = Date.now() + timeoutMs;
let lastSnapshots = [];
while (Date.now() < deadline) {
  try {
    lastSnapshots = await Promise.all(urls.map(async url => {
      const response = await fetch(`${url}/mesh-snapshot`);
      if (!response.ok) throw new Error(`${url} returned ${response.status}`);
      return await response.json();
    }));
    const expectedRids = new Set(lastSnapshots.map(snapshot => snapshot.rid));
    if (
      expectedRids.size === urls.length
      && lastSnapshots.every(snapshot =>
        snapshot.ready === true
        && expectedRids.has(snapshot.rid)
        && snapshot.readyPeerRids.length === expectedPeerCount
        && snapshot.readyPeerRids.every(rid => expectedRids.has(rid) && rid !== snapshot.rid)
      )
    ) {
      process.exit(0);
    }
  } catch {
    // A host can become healthy before its topology endpoint returns a snapshot.
  }
  await new Promise(resolve => setTimeout(resolve, intervalMs));
}

console.error('Timed out waiting for public RouteMesh snapshots.');
console.error(JSON.stringify(lastSnapshots, null, 2));
process.exit(1);
