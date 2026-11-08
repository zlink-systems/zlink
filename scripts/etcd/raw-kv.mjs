#!/usr/bin/env node
import { readFile } from 'node:fs/promises';
import { pathToFileURL } from 'node:url';
import { args, integer, main, rpc, revision } from './common.mjs';

export const base64Hex = (hex) => {
  if (typeof hex !== 'string' || !/^(?:[0-9a-fA-F]{2})*$/.test(hex)) throw new Error('expected even-length hex bytes');
  return Buffer.from(hex, 'hex').toString('base64');
};
export async function rawKV(options) {
  const paths = { range: '/v3/kv/range', put: '/v3/kv/put', delete: '/v3/kv/deleterange', txn: '/v3/kv/txn' };
  if (!paths[options.command]) throw new Error('expected range, put, delete or txn');
  let body;
  if (options.command === 'txn') {
    if (!options['body-file']) throw new Error('txn requires --body-file (etcd gateway JSON)');
    body = JSON.parse(await readFile(options['body-file'], 'utf8'));
  } else {
    body = { key: base64Hex(options['key-hex']) };
    if (!body.key) throw new Error('key must not be empty');
    if (options['end-hex'] !== undefined) body.range_end = base64Hex(options['end-hex']);
    if (options.revision !== undefined) body.revision = revision(options.revision);
    if (options.limit !== undefined) body.limit = String(integer(options.limit, 'limit'));
    if (options.command === 'put') body.value = base64Hex(options['value-hex']);
  }
  const result = await rpc(options.endpoint, paths[options.command], body);
  if (result.kvs) result.bytes = result.kvs.map((kv) => ({ ...kv,
    keyHex: Buffer.from(kv.key, 'base64').toString('hex'), valueHex: Buffer.from(kv.value ?? '', 'base64').toString('hex') }));
  return result;
}
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) main(async () => {
  const options = args({ endpoint: 'string', 'key-hex': 'string', 'value-hex': 'string', 'end-hex': 'string', revision: 'string', limit: 'string', 'body-file': 'string' });
  if (!options.endpoint) throw new Error('--endpoint is required');
  console.log(JSON.stringify(await rawKV(options), null, 2));
});
