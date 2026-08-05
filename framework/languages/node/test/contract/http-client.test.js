/* SPDX-License-Identifier: Apache-2.0 */
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const http = require('node:http');
const https = require('node:https');
const net = require('node:net');
const zlib = require('node:zlib');
const fs = require('node:fs');
const path = require('node:path');

const { ZLinkHttpClient } = require('../../packages/http-client/dist');
const { ZLinkFrameworkException, ZLinkFrameworkErrorKind } = require('../../packages/framework/dist');
const frameworkInternal = require('../../packages/framework/dist/internal');
const frameworkIntegration = require('../../packages/framework/dist/nest-integration');

const TLS_DIR = path.join(__dirname, '..', 'fixtures', 'tls');
const tlsPath = (name) => path.join(TLS_DIR, name);

test('standalone HTTP request exposes async response terminators but no server terminators', async () => {
  const server = await startServer((_req, res) => {
    res.setHeader('content-type', 'application/json');
    res.end(JSON.stringify({ ok: true }));
  });
  const client = ZLinkHttpClient.create(server.baseUrl).build();
  try {
    const call = client.get('/terminators');
    assert.equal(typeof call.async, 'function');
    assert.equal(typeof call.fetch, 'function');
    assert.equal(typeof call.submitRaw, 'function');
    assert.equal(typeof call.submit, 'undefined');
    assert.equal(typeof call.yield, 'undefined');
  } finally {
    await client.close();
    await server.close();
  }
});

test('server HTTP one-way submit returns Promise<void>', async () => {
  const server = await startServer((_req, res) => {
    res.end();
  });
  const scheduler = frameworkIntegration.createIntegrationHttpExecutionScheduler({});
  const client = ZLinkHttpClient.create(server.baseUrl).executionScheduler(scheduler).build();
  try {
    const submission = client.post('/one-way').submit();
    assert.equal(submission instanceof Promise, true);
    assert.equal(await submission, undefined);
  } finally {
    await client.close();
    await server.close();
  }
});

test('HTTP async retains a Spot turn while yield releases and resumes it', async () => {
  const releases = [];
  const server = await startServer(async (_req, res) => {
    await new Promise((resolve) => releases.push(resolve));
    res.setHeader('content-type', 'application/json');
    res.end(JSON.stringify({ ok: true }));
  });
  const runtimeErrors = [];
  const scheduler = frameworkIntegration.createIntegrationHttpExecutionScheduler({
    errorSink: { reportRuntimeTaskException: (_task, error) => runtimeErrors.push(error) }
  });
  const client = ZLinkHttpClient.create(server.baseUrl).executionScheduler(scheduler).build();
  const serial = new frameworkInternal.ZLinkSpotSerialExecutor();
  try {
    const retainedEvents = [];
    const retained = serial.execute(async () => {
      retainedEvents.push('handler:start');
      await client.get('/retained').async();
      retainedEvents.push('handler:reply');
    });
    const retainedNext = serial.execute(() => retainedEvents.push('next'));
    await waitFor(() => releases.length === 1);
    assert.deepEqual(retainedEvents, ['handler:start']);
    releases.shift()();
    await Promise.all([retained, retainedNext]);
    assert.deepEqual(retainedEvents, ['handler:start', 'handler:reply', 'next']);

    const yieldedEvents = [];
    const yielded = serial.execute(async () => {
      yieldedEvents.push('handler:start');
      await client.get('/yielded').yield();
      yieldedEvents.push('handler:reply');
    });
    const yieldedNext = serial.execute(() => yieldedEvents.push('next'));
    await waitFor(() => releases.length === 1);
    assert.deepEqual(yieldedEvents, ['handler:start', 'next']);
    releases.shift()();
    await Promise.all([yielded, yieldedNext]);
    assert.deepEqual(yieldedEvents, ['handler:start', 'next', 'handler:reply']);
    assert.deepEqual(runtimeErrors, []);
  } finally {
    await client.close();
    await server.close();
  }
});

