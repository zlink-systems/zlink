'use strict';
var __importDefault = (this && this.__importDefault) || function (mod) {
    return (mod && mod.__esModule) ? mod : { "default": mod };
};
Object.defineProperty(exports, "__esModule", { value: true });
const node_test_1 = __importDefault(require("node:test"));
const strict_1 = __importDefault(require("node:assert/strict"));
const node_fs_1 = __importDefault(require("node:fs"));
const node_path_1 = __importDefault(require("node:path"));
function collectTypeScriptFiles(root) {
    return node_fs_1.default.readdirSync(root, { withFileTypes: true }).flatMap((entry) => {
        const fullPath = node_path_1.default.join(root, entry.name);
        if (entry.isDirectory())
            return collectTypeScriptFiles(fullPath);
        return entry.isFile() && entry.name.endsWith('.ts') ? [fullPath] : [];
    });
}
function collectScriptFiles(root) {
    return node_fs_1.default.readdirSync(root, { withFileTypes: true }).flatMap((entry) => {
        const fullPath = node_path_1.default.join(root, entry.name);
        if (entry.isDirectory())
            return collectScriptFiles(fullPath);
        return entry.isFile() && /\.(?:js|ts)$/.test(entry.name) ? [fullPath] : [];
    });
}
function runtimeSpecifiers(source) {
    const specifiers = [];
    const patterns = [
        /(?:import|export)\s+(?:[\s\S]*?\s+from\s+)?['"]([^'"]+)['"]/g,
        /\brequire\s*\(\s*['"]([^'"]+)['"]\s*\)/g,
        /\bimport\s*\(\s*['"]([^'"]+)['"]\s*\)/g
    ];
    for (const pattern of patterns) {
        for (const match of source.matchAll(pattern)) {
            const normalized = match[1].replaceAll('\\', '/').toLowerCase();
            const delimited = `/${normalized}/`;
            if (delimited.includes('/runtime/') || delimited.includes('/native/')) {
                specifiers.push(match[1]);
            }
        }
    }
    return specifiers;
}
function forbiddenPackageExports(exportsValue) {
    if (!exportsValue || typeof exportsValue !== 'object')
        return [];
    return Object.keys(exportsValue).filter((key) => key !== '.');
}
(0, node_test_1.default)('contract source does not import or re-export runtime implementation', () => {
    strict_1.default.deepEqual(runtimeSpecifiers("import { Message } from '../messaging/message';"), []);
    strict_1.default.deepEqual(runtimeSpecifiers("import { Native } from '../../runtime/native/native';"), [
        '../../runtime/native/native'
    ]);
    strict_1.default.deepEqual(runtimeSpecifiers("export { Socket } from '../runtime/sockets/socket';"), [
        '../runtime/sockets/socket'
    ]);
    strict_1.default.deepEqual(runtimeSpecifiers("const addon = require('../../native/addon');"), [
        '../../native/addon'
    ]);
    const contractRoot = node_path_1.default.resolve(__dirname, '../../src/zlink/contracts');
    const violations = collectTypeScriptFiles(contractRoot).flatMap((file) => runtimeSpecifiers(node_fs_1.default.readFileSync(file, 'utf8')).map((specifier) => ({ file, specifier })));
    strict_1.default.deepEqual(violations, []);
});
(0, node_test_1.default)('package exports expose only the public root', () => {
    strict_1.default.deepEqual(forbiddenPackageExports({ '.': './dist/index.js' }), []);
    strict_1.default.deepEqual(forbiddenPackageExports({ '.': './dist/index.js', './runtime/native': './dist/runtime/native.js' }), ['./runtime/native']);
    const packageJson = JSON.parse(node_fs_1.default.readFileSync(node_path_1.default.resolve(__dirname, '../../package.json'), 'utf8'));
    strict_1.default.deepEqual(forbiddenPackageExports(packageJson.exports), []);
});
(0, node_test_1.default)('managed routed admission uses only exact per-part Core APIs', () => {
    const nativeBridge = node_fs_1.default.readFileSync(node_path_1.default.resolve(__dirname, '../../native/src/addon_routed_admission.cc'), 'utf8');
    for (const symbol of [
        'zlink_routed_send_ready_handler',
        'zlink_select_routed_submit_target',
        'zlink_dealer_send_transport_pair_part',
        'zlink_dealer_request_transport_pair_part',
        'zlink_send_part_transport_pair',
        'zlink_router_request_transport_pair_part'
    ]) {
        strict_1.default.ok(nativeBridge.includes(symbol), symbol);
    }
    strict_1.default.equal(nativeBridge.includes('zlink_routed_send_parts'), false);
    strict_1.default.equal(nativeBridge.includes('zlink_routed_request_parts'), false);
});
(0, node_test_1.default)('bindings samples stay on the Core 0.10.1 raw socket boundary', () => {
    const sampleRoots = [
        node_path_1.default.resolve(__dirname, '../../samples'),
        node_path_1.default.resolve(__dirname, '../../../javascript/samples')
    ];
    const forbidden = [
        /\bcreateMeshNode\s*\(/,
        /\bcreateActor\s*\(/,
        /\bcreateSpot\s*\(/,
        /\bjoinActorSpot\s*\(/,
        /\bbindActor\s*\(/
    ];
    const violations = sampleRoots.flatMap((root) => collectScriptFiles(root).flatMap((file) => {
        const source = node_fs_1.default.readFileSync(file, 'utf8');
        return forbidden.some((pattern) => pattern.test(source)) ? [file] : [];
    }));
    strict_1.default.deepEqual(violations, [], 'Actor, Spot and session service examples belong to framework samples');
});
