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
//        [--global-name=<identifier>] [--supported:<feature>=<true|false> ...]
//        [--external:<name> ...]
//
// --global-name is esbuild's `globalName` for `--format=iife`. The Unity WebGL
// UPM adapter needs the package root as a plain identifier because an
// emscripten pre-js file cannot use `import`. Passing the option through is
// one line here; a separate entry file that assigns to a global would add a
// second module instance to maintain and would still need this bundler to run.
//
// That IIFE bundle is also the one output built for es2019 rather than es2022.
// It is committed into the UPM package as a .jspre, and emscripten concatenates
// it into the player's JavaScript, where emcc -O2 and above rewrite the result
// with the JS optimizer bundled in the Emscripten 3.1.38 that Unity 2023.2 and
// later ship. That optimizer parses at a fixed ecmaVersion 2020 and converts the
// tree with a terser whose converter predates ES2020, so es2022 `static` class
// fields fail the parse and ES2020 optional chaining fails the conversion; a
// Unity release build cannot link either. es2019 is the level both stages
// accept. --supported:bigint=true is the one exception: the wire code uses
// BigInt literals, the optimizer handles them, and lowering them would put a
// BigInt() call in the frame header path.
//
// es2019 makes the link succeed; it does not make that optimizer safe. Its
// dead-code stage still deletes destructuring declarations, which no bundler
// setting reaches - see test/browser/unity-webgl-emscripten.test.js, which pins
// both what is fixed and what is not. The ESM browser bundles are loaded by the
// browser itself, never by emscripten, so they stay at es2022.
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
  else if (arg.startsWith('--supported:')) {
    const [feature, value] = arg.slice('--supported:'.length).split('=');
    if (value !== 'true' && value !== 'false') throw new Error(`--supported:${feature} needs true or false`);
    options.supported = { ...options.supported, [feature]: value === 'true' };
  }
  else if (arg.startsWith('--external:')) options.external.push(arg.slice('--external:'.length));
  else if (arg.startsWith('--')) throw new Error(`unsupported esbuild argument: ${arg}`);
  else options.entryPoints.push(arg);
}
if (options.entryPoints.length === 0 || !options.outfile) {
  throw new Error('esbuild-bundle.mjs needs an entry point and --outfile');
}
await build({ ...options, logLevel: 'warning' });