test('HTTP callback completion is posted as a new Spot turn', async () => {
  const server = await startServer((_req, res) => {
    res.setHeader('content-type', 'application/json');
    res.end(JSON.stringify({ ok: true }));
  });
  const scheduler = frameworkIntegration.createIntegrationHttpExecutionScheduler({});
  const client = ZLinkHttpClient.create(server.baseUrl).executionScheduler(scheduler).build();
  const serial = new frameworkInternal.ZLinkSpotSerialExecutor();
  const events = [];
  let releaseTurn;
  const turnGate = new Promise((resolve) => { releaseTurn = resolve; });
  let callbackDone;
  const callback = new Promise((resolve) => { callbackDone = resolve; });
  try {
    const busy = serial.execute(async () => {
      events.push('handler:start');
      client.get('/callback').async((error, response) => {
        assert.equal(error, undefined);
        assert.equal(response.body.ok, true);
        events.push('callback');
        callbackDone();
      });
      await turnGate;
      events.push('handler:end');
    });
    await new Promise((resolve) => setTimeout(resolve, 20));
    assert.deepEqual(events, ['handler:start']);
    releaseTurn();
    await Promise.all([busy, callback]);
    assert.deepEqual(events, ['handler:start', 'handler:end', 'callback']);
  } finally {
    await client.close();
    await server.close();
  }
});

async function waitFor(predicate) {
  const deadline = Date.now() + 1000;
  while (!predicate()) {
    if (Date.now() >= deadline) throw new Error('condition timed out');
    await new Promise((resolve) => setTimeout(resolve, 1));
  }
}

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

function startTlsServer(handler, { requireClientCert = false } = {}) {
  const server = https.createServer(
    {
      cert: fs.readFileSync(tlsPath('server-cert.pem')),
      key: fs.readFileSync(tlsPath('server-key.pem')),
      requestCert: requireClientCert,
      rejectUnauthorized: false,
    },
    (req, res) => {
      handler(req, res);
    },
  );
  return listen(server, true);
}

function listen(server, isHttps = false) {
  return new Promise((resolve) => {
    server.listen(0, '127.0.0.1', () => {
      const { port } = server.address();
      resolve({
        baseUrl: `${isHttps ? 'https' : 'http'}://127.0.0.1:${port}`,
        close: () => new Promise((done) => server.close(() => done())),
      });
    });
  });
}

function readBody(req) {
  return new Promise((resolve) => {
    const chunks = [];
    req.on('data', (c) => chunks.push(c));
    req.on('end', () => resolve(Buffer.concat(chunks).toString('utf8')));
  });
}

test('dispatches each HTTP method', async () => {
  const seen = [];
  const server = await startServer((req, res) => {
    seen.push(req.method);
    res.end('{}');
  });
  const client = ZLinkHttpClient.create(server.baseUrl).build();
  try {
    for (const verb of ['get', 'post', 'put', 'delete', 'patch', 'options']) {
      const r = await client[verb]('/r').submitRaw();
      assert.equal(r.status, 200);
    }
    assert.deepEqual(seen, ['GET', 'POST', 'PUT', 'DELETE', 'PATCH', 'OPTIONS']);
  } finally {
    await client.close();
    await server.close();
  }
});

test('HEAD returns status with empty body', async () => {
  const server = await startServer((req, res) => {
    res.setHeader('x-marker', 'present');
    res.end();
  });
  const client = ZLinkHttpClient.create(server.baseUrl).build();
  try {
    const r = await client.head('/r').submitRaw();
    assert.equal(r.status, 200);
    assert.equal(r.body, '');
    assert.equal(r.headers['x-marker'], 'present');
  } finally {
    await client.close();
    await server.close();
  }
});

test('typed async returns null for successful empty body responses', async () => {
  const server = await startServer((req, res) => {
    res.statusCode = 204;
    res.end();
  });
  const client = ZLinkHttpClient.create(server.baseUrl).build();
  try {
    const r = await client.get('/empty').async();
    assert.equal(r.status, 204);
    assert.equal(r.body, null);
    assert.equal(r.rawBody, '');
  } finally {
    await client.close();
    await server.close();
  }
});

test('percent-encodes query parameters', async () => {
  let rawUrl;
  const server = await startServer((req, res) => {
    rawUrl = req.url;
    res.end('{}');
  });
  const client = ZLinkHttpClient.create(server.baseUrl).build();
  try {
    await client.get('/search').query('q', 'a b&c').query('k', 'v/w').submitRaw();
    assert.equal(rawUrl, '/search?q=a%20b%26c&k=v%2Fw');
  } finally {
    await client.close();
    await server.close();
  }
});

test('sends default and request headers', async () => {
  let auth;
  let trace;
  const server = await startServer((req, res) => {
    auth = req.headers['authorization'];
    trace = req.headers['x-trace'];
    res.end('{}');
  });
  const client = ZLinkHttpClient.create(server.baseUrl).defaultHeader('authorization', 'Bearer token-123').build();
  try {
    await client.get('/r').header('x-trace', 'abc').submitRaw();
    assert.equal(auth, 'Bearer token-123');
    assert.equal(trace, 'abc');
  } finally {
    await client.close();
    await server.close();
  }
});

