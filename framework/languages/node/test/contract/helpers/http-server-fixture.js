/* SPDX-License-Identifier: Apache-2.0 */
'use strict';
const http = require('node:http');

function startServer(handler) {
  const server = http.createServer((req, res) => {
    Promise.resolve(handler(req, res)).catch(() => {
      try {
        res.statusCode = 500;
        res.end();
      } catch {
        /* ignore */
      }
    });
  });
  return listen(server);
}

function listen(server, isHttps = false) {
  return new Promise((resolve) => {
    server.listen(0, '127.0.0.1', () => {
      const { port } = server.address();
      resolve({
        server,
        baseUrl: `${isHttps ? 'https' : 'http'}://127.0.0.1:${port}`,
        close: () => new Promise((done) => server.close(() => done()))
      });
    });
  });
}

module.exports = { startServer, listen };
