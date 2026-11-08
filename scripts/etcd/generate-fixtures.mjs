#!/usr/bin/env node
// Regenerate golden byte vectors; the conformance inventory is maintained by hand.
import { readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { resolve } from 'node:path';
import { main, writeJson } from './common.mjs';

const root = fileURLToPath(new URL('../../', import.meta.url));
const specRoot = 'framework/doc/framework/common/spec/server/05-location-relocation/';
const etcdSpec = `${specRoot}07-location-store-etcd.ko.md`;
main(async () => {
  const old = JSON.parse(await readFile(resolve(root, 'framework/runtime/protocol/golden/store-record-v1.json'), 'utf8'));
  const inputKeys = old.keyDerivation.map((entry) => ({ name: entry.record, namespace: old.prefixExample,
    frameworkKeyHex: entry.preimageHex, inputRef: `store-record-v1.json#/keyDerivation/${old.keyDerivation.indexOf(entry)}/preimageHex` }));
  inputKeys.push(
    { name: 'unicode-and-nul', namespace: '시험😀', frameworkKeyHex: Buffer.from('키\0😀\0끝').toString('hex') },
    { name: 'maximum-ascii-lengths', namespace: 'n'.repeat(64), frameworkKeyHex: 'ab'.repeat(1024) },
    { name: 'maximum-utf8-namespace', namespace: '한'.repeat(21) + 'a', frameworkKeyHex: '00' }
  );
  const keyVectors = inputKeys.map((input) => {
    const prefix = Buffer.from(input.namespace);
    const key = Buffer.from(input.frameworkKeyHex, 'hex');
    return { ...input,
      metadataKeyHex: Buffer.concat([prefix, Buffer.from([0, 0x6d, 0]), key]).toString('hex'),
      payloadKeyHex: Buffer.concat([prefix, Buffer.from([0, 0x70, 0]), key]).toString('hex'),
      clockKeyHex: Buffer.concat([prefix, Buffer.from([0, 0x63])]).toString('hex') };
  });
  const metadataVectors = [];
  for (const expires of [false, true]) for (const payloadLength of [0, 1048576]) {
    const bytes = Buffer.alloc(14);
    bytes[0] = 1; bytes[1] = Number(expires);
    const expiresAtMs = expires ? '9223372036854' : '0';
    bytes.writeBigUInt64BE(BigInt(expiresAtMs), 2); bytes.writeUInt32BE(payloadLength, 10);
    metadataVectors.push({ name: `${expires ? 'expiring' : 'durable'}-${payloadLength}`, expiresAtMs,
      hasExpiry: expires, payloadLength, metadataHex: bytes.toString('hex') });
  }
  const golden = { format: 'location-store-etcd-v1', version: 1, consumers: ['cpp', 'dotnet', 'jvm', 'node'],
    spec: { file: etcdSpec, section: '3' }, keyVectors, metadataVectors,
    clockVectors: ['0', '1', '9223372036854'].map((value) => {
      const bytes = Buffer.alloc(8); bytes.writeBigUInt64BE(BigInt(value)); return { value, hex: bytes.toString('hex') };
    }),
    storeVersions: {
      accepted: [
        { clusterIdDecimal: '0', modRevision: '1', value: 'e1:0000000000000000:1' },
        { clusterIdDecimal: '18364758544493064720', modRevision: '9223372036854775807', value: 'e1:fedcba9876543210:9223372036854775807' }
      ],
      rejected: ['e2:fedcba9876543210:1', 'e1:FEDCBA9876543210:1', 'e1:fedcba987654321:1',
        'e1:0xfedcba9876543210:1', 'e1:fedcba9876543210:01', 'e1:fedcba9876543210:-1',
        'e1:fedcba9876543210:1.0', 'e1:fedcba9876543210:1e3', 'e1:fedcba9876543210:1\n']
    },
    corruptRecords: [
      ...['9223372036855', '18446744073709551615'].map((value) => {
        const metadata = Buffer.alloc(14); metadata[0] = 1; metadata[1] = 1;
        metadata.writeBigUInt64BE(BigInt(value), 2);
        return { name: value === '9223372036855' ? 'expiry-above-cap' : 'expiry-uint64-max', metadataHex: metadata.toString('hex'), payloadHex: '', expected: 'storeFailure' };
      }),
      ...['9223372036855', '18446744073709551615'].map((value) => {
        const bytes = Buffer.alloc(8); bytes.writeBigUInt64BE(BigInt(value));
        return { name: value === '9223372036855' ? 'clock-above-cap' : 'clock-uint64-max', clockHex: bytes.toString('hex'), expected: 'storeFailure' };
      }),
      { name: 'clock-wrong-length', clockHex: '00000000000000', expected: 'storeFailure' },
      { name: 'unknown-format-tag', metadataHex: '0200000000000000000000000000', payloadHex: '', expected: 'storeFailure' },
      { name: 'unused-flag-bit', metadataHex: '0102000000000000000000000000', payloadHex: '', expected: 'storeFailure' },
      { name: 'payload-length-mismatch', metadataHex: '0100000000000000000000000001', payloadHex: '', expected: 'storeFailure' },
      { name: 'metadata-only', metadataHex: '0100000000000000000000000000', payloadHex: null, expected: 'storeFailure' },
      { name: 'payload-only', metadataHex: null, payloadHex: '', expected: 'storeFailure' },
      { name: 'different-mod-revisions', metadataHex: '0100000000000000000000000000', payloadHex: '', metadataModRevision: '2', payloadModRevision: '3', expected: 'storeFailure' }
    ]
  };
  await writeJson(resolve(root, 'framework/runtime/protocol/golden/location-store-etcd-v1.json'), golden);
  console.log('generated golden byte vectors');
});
