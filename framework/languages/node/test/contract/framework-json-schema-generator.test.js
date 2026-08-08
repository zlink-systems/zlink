'use strict';

const assert = require('node:assert/strict');
const childProcess = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const { pathToFileURL } = require('node:url');

const nodeRoot = path.resolve(__dirname, '../..');
const generator = path.join(nodeRoot, 'scripts/generate-framework-json-schemas.mjs');
const fixtureDirectory = path.join(__dirname, 'fixtures');
const frameworkJson = require('../../packages/framework/dist/runtime/messaging/framework-json-v1');
const packetContracts = require('../../packages/framework/dist/contracts/Handlers/JsonContract');

test('build-time schema generator emits server and browser registries from TS DTOs', async () => {
  const outputDirectory = fs.mkdtempSync(path.join(nodeRoot, 'test/.zlink-node-json-schema-'));
  const serverOutput = path.join(outputDirectory, 'server.cjs');
  const browserOutput = path.join(outputDirectory, 'browser.mjs');
  try {
    runGenerator(
      'framework-json-schema-fixture.tsconfig.json',
      serverOutput,
      browserOutput
    );

    const server = require(serverOutput);
    assert.deepEqual(Object.keys(server.packetContracts), ['ClassPacket', 'FixturePacket']);
    assert.equal(typeof server.register, 'function');
    assert.deepEqual(server.packetContracts.ClassPacket.payload, {
      type: 'object',
      properties: {
        flags: { type: 'array', items: { type: 'boolean' } },
        identifier: { type: 'string' }
      },
      required: ['identifier']
    });
    assert.deepEqual(server.packetContracts.FixturePacket.payload, {
      type: 'object',
      properties: {
        bytes: { type: 'bytes' },
        nested: {
          type: 'object',
          properties: {
            count: { type: 'int32' },
            label: { type: 'string' }
          },
          required: ['label']
        },
        nullable: {
          type: 'nullable',
          value: {
            type: 'object',
            properties: {
              count: { type: 'int32' },
              label: { type: 'string' }
            },
            required: ['label']
          }
        },
        optional: { type: 'boolean' },
        required: { type: 'string' },
        sequence: { type: 'int64' },
        status: { type: 'enum', names: ['ready', 'closed'] },
        values: { type: 'array', items: { type: 'int32' } }
      },
      required: ['bytes', 'nested', 'nullable', 'required', 'sequence', 'status', 'values']
    });

    const registered = packetContracts.readZLinkPacketJsonContract('FixturePacket');
    assert.deepEqual(registered, server.packetContracts.FixturePacket);

    const valid = frameworkJson.stringifyFrameworkJsonV1({
      bytes: 'AQI=',
      nested: { label: 'nested' },
      nullable: null,
      required: 'value',
      sequence: '7',
      status: 'ready',
      values: [1, 2]
    }, server.packetContracts.FixturePacket.payload);
    const decoded = frameworkJson.parseFrameworkJsonV1(
      valid,
      {},
      server.packetContracts.FixturePacket.payload
    );
    assert.deepEqual(decoded.bytes, Uint8Array.from([1, 2]));
    assert.equal(decoded.sequence, 7n);
    assert.throws(
      () => frameworkJson.parseFrameworkJsonV1(
        '{"required":"value","nullable":null,"nested":{"label":"nested"},"values":[1],"sequence":"7","status":"bad","bytes":"AQI="}',
        {},
        server.packetContracts.FixturePacket.payload
      ),
      /one of ready, closed/
    );

    const browser = await import(pathToFileURL(browserOutput).href);
    assert.deepEqual(browser.packetContracts, server.packetContracts);
  } finally {
    fs.rmSync(outputDirectory, { recursive: true, force: true });
  }
});

test('build-time schema generator rejects unsupported DTO expressions', () => {
  const outputDirectory = fs.mkdtempSync(path.join(os.tmpdir(), 'zlink-node-json-schema-unsupported-'));
  try {
    const result = runGenerator(
      'framework-json-schema-unsupported.tsconfig.json',
      path.join(outputDirectory, 'server.cjs'),
      undefined,
      false
    );
    assert.notEqual(result.status, 0);
    assert.match(`${result.stdout}\n${result.stderr}`, /unsupported 'Date' type/);
  } finally {
    fs.rmSync(outputDirectory, { recursive: true, force: true });
  }
});

function runGenerator(configName, serverOutput, browserOutput, throwOnFailure = true) {
  const args = [generator, '--project', path.join(fixtureDirectory, configName), '--out', serverOutput];
  if (browserOutput !== undefined) args.push('--browser-out', browserOutput);
  const result = childProcess.spawnSync(process.execPath, args, {
    cwd: nodeRoot,
    encoding: 'utf8'
  });
  if (throwOnFailure && result.status !== 0) {
    throw new Error(`schema generator failed: ${result.stdout}\n${result.stderr}`);
  }
  return result;
}
