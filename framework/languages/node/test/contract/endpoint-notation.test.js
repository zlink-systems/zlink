const assert = require('node:assert/strict');
const test = require('node:test');

const {
  normalizeEndpoint,
  buildAdvertisedEndpoint,
  parseEndpointHostPort
} = require('../../packages/framework/dist/contracts/Configuration/EndpointNotation');

test('normalizeEndpoint lowercases scheme and host, and trims surrounding whitespace', () => {
  assert.equal(normalizeEndpoint('TCP://Example.COM:80'), 'tcp://example.com:80');
  assert.equal(normalizeEndpoint('  tcp://host:80  '), 'tcp://host:80');
  assert.equal(normalizeEndpoint('TLS://HOST:443'), 'tls://host:443');
});

test('normalizeEndpoint unifies IPv6 bracket notation and preserves the zone id verbatim', () => {
  assert.equal(normalizeEndpoint('tcp://[::1]:80'), 'tcp://[::1]:80');
  assert.equal(normalizeEndpoint('tcp://[FE80::1%eth0]:80'), 'tcp://[fe80::1%eth0]:80');
  assert.equal(normalizeEndpoint('tcp://[FE80::1%Eth0]:80'), 'tcp://[fe80::1%Eth0]:80');
});

test('normalizeEndpoint strips leading zeros from the port', () => {
  assert.equal(normalizeEndpoint('tcp://host:0080'), 'tcp://host:80');
  assert.equal(normalizeEndpoint('tcp://host:0'), 'tcp://host:0');
});

test('normalizeEndpoint strips trailing slashes from the path', () => {
  assert.equal(normalizeEndpoint('tcp://host:80/path/'), 'tcp://host:80/path');
  assert.equal(normalizeEndpoint('tcp://host:80/path//'), 'tcp://host:80/path');
});

test('normalizeEndpoint round-trips every policy 2.2 case together', () => {
  const input = '  TCP://[FE80::1%eth0]:0080/some/path/  ';
  assert.equal(normalizeEndpoint(input), 'tcp://[fe80::1%eth0]:80/some/path');
});

test('normalizeEndpoint is lossless: userInfo, query, fragment and zone id are preserved exactly', () => {
  const withExtras = 'TCP://User:Pass@HOST:0080/path/?q=1&r=2#frag';
  assert.equal(
    normalizeEndpoint(withExtras),
    'tcp://User:Pass@host:80/path?q=1&r=2#frag'
  );
  assert.equal(
    normalizeEndpoint('tcp://[FE80::1%eth0]:80'),
    'tcp://[fe80::1%eth0]:80'
  );
});

test('normalizeEndpoint is idempotent for every policy 2.2 case', () => {
  const cases = [
    'TCP://Example.COM:0080/',
    'tcp://[::1]:80',
    'tcp://[FE80::1%eth0]:80',
    'tcp://host:80/path/',
    'tcp://user:pass@host:80/path?q=1#frag',
    '  tcp://host:80  ',
    'tcp://host:80/a//',
    'tls://HOST:443'
  ];
  for (const value of cases) {
    const once = normalizeEndpoint(value);
    const twice = normalizeEndpoint(once);
    assert.equal(twice, once, `not idempotent for ${JSON.stringify(value)}`);
  }
});

test('normalizeEndpoint does not resolve DNS: localhost and 127.0.0.1 remain distinct endpoints', () => {
  assert.notEqual(normalizeEndpoint('tcp://localhost:80'), normalizeEndpoint('tcp://127.0.0.1:80'));
});

test('normalizeEndpoint only lowercases the scheme for non-authority schemes like ipc://, leaving the path byte-identical', () => {
  // ipc:// is followed by a filesystem path, not a host:port authority.
  // Paths can be case sensitive and a trailing slash can be meaningful, so
  // the host/port/trailing-slash rules must not apply here (matches the
  // C++ implementation in endpoint_notation.hpp).
  assert.equal(normalizeEndpoint('IPC:///tmp/MixedCase/Socket.sock'), 'ipc:///tmp/MixedCase/Socket.sock');
  assert.equal(normalizeEndpoint('ipc:///tmp/Socket.sock/'), 'ipc:///tmp/Socket.sock/');
  assert.equal(normalizeEndpoint('  IPC:///tmp/x  '), 'ipc:///tmp/x');
  assert.equal(normalizeEndpoint('ipc://RelativePath'), 'ipc://RelativePath');
});

test('normalizeEndpoint only lowercases the scheme for inproc://, leaving the name byte-identical', () => {
  assert.equal(normalizeEndpoint('INPROC://MixedCaseName'), 'inproc://MixedCaseName');
  assert.equal(normalizeEndpoint('inproc://Name-With-Trailing-Slash/'), 'inproc://Name-With-Trailing-Slash/');
});

test('normalizeEndpoint is idempotent for non-authority schemes too', () => {
  const cases = ['IPC:///tmp/MixedCase/Socket.sock', 'INPROC://MixedCaseName'];
  for (const value of cases) {
    const once = normalizeEndpoint(value);
    const twice = normalizeEndpoint(once);
    assert.equal(twice, once, `not idempotent for ${JSON.stringify(value)}`);
  }
});

test('buildAdvertisedEndpoint replaces only the host and accepts an uppercase source scheme', () => {
  assert.equal(buildAdvertisedEndpoint('TCP://0.0.0.0:80', 'example.com'), 'tcp://example.com:80');
  assert.equal(buildAdvertisedEndpoint('tcp://0.0.0.0:80', undefined), 'tcp://0.0.0.0:80');
});

test('buildAdvertisedEndpoint honors a scheme restriction and rejects a non-matching bound endpoint', () => {
  assert.equal(buildAdvertisedEndpoint('TCP://0.0.0.0:80', 'host', 'tcp'), 'tcp://host:80');
  assert.equal(buildAdvertisedEndpoint('ipc:///tmp/x', 'host', 'tcp'), undefined);
  assert.equal(buildAdvertisedEndpoint('ipc:///tmp/x', 'host'), undefined);
});

test('buildAdvertisedEndpoint brackets an IPv6 advertise host', () => {
  assert.equal(buildAdvertisedEndpoint('tcp://0.0.0.0:80', '::1'), 'tcp://[::1]:80');
});

test('parseEndpointHostPort is IPv6-safe (bracket-aware, never lastIndexOf(":"))', () => {
  assert.deepEqual(parseEndpointHostPort('tcp://[fe80::1%eth0]:80', 'tcp'), { host: 'fe80::1%eth0', port: 80 });
  assert.deepEqual(parseEndpointHostPort('tcp://host:80', 'tcp'), { host: 'host', port: 80 });
  assert.equal(parseEndpointHostPort('tcp://host:80', 'tls'), undefined);
});