test('typed JSON round trip', async () => {
  let received;
  const server = await startServer(async (req, res) => {
    received = JSON.parse(await readBody(req));
    res.setHeader('content-type', 'application/json');
    res.end(JSON.stringify({ id: 'game-7', ranked: true }));
  });
  const client = ZLinkHttpClient.create(server.baseUrl).build();
  try {
    const body = await client.post('/games').body({ name: 'ranked-0611' }).fetch();
    assert.equal(received.name, 'ranked-0611');
    assert.equal(body.id, 'game-7');
    assert.equal(body.ranked, true);
  } finally {
    await client.close();
    await server.close();
  }
});

test('raw body sets content type', async () => {
  let contentType;
  let body;
  const server = await startServer(async (req, res) => {
    contentType = req.headers['content-type'];
    body = await readBody(req);
    res.end('{}');
  });
  const client = ZLinkHttpClient.create(server.baseUrl).build();
  try {
    await client.post('/raw').body('plain text', 'text/plain').submitRaw();
    assert.equal(body, 'plain text');
    assert.equal(contentType, 'text/plain');
  } finally {
    await client.close();
    await server.close();
  }
});

test('form-urlencoded body', async () => {
  let contentType;
  let body;
  const server = await startServer(async (req, res) => {
    contentType = req.headers['content-type'];
    body = await readBody(req);
    res.end('{}');
  });
  const client = ZLinkHttpClient.create(server.baseUrl).build();
  try {
    await client.post('/form').form('name', 'a b').form('city', 'x/y').submitRaw();
    assert.equal(contentType, 'application/x-www-form-urlencoded');
    assert.equal(body, 'name=a%20b&city=x%2Fy');
  } finally {
    await client.close();
    await server.close();
  }
});

test('multipart body', async () => {
  let contentType;
  let body;
  const server = await startServer(async (req, res) => {
    contentType = req.headers['content-type'];
    body = await readBody(req);
    res.end('{}');
  });
  const client = ZLinkHttpClient.create(server.baseUrl).build();
  try {
    await client
      .post('/upload')
      .multipart('field', 'value')
      .multipartFile('file', 'a.txt', 'file-content', 'text/plain')
      .submitRaw();
    assert.ok(contentType.startsWith('multipart/form-data; boundary='));
    assert.ok(body.includes('name="field"'));
    assert.ok(body.includes('filename="a.txt"'));
    assert.ok(body.includes('file-content'));
  } finally {
    await client.close();
    await server.close();
  }
});

test('streaming upload sends all chunks', async () => {
  const captured = await new Promise((resolve) => {
    const server = net.createServer((socket) => {
      let buf = '';
      socket.on('data', (d) => {
        buf += d.toString('latin1');
        if (buf.includes('0\r\n\r\n')) {
          socket.write('HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}');
          socket.end();
          const bodyStart = buf.indexOf('\r\n\r\n') + 4;
          resolve({ raw: buf.slice(bodyStart), server });
        }
      });
    });
    server.listen(0, '127.0.0.1', async () => {
      const { port } = server.address();
      const client = ZLinkHttpClient.create(`http://127.0.0.1:${port}`).build();
      const chunks = ['part-1;', 'part-2;', 'part-3'].map((s) => Buffer.from(s));
      let i = 0;
      await client.post('/s').bodyStream(() => (i < chunks.length ? new Uint8Array(chunks[i++]) : null), 'application/octet-stream').submitRaw();
      await client.close();
    });
  });
  // de-chunk the captured body
  let body = '';
  let rest = captured.raw;
  for (;;) {
    const nl = rest.indexOf('\r\n');
    const size = parseInt(rest.slice(0, nl), 16);
    if (size === 0) break;
    body += rest.slice(nl + 2, nl + 2 + size);
    rest = rest.slice(nl + 2 + size + 2);
  }
  await new Promise((done) => captured.server.close(() => done()));
  assert.equal(body, 'part-1;part-2;part-3');
});

