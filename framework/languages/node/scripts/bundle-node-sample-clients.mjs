import path from 'node:path';
import process from 'node:process';
import { build } from 'esbuild';

const [sampleDirectoryArgument, ...entryArguments] = process.argv.slice(2);
if (sampleDirectoryArgument === undefined || entryArguments.length === 0) {
  throw new Error(
    'Usage: node scripts/bundle-node-sample-clients.mjs <sample-directory> <client-entry> [...]'
  );
}

const sampleDirectory = path.resolve(sampleDirectoryArgument);
const entryPoints = entryArguments.map((entry) => path.resolve(sampleDirectory, entry));

await build({
  entryPoints,
  outdir: path.join(sampleDirectory, 'dist', 'Client'),
  entryNames: '[name]',
  bundle: true,
  format: 'cjs',
  platform: 'node',
  target: 'node22',
  plugins: [{
    name: 'sample-client-runtime-boundary',
    setup(context) {
      context.onResolve({ filter: /^(?:[^./]|#)/ }, (args) => {
        if (args.path === '@zlink-systems/stream-connector') return undefined;
        return { path: args.path, external: true };
      });
    }
  }]
});
