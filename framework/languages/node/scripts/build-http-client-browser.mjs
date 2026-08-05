import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { build } from 'esbuild';

const nodeRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const runtimePath = path.join(nodeRoot, 'packages/http-client/src/runtime/browser-runtime.ts');

await build({
  entryPoints: [path.join(nodeRoot, 'packages/http-client/src/index.ts')],
  bundle: true,
  format: 'esm',
  platform: 'browser',
  target: 'es2022',
  outfile: path.join(nodeRoot, 'packages/http-client/dist/browser/index.mjs'),
  plugins: [{
    name: 'browser-http-runtime',
    setup(buildContext) {
      buildContext.onResolve({ filter: /^\.\/runtime\/runtime$/ }, (args) => {
        if (args.importer.endsWith(`${path.sep}client.ts`)) return { path: runtimePath };
        return undefined;
      });
    },
  }],
});
