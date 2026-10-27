const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');

const fixture = path.join(__dirname, 'fixtures/auto-connect-login-process.js');

test('Nest Session handler awaits public Spot creation after stream authentication in test mode', () => {
  const result = spawnSync(
    process.execPath,
    [fixture],
    {
      env: { ...process.env, NODE_ENV: 'test' },
      encoding: 'utf8'
    }
  );
  assert.equal(result.signal, null, result.stderr);
  assert.equal(result.status, 0, result.stderr);
  assert.match(result.stdout, /LOGIN_LOBBY_COMPLETE/u);
});

test('auto-connect login binds its WebSocket endpoint while another socket reserves the old candidate port', () => {
  const preload = path.join(os.tmpdir(), `zlink-reserve-unused-port-${process.pid}.cjs`);
  fs.writeFileSync(preload, `
    const net = require('node:net');
    const createServer = net.createServer;
    net.createServer = (...args) => {
      const server = createServer(...args);
      const close = server.close;
      server.close = (callback) => {
        const port = server.address().port;
        return close.call(server, () => {
          const reservation = createServer();
          reservation.listen(port, '127.0.0.1', () => {
            process.stderr.write('RESERVED_CANDIDATE_PORT\\n');
            callback?.();
            setTimeout(() => reservation.close(), 1000).unref();
          });
        });
      };
      return server;
    };
  `);
  try {
    const result = spawnSync(process.execPath, [fixture], {
      env: {
        ...process.env,
        NODE_ENV: 'test',
        NODE_OPTIONS: `${process.env.NODE_OPTIONS ?? ''} --require ${preload}`.trim()
      },
      encoding: 'utf8',
      timeout: 10_000
    });
    assert.equal(result.signal, null, result.stderr);
    assert.equal(result.status, 0, result.stderr);
    assert.match(result.stdout, /LOGIN_LOBBY_COMPLETE/u);
  } finally {
    fs.rmSync(preload, { force: true });
  }
});