test('streaming download delivers chunks to sink', async () => {
  const server = await startServer((req, res) => {
    res.setHeader('content-type', 'text/plain');
    res.end('streamed-response-payload');
  });
  const client = ZLinkHttpClient.create(server.baseUrl).build();
  try {
    let sink = '';
    const r = await client.get('/download').download((chunk) => {
      sink += Buffer.from(chunk).toString('utf8');
    });
    assert.equal(r.status, 200);
    assert.equal(r.body, '');
    assert.equal(sink, 'streamed-response-payload');
  } finally {
    await client.close();
    await server.close();
  }
});

test('status >= 400 throws requestFailed', async () => {
  const server = await startServer((req, res) => {
    res.statusCode = 404;
    res.end(JSON.stringify({ error: 'missing' }));
  });
  const client = ZLinkHttpClient.create(server.baseUrl).build();
  try {
    await assert.rejects(
      () => client.get('/players/0').async(),
      (e) => e instanceof ZLinkFrameworkException && e.kind === ZLinkFrameworkErrorKind.Unavailable,
    );
  } finally {
    await client.close();
    await server.close();
  }
});

test('malformed JSON throws payloadDecodeFailed', async () => {
  const server = await startServer((req, res) => {
    res.end('not-json');
  });
  const client = ZLinkHttpClient.create(server.baseUrl).build();
  try {
    await assert.rejects(
      () => client.get('/x').async(),
      (e) => e instanceof ZLinkFrameworkException && e.kind === ZLinkFrameworkErrorKind.ProtocolError,
    );
  } finally {
    await client.close();
    await server.close();
  }
});

test('follows redirect and rewrites POST to GET on 303', async () => {
  let finalMethod;
  const server = await startServer(async (req, res) => {
    if (req.url === '/start') {
      res.statusCode = 303;
      res.setHeader('location', '/result');
      res.end();
      return;
    }
    finalMethod = req.method;
    res.end(JSON.stringify({ id: 'game-1', ranked: false }));
  });
  const client = ZLinkHttpClient.create(server.baseUrl).followRedirects(3).build();
  try {
    const r = await client.post('/start').body({ name: 'x' }).async();
    assert.equal(finalMethod, 'GET');
    assert.equal(r.body.id, 'game-1');
  } finally {
    await client.close();
    await server.close();
  }
});

test('preserves authorization on same-origin redirect', async () => {
  let authAtResult;
  const server = await startServer((req, res) => {
    if (req.url === '/start') {
      res.statusCode = 307;
      res.setHeader('location', '/result');
      res.end();
      return;
    }
    authAtResult = req.headers['authorization'];
    res.end('{}');
  });
  const client = ZLinkHttpClient.create(server.baseUrl).bearerToken('secret').followRedirects(3).build();
  try {
    await client.get('/start').submitRaw();
    assert.equal(authAtResult, 'Bearer secret');
  } finally {
    await client.close();
    await server.close();
  }
});

test('strips authorization on cross-origin redirect', async () => {
  let authAtOther;
  const other = await startServer((req, res) => {
    authAtOther = req.headers['authorization'] ?? null;
    res.end('{}');
  });
  const origin = await startServer((req, res) => {
    res.statusCode = 307;
    res.setHeader('location', `${other.baseUrl}/result`);
    res.end();
  });
  const client = ZLinkHttpClient.create(origin.baseUrl).bearerToken('secret').followRedirects(3).build();
  try {
    await client.get('/start').submitRaw();
    assert.equal(authAtOther, null);
  } finally {
    await client.close();
    await origin.close();
    await other.close();
  }
});

test('redirect limit exceeded throws', async () => {
  const server = await startServer((req, res) => {
    res.statusCode = 302;
    res.setHeader('location', '/loop');
    res.end();
  });
  const client = ZLinkHttpClient.create(server.baseUrl).followRedirects(2).build();
  try {
    await assert.rejects(
      () => client.get('/loop').submitRaw(),
      (e) => e instanceof ZLinkFrameworkException && e.kind === ZLinkFrameworkErrorKind.Unavailable,
    );
  } finally {
    await client.close();
    await server.close();
  }
});

test('retries retriable transport failure', async () => {
  let connections = 0;
  const server = net.createServer((socket) => {
    connections++;
    if (connections <= 1) {
      socket.destroy(); // drop -> retriable transport error
      return;
    }
    socket.on('data', () => {
      socket.write('HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}');
      socket.end();
    });
  });
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  const { port } = server.address();
  const client = ZLinkHttpClient.create(`http://127.0.0.1:${port}`).retry(3).build();
  try {
    const r = await client.get('/r').submitRaw();
    assert.equal(r.status, 200);
    assert.ok(connections >= 2);
  } finally {
    await client.close();
    await new Promise((done) => server.close(() => done()));
  }
});

