// Packaging contract for framework/languages/unity/com.zlink.stream-connector.webgl.
//
// UPM installs a package by checking out a git tag or by unpacking a tarball; it
// never runs a Node build. The browser bundle therefore has to be committed, and
// the cost of a committed generated file is drift. This test is the guard: it
// regenerates the file from the current source and fails when the committed
// bytes differ.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const { buildSync } = require('esbuild');

const nodeRoot = path.resolve(__dirname, '../..');
const packageRoot = path.resolve(nodeRoot, '../unity/com.zlink.stream-connector.webgl');

test('the UPM package has the layout Unity requires', () => {
  const manifest = JSON.parse(fs.readFileSync(path.join(packageRoot, 'package.json'), 'utf8'));
  assert.equal(manifest.name, 'com.zlink.stream-connector.webgl');
  assert.match(manifest.version, /^\d+\.\d+\.\d+$/);
  assert.equal(
    manifest.version,
    JSON.parse(fs.readFileSync(path.join(nodeRoot, 'packages/stream-connector/package.json'), 'utf8')).version,
    'the adapter ships with the stream-connector release it embeds'
  );
  assert.ok(manifest.displayName);
  assert.ok(manifest.unity);
  assert.deepEqual(manifest.dependencies, {});

  for (const required of [
    'LICENSE',
    'README.md',
    'Runtime/Systems.Zlink.Stream.Connector.WebGL.asmdef',
    'Plugins/WebGL/ZlinkStreamConnector.jslib',
    'Plugins/WebGL/ZlinkStreamRuntime.jspre',
    'Plugins/WebGL/zlink-stream-connector.jspre'
  ]) {
    assert.ok(fs.existsSync(path.join(packageRoot, required)), `${required} is missing`);
  }

  const asmdef = JSON.parse(
    fs.readFileSync(path.join(packageRoot, 'Runtime/Systems.Zlink.Stream.Connector.WebGL.asmdef'), 'utf8')
  );
  assert.equal(asmdef.name, 'Systems.Zlink.Stream.Connector.WebGL');
  // The adapter and the native Zlink.Stream.Connector assembly define the same
  // types. Restricting this one to WebGL is what keeps them from colliding in a
  // project that ships both.
  assert.deepEqual(asmdef.includePlatforms, ['WebGL']);
  assert.equal(asmdef.rootNamespace, 'Systems.Zlink.Stream.Connector');

  // Unity links .jslib with --js-library and .jspre with --pre-js. A plain .js file
  // under Plugins/WebGL is not linked into the build at all.
  for (const file of fs.readdirSync(path.join(packageRoot, 'Plugins/WebGL'))) {
    assert.match(file, /\.(jslib|jspre)$/, `${file} would not be linked into a WebGL build`);
  }
});

test('the committed browser bundle matches the current stream-connector source', async () => {
  const { syncUnityWebglPackage } = await import(
    path.join(nodeRoot, 'scripts/sync-unity-webgl-package.mjs')
  );
  syncUnityWebglPackage({ check: true });
});

test('the embedded bundle carries no Node-only module or Buffer', () => {
  const outputDirectory = fs.mkdtempSync(path.join(os.tmpdir(), 'zlink-unity-webgl-bundle-'));
  const output = path.join(outputDirectory, 'unity-webgl-bundle.js');
  try {
    const result = buildSync({
      absWorkingDir: packageRoot,
      entryPoints: ['Plugins/WebGL/zlink-stream-connector.jspre'],
      bundle: true,
      platform: 'browser',
      format: 'iife',
      outfile: output,
      metafile: true,
      loader: { '.jspre': 'js' },
      logLevel: 'silent'
    });

    const inputs = Object.keys(result.metafile.inputs)
      .map((input) => input.split(path.sep).join('/'));
    const forbiddenModules = /(^|\/)(node:)?(net|tls|async_hooks|crypto)(\.[cm]?[jt]s)?$/;
    assert.equal(inputs.some((input) => forbiddenModules.test(input)), false, inputs.join('\n'));

    const bundle = fs.readFileSync(output, 'utf8');
    assert.doesNotMatch(bundle, /node:(?:net|tls|async_hooks|crypto)/);
    assert.doesNotMatch(bundle, /require\(["'](?:net|tls|async_hooks|crypto)["']\)/);
    assert.doesNotMatch(bundle, /\bBuffer\b/);
  } finally {
    fs.rmSync(outputDirectory, { recursive: true, force: true });
  }
});

test('the jslib exports exactly the boundary the C# side declares', () => {
  const jslib = fs.readFileSync(path.join(packageRoot, 'Plugins/WebGL/ZlinkStreamConnector.jslib'), 'utf8');
  const interop = fs.readFileSync(
    path.join(packageRoot, 'Runtime/Interop/ZlinkStreamInterop.cs'),
    'utf8'
  );

  const exported = [...jslib.matchAll(/^ {2}(Zlink\w+): function/gm)].map((match) => match[1]).sort();
  const imported = [...interop.matchAll(/EntryPoint = "(Zlink\w+)"/g)].map((match) => match[1]).sort();
  assert.ok(exported.length > 0, 'the jslib declares boundary functions');
  assert.deepEqual(imported, exported, 'every DllImport has a jslib function and the reverse');
});
