import fs from 'node:fs';

const bundledOutputs = [
  'packages/framework-codec-msgpack/dist/browser/index.mjs',
  'packages/framework-codec-msgpack/dist/server/framework.cjs'
];

for (const output of bundledOutputs) {
  const source = fs.readFileSync(output, 'utf8');
  const rewritten = source.replace(
    /^\s*\/\/ eslint-disable-next-line @typescript-eslint\/naming-convention\r?\n/gm,
    ''
  );
  if (rewritten !== source) fs.writeFileSync(output, rewritten);
}
