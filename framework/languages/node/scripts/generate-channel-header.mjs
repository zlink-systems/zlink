import { execFileSync } from 'node:child_process';
import { createRequire } from 'node:module';
import { copyFileSync, mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const require = createRequire(import.meta.url);
const scriptDirectory = dirname(fileURLToPath(import.meta.url));
const workspace = resolve(scriptDirectory, '..');
const repository = resolve(workspace, '../../..');
const protobufCliPackagePath = require.resolve('protobufjs-cli/package.json');
const protobufCliPackage = JSON.parse(readFileSync(protobufCliPackagePath, 'utf8'));
const protobufCliDirectory = dirname(protobufCliPackagePath);
const pbjs = resolve(protobufCliDirectory, protobufCliPackage.bin.pbjs);
const pbts = resolve(protobufCliDirectory, protobufCliPackage.bin.pbts);
const schema = join(repository, 'framework/runtime/protocol/channel-header-v1.proto');
const generatedDirectory = join(
  workspace,
  'packages/framework/src/runtime/channels/generated'
);
const generatedModule = join(workspace, 'packages/framework/generated/channel-header-v1.js');
const generatedTypes = join(generatedDirectory, 'channel-header-v1.d.ts');
const check = process.argv.includes('--check');

//  tsc emits only .ts sources, so every emitted tree (dist, or a gate's --into tree) gets
//  the committed generated module next to the compiled channels/ directory.
const intoIndex = process.argv.indexOf('--into');
if (process.argv.includes('--dist') || intoIndex >= 0) {
  const { mkdirSync } = await import('node:fs');
  const targetDirectory =
    intoIndex >= 0
      ? resolve(workspace, process.argv[intoIndex + 1], 'runtime/channels/generated')
      : join(workspace, 'packages/framework/dist/runtime/channels/generated');
  mkdirSync(targetDirectory, { recursive: true });
  copyFileSync(generatedModule, join(targetDirectory, 'channel-header-v1.js'));
  copyFileSync(generatedTypes, join(targetDirectory, 'channel-header-v1.d.ts'));
  process.exit(0);
}

const temporaryDirectory = mkdtempSync(join(tmpdir(), 'zlink-channel-header-'));

try {
  const modulePath = join(temporaryDirectory, 'channel-header-v1.js');
  const typesPath = join(temporaryDirectory, 'channel-header-v1.d.ts');

  execFileSync(process.execPath, [pbjs, '-t', 'static-module', '-w', 'commonjs', '-o', modulePath, schema], {
    cwd: workspace,
    stdio: 'inherit'
  });
  execFileSync(process.execPath, [pbts, '-o', typesPath, modulePath], {
    cwd: workspace,
    stdio: 'inherit'
  });
  if (check) {
    for (const [path, generatedPath] of [
      [generatedModule, modulePath],
      [generatedTypes, typesPath]
    ]) {
      const committed = readFileSync(path, 'utf8').replace(/\r\n/g, '\n');
      const generated = readFileSync(generatedPath, 'utf8').replace(/\r\n/g, '\n');
      if (committed !== generated) {
        throw new Error(`${path.slice(workspace.length + 1)} is stale; run npm run generate:channel-header`);
      }
    }
  } else {
    const { mkdirSync } = await import('node:fs');
    mkdirSync(dirname(generatedModule), { recursive: true });
    mkdirSync(generatedDirectory, { recursive: true });
    copyFileSync(modulePath, generatedModule);
    copyFileSync(typesPath, generatedTypes);
  }
} finally {
  rmSync(temporaryDirectory, { recursive: true, force: true });
}
