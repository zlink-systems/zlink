// SPDX-License-Identifier: MPL-2.0

'use strict';

const net = require('node:net');
const os = require('node:os');
const path = require('node:path');
const { once } = require('node:events');

async function reservePort() {
  const server = net.createServer();
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const address = server.address();
  await new Promise<void>((resolve, reject) => server.close((error) => error ? reject(error) : resolve()));
  return address.port;
}

async function benchmarkEndpoint(
  transport,
  token,
  options: { suite?: string; bindPort?: number | string } = {}
) {
  const suite = options.suite || 'single';
  if (transport === 'inproc' && suite === 'single') {
    return `inproc://perf-${token}-${process.pid}`;
  }
  if (transport === 'ipc') {
    const prefix = suite === 'multi' ? 'zlink-node-multi-perf' : 'zlink-node-perf';
    return `ipc://${path.join(os.tmpdir(), `${prefix}-${process.pid}-${token}.sock`)}`;
  }
  if (transport === 'tcp' || transport === 'tls' || transport === 'ws' || transport === 'wss') {
    const bindPort = Number(options.bindPort);
    const port = Number.isFinite(bindPort) && bindPort > 0
      ? Math.trunc(bindPort)
      : await reservePort();
    return `${transport}://127.0.0.1:${port}`;
  }
  throw new Error(`unsupported ${suite} transport: ${transport}`);
}

module.exports = {
  benchmarkEndpoint,
  reservePort
};
