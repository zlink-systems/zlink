import { readFileSync } from 'node:fs';

export const VERSION = JSON.parse(readFileSync(new URL('./etcd-version.json', import.meta.url), 'utf8'));
export const LIMITS = Object.freeze({
  portMin: 20000, portMax: 55000, portAttempts: 200,
  requestMs: 5000, readinessMs: 90000, pollMs: 200, commandMs: 180000,
  verificationMs: 540000, commandOutputBytes: 8388608, maximumPort: 65535, proxyBufferBytes: 65536,
  members: 3, maxTxnOps: 4096, maxRequestBytes: 16777216,
  compactionMode: 'periodic', compactionRetention: '10m',
  containerClientPort: 2379, containerPeerPort: 2380,
  maximumRevision: '9223372036854775807',
  label: 'dev.zlink.etcd.run', stateVersion: 1
});
