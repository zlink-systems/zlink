// SPDX-License-Identifier: MPL-2.0
'use strict';
var __importDefault = (this && this.__importDefault) || function (mod) {
    return (mod && mod.__esModule) ? mod : { "default": mod };
};
Object.defineProperty(exports, "__esModule", { value: true });
const node_test_1 = __importDefault(require("node:test"));
const strict_1 = __importDefault(require("node:assert/strict"));
const node_child_process_1 = require("node:child_process");
const node_fs_1 = __importDefault(require("node:fs"));
const node_path_1 = __importDefault(require("node:path"));
const { parseCommonArgs } = require('../perf/common/perf_args');
const packageRoot = node_path_1.default.resolve(__dirname, '../..');
const multiRunner = node_path_1.default.join(packageRoot, 'perf/multi/run_benchmarks.sh');
const multiSource = node_fs_1.default.readFileSync(node_path_1.default.join(packageRoot, 'perf/multi/run_benchmarks.ts'), 'utf8');
const multiShell = node_fs_1.default.readFileSync(multiRunner, 'utf8');
const singleSource = node_fs_1.default.readFileSync(node_path_1.default.join(packageRoot, 'perf/single/run_benchmarks.ts'), 'utf8');
const singleShell = node_fs_1.default.readFileSync(node_path_1.default.join(packageRoot, 'perf/single/run_benchmarks.sh'), 'utf8');
const packageJson = JSON.parse(node_fs_1.default.readFileSync(node_path_1.default.join(packageRoot, 'package.json'), 'utf8'));
const defaults = {
    pattern: 'ALL',
    duration: 5,
    msgSizes: [1024],
    resultsDir: 'perf/results',
    transports: ['tcp'],
    clients: 100
};
(0, node_test_1.default)('multi runner help documents the supported I/O-thread contract', () => {
    strict_1.default.match(multiSource, /--io-threads N\s+Set both server\/client I\/O threads \(default: 4\)\./);
    strict_1.default.match(multiSource, /--server-io-threads N\s+Override server I\/O threads \(default: 4\)\./);
    strict_1.default.match(multiSource, /--client-io-threads N\s+Override client I\/O threads \(default: 4\)\./);
    const options = parseCommonArgs([
        '--io-threads', '3',
        '--server-io-threads', '5',
        '--client-io-threads', '7'
    ], defaults);
    strict_1.default.equal(options.ioThreads, 3);
    strict_1.default.equal(options.serverIoThreads, 5);
    strict_1.default.equal(options.clientIoThreads, 7);
});
(0, node_test_1.default)('Node perf rejects build-dir instead of consuming and ignoring it', () => {
    const expected = /--build-dir is not supported by the Node perf runner/;
    strict_1.default.throws(() => parseCommonArgs(['--build-dir', 'alternate-build'], defaults), expected);
    strict_1.default.throws(() => parseCommonArgs(['--build-dir=alternate-build'], defaults), expected);
    if (process.platform !== 'win32') {
        const result = (0, node_child_process_1.spawnSync)(multiRunner, ['--build-dir', 'alternate-build'], {
            cwd: packageRoot,
            encoding: 'utf8'
        });
        strict_1.default.equal(result.status, 1);
        strict_1.default.match(result.stderr, expected);
        strict_1.default.doesNotMatch(result.stdout, /> @zlink-systems\/zlink@.* build/);
    }
});
(0, node_test_1.default)('Node build modes keep latest-source, reuse, and clean semantics explicit', () => {
    strict_1.default.doesNotThrow(() => parseCommonArgs(['--reuse-build'], defaults));
    strict_1.default.doesNotThrow(() => parseCommonArgs(['--clean-build'], defaults));
    strict_1.default.match(multiSource, /--reuse-build\s+Reuse existing fixed Node outputs; skip rebuild\./);
    strict_1.default.match(multiSource, /--clean-build\s+Remove TypeScript and native build outputs, then rebuild\./);
    strict_1.default.match(singleSource, /--reuse-build\s+Reuse existing fixed Node outputs; skip rebuild\./);
    strict_1.default.match(singleSource, /--clean-build\s+Remove TypeScript and native build outputs, then rebuild\./);
    strict_1.default.ok(multiShell.includes('npm run build:incremental'));
    strict_1.default.ok(multiShell.includes('npm run rebuild-native'));
    strict_1.default.ok(multiShell.includes('rm -rf "$ROOT_DIR/dist" "$ROOT_DIR/dist-tools" "$ROOT_DIR/build"'));
    strict_1.default.ok(singleShell.includes('npm run build:incremental'));
    strict_1.default.ok(singleShell.includes('npm run rebuild-native'));
    strict_1.default.ok(singleShell.includes('rm -rf "$ROOT_DIR/dist" "$ROOT_DIR/dist-tools" "$ROOT_DIR/build"'));
    strict_1.default.equal(packageJson.scripts.build, 'npm run clean && npm run build:incremental');
    strict_1.default.equal(packageJson.scripts['build:incremental'], 'tsc -p tsconfig.json && node scripts/generate_esm_wrapper.js '
        + '&& tsc -p tsconfig.tools.json && node scripts/link_dist_tools.js');
    strict_1.default.ok(multiShell.includes('dist-tools/perf/multi/run_benchmarks.js'));
    strict_1.default.ok(multiShell.includes('dist/index.js'));
    strict_1.default.ok(multiShell.includes('build/Release/zlink.node'));
    strict_1.default.ok(multiShell.includes('prebuilds/$PREBUILD_PLATFORM/zlink.node'));
    if (process.platform !== 'win32') {
        const result = (0, node_child_process_1.spawnSync)(multiRunner, ['--reuse-build', '--clean-build'], {
            cwd: packageRoot,
            encoding: 'utf8'
        });
        strict_1.default.equal(result.status, 1);
        strict_1.default.match(result.stderr, /--reuse-build and --clean-build are mutually exclusive/);
        strict_1.default.doesNotMatch(result.stdout, /> @zlink-systems\/zlink@.* build/);
    }
});
