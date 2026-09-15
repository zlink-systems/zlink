#!/usr/bin/env node
// Cross-platform esbuild bundling entry point for the workspace scripts.
// npm runs package scripts through cmd.exe on Windows, where a POSIX path such
// as node_modules/.bin/esbuild is not an executable, and after a fresh
// `npm ci` node_modules/esbuild/bin/esbuild is the native binary, so it cannot
// be launched with `node <path>` either. Using the esbuild JavaScript API keeps
// one invocation that works everywhere.
//
// Usage: node scripts/esbuild-bundle.mjs <entry> --bundle --format=<esm|cjs|iife>
//        --platform=<browser|node> --target=<target> --outfile=<path>
//        [--global-name=<identifier>] [--external:<name> ...]
//
// --global-name is esbuild's `globalName` for `--format=iife`. The Unity WebGL
// UPM adapter needs the package root as a plain identifier because an
// emscripten pre-js file cannot use `import`. Passing the option through is
// one line here; a separate entry file that assigns to a global would add a
// second module instance to maintain and would still need this bundler to run.
import { build } from 'esbuild';

const args = process.argv.slice(2);
const options = { entryPoints: [], external: [] };
for (const arg of args) {
  if (arg === '--bundle') options.bundle = true;
  else if (arg.startsWith('--format=')) options.format = arg.slice('--format='.length);
  else if (arg.startsWith('--platform=')) options.platform = arg.slice('--platform='.length);
  else if (arg.startsWith('--target=')) options.target = arg.slice('--target='.length);
  else if (arg.startsWith('--outfile=')) options.outfile = arg.slice('--outfile='.length);
  else if (arg.startsWith('--global-name=')) options.globalName = arg.slice('--global-name='.length);
  else if (arg.startsWith('--external:')) options.external.push(arg.slice('--external:'.length));
  else if (arg.startsWith('--')) throw new Error(`unsupported esbuild argument: ${arg}`);
  else options.entryPoints.push(arg);
}
if (options.entryPoints.length === 0 || !options.outfile) {
  throw new Error('esbuild-bundle.mjs needs an entry point and --outfile');
}
await build({ ...options, logLevel: 'warning' });