test('cookie jar round trip with path scope', async () => {
  let cookieAtScoped;
  let cookieAtRoot;
  const server = await startServer((req, res) => {
    if (req.url === '/login') {
      res.setHeader('set-cookie', 'session=abc; Path=/secure');
      res.end('{}');
    } else if (req.url === '/secure/data') {
      cookieAtScoped = req.headers['cookie'] ?? null;
      res.end('{}');
    } else {
      cookieAtRoot = req.headers['cookie'] ?? null;
      res.end('{}');
    }
  });
  const client = ZLinkHttpClient.create(server.baseUrl).cookies().build();
  try {
    await client.get('/login').submitRaw();
    await client.get('/secure/data').submitRaw();
    await client.get('/open').submitRaw();
    assert.equal(cookieAtScoped, 'session=abc');
    assert.equal(cookieAtRoot, null);
  } finally {
    await client.close();
    await server.close();
  }
});

for (const encoding of ['gzip', 'deflate']) {
  test(`transparently decodes ${encoding} response`, async () => {
    const payload = JSON.stringify({ id: 42, name: 'Compressed' });
    const server = await startServer((req, res) => {
      const compressed = encoding === 'gzip' ? zlib.gzipSync(payload) : zlib.deflateSync(payload);
      res.setHeader('content-type', 'application/json');
      res.setHeader('content-encoding', encoding);
      res.end(compressed);
    });
    const client = ZLinkHttpClient.create(server.baseUrl).compression().build();
    try {
      const r = await client.get('/data').async();
      assert.equal(r.body.id, 42);
      assert.equal(r.headers['content-encoding'], undefined);
    } finally {
      await client.close();
      await server.close();
    }
  });
}

test('basic and bearer auth set authorization header', async () => {
  const seen = [];
  const server = await startServer((req, res) => {
    seen.push(req.headers['authorization']);
    res.end('{}');
  });
  try {
    const basic = ZLinkHttpClient.create(server.baseUrl).basicAuth('aria', 'secret').build();
    await basic.get('/a').submitRaw();
    await basic.close();
    const bearer = ZLinkHttpClient.create(server.baseUrl).bearerToken('tok-9').build();
    await bearer.get('/b').submitRaw();
    await bearer.close();
    assert.equal(seen[0], 'Basic ' + Buffer.from('aria:secret').toString('base64'));
    assert.equal(seen[1], 'Bearer tok-9');
  } finally {
    await server.close();
  }
});

test('secure cookie not sent over http and Max-Age=0 deletes', async () => {
  let cookieAfterSecure;
  let cookieAfterDelete;
  const server = await startServer((req, res) => {
    if (req.url === '/set') {
      res.setHeader('set-cookie', ['sid=abc; Secure', 'keep=1']);
      res.end('{}');
    } else if (req.url === '/c1') {
      cookieAfterSecure = req.headers['cookie'] ?? null;
      res.setHeader('set-cookie', 'keep=; Max-Age=0');
      res.end('{}');
    } else {
      cookieAfterDelete = req.headers['cookie'] ?? null;
      res.end('{}');
    }
  });
  const client = ZLinkHttpClient.create(server.baseUrl).cookies().build();
  try {
    await client.get('/set').submitRaw();
    await client.get('/c1').submitRaw();
    await client.get('/c2').submitRaw();
    assert.equal(cookieAfterSecure, 'keep=1');
    assert.equal(cookieAfterDelete, null);
  } finally {
    await client.close();
    await server.close();
  }
});

test('compressed malformed body throws payloadDecodeFailed', async () => {
  const server = await startServer((req, res) => {
    res.setHeader('content-encoding', 'gzip');
    res.end(Buffer.from([1, 2, 3, 4, 5, 6, 7, 8]));
  });
  const client = ZLinkHttpClient.create(server.baseUrl).compression().build();
  try {
    await assert.rejects(
      () => client.get('/bad').submitRaw(),
      (e) => e instanceof ZLinkFrameworkException && e.kind === ZLinkFrameworkErrorKind.ProtocolError,
    );
  } finally {
    await client.close();
    await server.close();
  }
});

test('timeout throws a retriable failure', async () => {
  const server = await startServer(async (req, res) => {
    await new Promise((r) => setTimeout(r, 500));
    res.end('{}');
  });
  const client = ZLinkHttpClient.create(server.baseUrl).timeout(80).build();
  try {
    await assert.rejects(
      () => client.get('/slow').submitRaw(),
      (e) => e instanceof ZLinkFrameworkException
        && e.kind === ZLinkFrameworkErrorKind.DeadlineExceeded
        && !('isRetriable' in e),
    );
  } finally {
    await client.close();
    await server.close();
  }
});

