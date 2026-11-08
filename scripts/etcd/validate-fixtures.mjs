#!/usr/bin/env node
import assert from 'node:assert/strict';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { readFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { args, main, readJson } from './common.mjs';

const root = fileURLToPath(new URL('../../', import.meta.url));
const decode = (hex) => {
  assert.equal(typeof hex, 'string'); assert.match(hex, /^(?:[a-f0-9]{2})*$/);
  return Buffer.from(hex, 'hex');
};
export async function validateEtcdFixtures(options = {}) {
  const golden = await readJson(options['golden-file'] ?? resolve(root, 'framework/runtime/protocol/golden/location-store-etcd-v1.json'));
  assert.equal(golden.format, 'location-store-etcd-v1'); assert.equal(golden.version, 1);
  assert.ok(golden.keyVectors.length >= 9);
  for (const entry of golden.keyVectors) {
    const namespace = Buffer.from(entry.namespace);
    const key = decode(entry.frameworkKeyHex);
    assert.ok(namespace.length >= 1 && namespace.length <= 64 && !namespace.includes(0));
    assert.ok(key.length >= 1 && key.length <= 1024);
    for (const [field, suffix] of [['metadataKeyHex', '006d00'], ['payloadKeyHex', '007000']]) {
      assert.equal(entry[field], namespace.toString('hex') + suffix + entry.frameworkKeyHex);
      assert.ok(decode(entry[field]).length <= 1091);
    }
    assert.equal(entry.clockKeyHex, namespace.toString('hex') + '0063');
    if (entry.inputRef) {
      const [file, pointer] = entry.inputRef.split('#/');
      const source = await readJson(resolve(root, 'framework/runtime/protocol/golden', file));
      assert.equal(pointer.split('/').reduce((value, key) => value[key], source), entry.frameworkKeyHex);
    }
  }
  assert.ok(golden.keyVectors.some((v) => decode(v.metadataKeyHex).length === 1091));
  assert.ok(golden.keyVectors.some((v) => decode(v.frameworkKeyHex).includes(0)));
  assert.deepEqual(new Set(golden.metadataVectors.map((v) => `${v.hasExpiry}:${v.payloadLength}`)),
    new Set(['false:0', 'false:1048576', 'true:0', 'true:1048576']));
  const maximumTimeMs = 9223372036854n;
  for (const vector of golden.metadataVectors) {
    const bytes = decode(vector.metadataHex);
    assert.equal(bytes.length, 14); assert.equal(bytes[0], 1); assert.equal(bytes[1], Number(vector.hasExpiry));
    assert.equal(bytes.readBigUInt64BE(2).toString(), vector.expiresAtMs);
    assert.equal(bytes.readUInt32BE(10), vector.payloadLength);
    assert.ok(BigInt(vector.expiresAtMs) <= maximumTimeMs, vector.name);
    if (!vector.hasExpiry) assert.equal(vector.expiresAtMs, '0');
  }
  assert.ok(golden.metadataVectors.some((v) => BigInt(v.expiresAtMs) === maximumTimeMs));
  assert.deepEqual(golden.clockVectors.map((v) => v.value), ['0', '1', String(maximumTimeMs)]);
  for (const vector of golden.clockVectors) {
    assert.ok(BigInt(vector.value) >= 0n && BigInt(vector.value) <= maximumTimeMs);
    const bytes = decode(vector.hex); assert.equal(bytes.length, 8); assert.equal(bytes.readBigUInt64BE().toString(), vector.value);
  }
  const versionSyntax = (value) => /^e1:[0-9a-f]{16}:[1-9][0-9]*$/.test(value) && value.trim() === value;
  for (const vector of golden.storeVersions.accepted) {
    assert.ok(versionSyntax(vector.value));
    assert.equal(vector.value, `e1:${BigInt(vector.clusterIdDecimal).toString(16).padStart(16, '0')}:${vector.modRevision}`);
  }
  for (const value of golden.storeVersions.rejected) assert.equal(versionSyntax(value), false, JSON.stringify(value));
  for (const name of ['expiry-above-cap', 'clock-above-cap', 'clock-wrong-length', 'expiry-uint64-max', 'clock-uint64-max'])
    assert.ok(golden.corruptRecords.some((v) => v.name === name), name);
  for (const vector of golden.corruptRecords) {
    if (vector.clockHex !== undefined) {
      const clock = decode(vector.clockHex);
      assert.ok(clock.length !== 8 || clock.readBigUInt64BE() > maximumTimeMs, vector.name);
      assert.equal(vector.expected, 'storeFailure');
      continue;
    }
    const metadata = vector.metadataHex === null ? null : decode(vector.metadataHex);
    const payload = vector.payloadHex === null ? null : decode(vector.payloadHex);
    const broken = !metadata || !payload || metadata.length !== 14 || metadata[0] !== 1 || (metadata[1] & 0xfe) !== 0
      || metadata.readBigUInt64BE(2) > maximumTimeMs
      || metadata.readUInt32BE(10) !== payload.length || vector.metadataModRevision !== vector.payloadModRevision;
    assert.ok(broken, vector.name); assert.equal(vector.expected, 'storeFailure');
  }
  const fixture = await readJson(resolve(root, 'framework/runtime/conformance/location-store-provider-v1.json'));
  assert.equal(fixture.fixture, 'zlink.framework.location-store-provider'); assert.equal(fixture.version, 1);
  assert.deepEqual(fixture.languages, golden.consumers);
  assert.equal(new Set(fixture.cases.map((v) => v.id)).size, fixture.cases.length);
  const specSections = new Map();
  for (const [file, section] of [
    ['07-location-store-etcd.ko.md', '12'], ['02-location-store-redis.ko.md', '10']
  ]) {
    const path = `framework/doc/framework/common/spec/server/05-location-relocation/${file}`;
    const source = options[file.startsWith('07') ? 'etcd-spec' : 'spi-spec'] ?? resolve(root, path);
    const lines = (await readFile(source, 'utf8')).split(/\r?\n/);
    const start = lines.findIndex((line) => line.startsWith(`## ${section}.`));
    assert.ok(start >= 0, `missing section: ${path} section ${section}`);
    const bullets = [];
    for (let index = start + 1; index < lines.length; index++) {
      if (lines[index] === '---' || lines[index].startsWith('## ')) break;
      if (!lines[index].startsWith('- ')) continue;
      let text = lines[index].slice(2);
      while (lines[index + 1]?.startsWith('  ')) text += ` ${lines[++index].trim()}`;
      bullets.push(text);
    }
    assert.ok(bullets.length > 0);
    specSections.set(`${path} section ${section}`, { bullets, observations: [] });
  }
  for (const item of [...fixture.cases, ...fixture.excludedRequirements]) {
    assert.deepEqual(Object.keys(item.spec).sort(), ['file', 'section']);
    const reference = `${item.spec.file} section ${item.spec.section}`;
    const section = specSections.get(reference);
    assert.ok(section, reference);
    assert.ok(section.bullets.includes(item.expectedObservation), `${reference}: unknown observation: ${item.expectedObservation}`);
    section.observations.push(item.expectedObservation);
    if (item.reason) {
      assert.ok(item.spec.file.endsWith('02-location-store-redis.ko.md'));
      assert.equal(item.id, undefined);
      continue;
    }
    assert.match(item.id, /^(etcd|spi)\.[a-z][a-z0-9-]+$/);
    assert.ok(['unit', 'contract', 'cross-language', 'fault-injection'].includes(item.level));
    assert.ok(item.infrastructure.includes('provider-adapter'));
    assert.equal(item.requiresStoreManager, item.infrastructure.includes('store-manager-process'));
  }
  for (const [reference, { bullets, observations }] of specSections) {
    assert.equal(observations.length, bullets.length, reference);
    for (const bullet of bullets) {
      assert.equal(observations.filter((text) => text === bullet).length, 1, `${reference}: requirement must appear exactly once: ${bullet}`);
    }
  }
  return { cases: fixture.cases.length, keyVectors: golden.keyVectors.length };
}
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) main(async () => console.log('etcd fixtures: PASS', await validateEtcdFixtures(args({ 'etcd-spec': 'string', 'spi-spec': 'string', 'golden-file': 'string' }))));
