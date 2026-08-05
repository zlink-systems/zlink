import { mkdirSync, rmSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

rmSync(resolve('packages/stream-connector/dist/browser'), {
  recursive: true,
  force: true
});
for (const packageName of ['framework-codec-msgpack', 'framework-codec-protobuf']) {
  rmSync(resolve(`packages/${packageName}/dist/browser`), { recursive: true, force: true });
  rmSync(resolve(`packages/${packageName}/dist/server`), { recursive: true, force: true });
}
rmSync(resolve('packages/stream-wire/dist/esm'), { recursive: true, force: true });
mkdirSync(resolve('packages/stream-connector/dist'), { recursive: true });
writeFileSync(
  resolve('packages/stream-connector/dist/package.json'),
  '{"type":"commonjs"}\n',
  'utf8'
);
