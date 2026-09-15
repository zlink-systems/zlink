// Links the Unity WebGL plugins with emscripten, the way Unity links them.
//
// Unity 2023.2 and later - Unity 6 included - bundle "Emscripten 3.1.38-unity"
// (Unity manual, Web native plug-ins for Emscripten), so the harness pins
// upstream 3.1.38. Set ZLINK_EMSDK_VERSION to link against another one.
//
// emsdk is not vendored and is not installed by this repository's setup. When it
// is missing the test skips with the install command, so a checkout without it
// still runs the rest of the suite.
const childProcess = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const EMSDK_VERSION = process.env.ZLINK_EMSDK_VERSION ?? '3.1.38';

const packageRoot = path.resolve(__dirname, '../../../../../unity/com.zlink.stream-connector.webgl');
const pluginRoot = path.join(packageRoot, 'Plugins/WebGL');

// One line, because a test runner prints a skip reason with the newlines escaped.
const INSTALL_HINT =
  'emscripten not found, so the Unity WebGL plugins cannot be linked. Install it with: ' +
  'git clone https://github.com/emscripten-core/emsdk.git ~/.cache/zlink/emsdk && ' +
  `~/.cache/zlink/emsdk/emsdk install ${EMSDK_VERSION} && ` +
  `~/.cache/zlink/emsdk/emsdk activate ${EMSDK_VERSION} ` +
  '(or point ZLINK_EMSDK_ROOT at an existing emsdk checkout).';

/** Returns { emcc, config } for the emsdk to use, or undefined when there is none. */
function findEmscripten() {
  const candidates = [];
  if (process.env.ZLINK_EMSDK_ROOT) candidates.push(process.env.ZLINK_EMSDK_ROOT);
  if (process.env.EMSDK) candidates.push(process.env.EMSDK);
  candidates.push(path.join(os.homedir(), '.cache/zlink/emsdk'));

  for (const root of candidates) {
    const emcc = path.join(root, 'upstream/emscripten', process.platform === 'win32' ? 'emcc.bat' : 'emcc');
    const config = path.join(root, '.emscripten');
    if (fs.existsSync(emcc) && fs.existsSync(config)) return { emcc, config };
  }

  // A shell that already has emsdk_env.sh sourced.
  if (process.env.EMSDK_PYTHON || process.env.EM_CONFIG) {
    const emcc = process.platform === 'win32' ? 'emcc.bat' : 'emcc';
    const probe = childProcess.spawnSync(emcc, ['--version'], { encoding: 'utf8' });
    if (probe.status === 0) return { emcc, config: process.env.EM_CONFIG };
  }

  return undefined;
}

/**
 * Flags chosen to match a Unity WebGL player link:
 *  - MODULARIZE + EXPORT_NAME: Unity emits `unityFramework` the same way, so the
 *    .jspre files land inside the module closure the jslib shares, which is where
 *    ZlinkStreamConnector.jslib resolves ZlinkStreamWebGlRuntime from.
 *  - ALLOW_MEMORY_GROWTH: Unity's default, and the reason the jslib re-reads
 *    HEAPU8 on every pump instead of caching the view.
 *  - ENVIRONMENT=web: a player build targets the browser only.
 *  - EXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString: for the page driver, not
 *    for the plugins. The plugins get _malloc, _free, HEAPU8, UTF8ToString,
 *    stringToUTF8, lengthBytesUTF8 and wasmTable from the link itself.
 *  - -O1 and not -O2: see the comment on OPTIMIZATION below.
 *
 * OPTIMIZATION: -O2 and above run emscripten 3.1.38's acorn-based JS optimizer
 * over the concatenated --pre-js content, and that parser is fixed at
 * ecmaVersion 2020. zlink-stream-connector.jspre is an esbuild es2022 bundle and
 * declares `static` class fields, which ES2020 cannot parse, so the link dies
 * with "SyntaxError: Unexpected token". -O1 does not run that pass.
 */
const OPTIMIZATION = process.env.ZLINK_EMSCRIPTEN_OPT ?? '-O1';

function emccArguments(source, output, options = {}) {
  const { optimization = OPTIMIZATION, exportAllocator = true } = options;
  return [
    source,
    optimization,
    '-sWASM=1',
    '-sMODULARIZE=1',
    '-sEXPORT_NAME=zlinkHarnessFramework',
    '-sALLOW_MEMORY_GROWTH=1',
    '-sINITIAL_MEMORY=33554432',
    '-sENVIRONMENT=web',
    '-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString',
    // ZlinkStreamConnector.jslib calls _malloc and _free without declaring them
    // in a __deps list, so the JS bindings for them exist only when the link
    // exports them. Unity's WebGL link does - its own jslib documentation writes
    // `var buffer = _malloc(lengthBytesUTF8(str) + 1)` - so a Unity-equivalent
    // link passes them here. See the "needs the host link to export" test for
    // what happens without them.
    ...(exportAllocator ? ['-sEXPORTED_FUNCTIONS=_main,_malloc,_free'] : []),
    '--js-library', path.join(pluginRoot, 'ZlinkStreamConnector.jslib'),
    '--pre-js', path.join(pluginRoot, 'zlink-stream-connector.jspre'),
    '--pre-js', path.join(pluginRoot, 'ZlinkStreamRuntime.jspre'),
    '-o', output
  ];
}

function runEmcc(emscripten, args) {
  return childProcess.spawnSync(emscripten.emcc, args, {
    encoding: 'utf8',
    env: { ...process.env, EM_CONFIG: emscripten.config },
    timeout: 600_000
  });
}

const HARNESS_SOURCE = path.join(__dirname, 'harness.c');

/** Links harness.c against the plugins. Returns { js, wasm } paths. */
function buildHarness(emscripten, outputDirectory, options = {}) {
  fs.mkdirSync(outputDirectory, { recursive: true });
  const output = path.join(outputDirectory, 'harness.js');
  const result = runEmcc(emscripten, emccArguments(HARNESS_SOURCE, output, options));
  if (result.status !== 0 || !fs.existsSync(output)) {
    throw new Error(`emcc failed (${result.status}):\n${result.stdout ?? ''}${result.stderr ?? ''}`);
  }
  return { js: output, wasm: path.join(outputDirectory, 'harness.wasm'), log: result.stderr ?? '' };
}

module.exports = {
  EMSDK_VERSION,
  INSTALL_HINT,
  OPTIMIZATION,
  HARNESS_SOURCE,
  buildHarness,
  emccArguments,
  findEmscripten,
  packageRoot,
  pluginRoot,
  runEmcc
};
