// SPDX-License-Identifier: MPL-2.0

'use strict';

const fs = require('node:fs');
const path = require('node:path');

let cachedAssets = null;

function isTlsTransport(transport) {
  return transport === 'tls' || transport === 'wss';
}

function resolveTlsAssets() {
  if (cachedAssets) {
    return cachedAssets;
  }
  let current = process.cwd();
  while (true) {
    const candidate = path.join(current, 'core', 'tests', 'certs', 'gen');
    const ca = path.join(candidate, 'ca.crt');
    const cert = path.join(candidate, 'server.crt');
    const key = path.join(candidate, 'server.key');
    if (fs.existsSync(ca) && fs.existsSync(cert) && fs.existsSync(key)) {
      cachedAssets = { ca, cert, key };
      return cachedAssets;
    }
    const parent = path.dirname(current);
    if (parent === current) {
      break;
    }
    current = parent;
  }
  throw new Error('TLS perf assets not found under core/tests/certs/gen');
}

function configureTlsServer(target, transport) {
  if (!isTlsTransport(transport)) {
    return;
  }
  const assets = resolveTlsAssets();
  target.setTlsServer(assets.cert, assets.key, false);
}

function configureTlsClient(target, transport, host = 'localhost') {
  if (!isTlsTransport(transport)) {
    return;
  }
  const assets = resolveTlsAssets();
  target.setTlsClient(assets.ca, host, false);
}

module.exports = {
  configureTlsClient,
  configureTlsServer,
  isTlsTransport,
  resolveTlsAssets
};