test('max response body size is enforced', async () => {
  const server = await startServer((req, res) => {
    res.setHeader('content-type', 'text/plain');
    res.end('x'.repeat(4096));
  });
  const client = ZLinkHttpClient.create(server.baseUrl).maxResponseBodySize(1024).build();
  try {
    await assert.rejects(
      () => client.get('/big').submitRaw(),
      (e) => e instanceof ZLinkFrameworkException,
    );
  } finally {
    await client.close();
    await server.close();
  }
});

test('non-blocking concurrency does not serialize requests', async () => {
  const server = await startServer(async (req, res) => {
    await new Promise((r) => setTimeout(r, 200));
    res.end('{}');
  });
  const client = ZLinkHttpClient.create(server.baseUrl).build();
  try {
    const start = Date.now();
    const results = await Promise.all(Array.from({ length: 20 }, () => client.get('/r').submitRaw()));
    const elapsed = Date.now() - start;
    for (const r of results) {
      assert.equal(r.status, 200);
    }
    assert.ok(elapsed < 2000, `elapsed=${elapsed}`);
  } finally {
    await client.close();
    await server.close();
  }
});

test('validation rejects invalid configuration', () => {
  assert.throws(() => ZLinkHttpClient.create().build());
  assert.throws(() => ZLinkHttpClient.create('ftp://x').build());
  assert.throws(() => ZLinkHttpClient.create('http://h').timeout(0));
  assert.throws(() => ZLinkHttpClient.create('http://h').proxy('https://p').build());
  assert.throws(() => ZLinkHttpClient.create('http://h').followRedirects(0));
  assert.throws(() => ZLinkHttpClient.create('http://h').retry(0));
});

test('validation rejects bad request configuration', async () => {
  const client = ZLinkHttpClient.create('http://127.0.0.1:1').build();
  assert.throws(() => client.get('no-slash'));
  assert.throws(
    () => client.post('/r').bodyStream(undefined, 'application/octet-stream'),
      (e) => e instanceof ZLinkFrameworkException && e.kind === ZLinkFrameworkErrorKind.ProtocolError,
  );
  await assert.rejects(
    () => client.get('/r').download(undefined),
    (e) => e instanceof ZLinkFrameworkException && e.kind === ZLinkFrameworkErrorKind.ProtocolError,
  );
  await assert.rejects(
    () => client.post('/r').body('a', 'text/plain').form('b', 'c').submitRaw(),
    (e) => e instanceof ZLinkFrameworkException && e.kind === ZLinkFrameworkErrorKind.ProtocolError,
  );
  await client.close();
});

test('trusts test certificate over https', async () => {
  const server = await startTlsServer((req, res) => {
    res.end('{}');
  });
  const client = ZLinkHttpClient.create(server.baseUrl).trustCertificateFile(tlsPath('server-cert.pem')).build();
  try {
    const r = await client.get('/secure').submitRaw();
    assert.equal(r.status, 200);
  } finally {
    await client.close();
    await server.close();
  }
});

test('untrusted https certificate is rejected', async () => {
  const server = await startTlsServer((req, res) => {
    res.end('{}');
  });
  const client = ZLinkHttpClient.create(server.baseUrl).trustCertificateFile(tlsPath('other-cert.pem')).build();
  try {
    await assert.rejects(() => client.get('/secure').submitRaw());
  } finally {
    await client.close();
    await server.close();
  }
});

test('presents client certificate for mTLS', async () => {
  let presentedCN;
  const server = await startTlsServer(
    (req, res) => {
      const cert = req.socket.getPeerCertificate();
      presentedCN = cert && cert.subject ? cert.subject.CN : undefined;
      res.end('{}');
    },
    { requireClientCert: true },
  );
  const client = ZLinkHttpClient.create(server.baseUrl)
    .trustCertificateFile(tlsPath('server-cert.pem'))
    .clientCertificateFile(tlsPath('client-cert.pem'), tlsPath('client-key.pem'))
    .build();
  try {
    const r = await client.get('/mtls').submitRaw();
    assert.equal(r.status, 200);
    assert.equal(presentedCN, 'zlink-test-client');
  } finally {
    await client.close();
    await server.close();
  }
});
