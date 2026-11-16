import { execFileSync } from 'node:child_process';
import { copyFileSync, mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const scriptDirectory = dirname(fileURLToPath(import.meta.url));
const workspace = resolve(scriptDirectory, '..');
const repository = resolve(workspace, '../../..');
const schema = join(repository, 'framework/runtime/protocol/channel-header-v1.proto');
const generatedDirectory = join(
  workspace,
  'packages/framework/src/runtime/channels/generated'
);
const generatedModule = join(generatedDirectory, 'channel-header-v1.js');
const generatedTypes = join(generatedDirectory, 'channel-header-v1.d.ts');
const check = process.argv.includes('--check');
const temporaryDirectory = mkdtempSync(join(tmpdir(), 'zlink-channel-header-'));

try {
  const modulePath = join(temporaryDirectory, 'channel-header-v1.js');
  const typesPath = join(temporaryDirectory, 'channel-header-v1.d.ts');
  const pbjs = join(workspace, 'node_modules/.bin/pbjs');
  const pbts = join(workspace, 'node_modules/.bin/pbts');

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
      if (!readFileSync(path).equals(readFileSync(generatedPath))) {
        throw new Error(`${path.slice(workspace.length + 1)} is stale; run npm run generate:channel-header`);
      }
    }
  } else {
    const { mkdirSync } = await import('node:fs');
    mkdirSync(generatedDirectory, { recursive: true });
    copyFileSync(modulePath, generatedModule);
    copyFileSync(typesPath, generatedTypes);
  }
} finally {
  rmSync(temporaryDirectory, { recursive: true, force: true });
}
