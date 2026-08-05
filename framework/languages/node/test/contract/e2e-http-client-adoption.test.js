const assert = require('node:assert/strict');
const { readFileSync, readdirSync, statSync } = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const nodeRoot = path.resolve(__dirname, '../..');
const e2eRoot = path.join(nodeRoot, 'e2e');

test('e2e clients use the public HTTP client package instead of raw fetch adapters', () => {
  const clientSources = sourceFiles(e2eRoot)
    .filter((file) => file.includes(`${path.sep}Client${path.sep}`));
  const combined = clientSources.map((file) => readFileSync(file, 'utf8')).join('\n');

  assert.doesNotMatch(
    combined,
    /(?:^|[^\w.])fetch\s*\(/m,
    'E2E Client source must not call global fetch.'
  );
  assert.doesNotMatch(combined, /\bbrowserE2eFetch\b/, 'E2E Client source must not use a harness HTTP adapter.');
  assert.equal(
    clientSources.filter((file) => file.endsWith(`${path.sep}Support${path.sep}http-client.ts`)).length,
    0,
    'Config-local HTTP client wrappers must be removed.'
  );
  assert.match(combined, /from ['"]@zlink-systems\/http-client['"]/, 'The public HTTP client package is not used.');

  const browserRuntime = readFileSync(path.join(e2eRoot, 'browser-client-runtime.ts'), 'utf8');
  assert.doesNotMatch(
    browserRuntime,
    /BrowserE2eHttpClient(?:Factory|Builder|Request)?/,
    'The browser harness must not duplicate the public HTTP client surface.'
  );
});

function sourceFiles(root) {
  const files = [];
  for (const entry of readdirSync(root)) {
    const candidate = path.join(root, entry);
    if (statSync(candidate).isDirectory()) files.push(...sourceFiles(candidate));
    else if (candidate.endsWith('.ts')) files.push(candidate);
  }
  return files;
}
