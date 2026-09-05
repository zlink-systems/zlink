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
(0, node_test_1.default)('Node requests and writable send retries use pull completion without callback bridges', () => {
    const nativeRoot = node_path_1.default.resolve(__dirname, '../../native/src');
    const sourceRoot = node_path_1.default.resolve(__dirname, '../../src/zlink');
    const gyp = node_fs_1.default.readFileSync(node_path_1.default.resolve(__dirname, '../../binding.gyp'), 'utf8');
    const nativeBridge = node_fs_1.default.readFileSync(node_path_1.default.join(nativeRoot, 'addon_core.cc'), 'utf8');
    const socketBinding = node_fs_1.default.readFileSync(node_path_1.default.join(sourceRoot, 'runtime/native/binding_socket.ts'), 'utf8');
    const completionOwner = node_fs_1.default.readFileSync(node_path_1.default.join(sourceRoot, 'runtime/messaging/completion_owner.ts'), 'utf8');
    const poller = node_fs_1.default.readFileSync(node_path_1.default.join(sourceRoot, 'runtime/eventing/poller.ts'), 'utf8');
    strict_1.default.equal(node_fs_1.default.existsSync(node_path_1.default.join(nativeRoot, 'addon_request_callbacks.cc')), false);
    strict_1.default.equal(node_fs_1.default.existsSync(node_path_1.default.join(nativeRoot, 'addon_request_callbacks.h')), false);
    strict_1.default.equal(gyp.includes('addon_request_callbacks.cc'), false);
    for (const symbol of [
        'zlink_completion_recv',
        'zlink_completion_close',
        'socket_submit_send',
        'socket_submit_request',
        'socket_completion_recv',
    ])
        strict_1.default.ok(nativeBridge.includes(symbol), symbol);
    for (const removed of [
        'zlink_send_async',
        'zlink_send_complete_handler',
        'napi_create_threadsafe_function',
    ])
        strict_1.default.equal(nativeBridge.includes(removed), false, removed);
    strict_1.default.ok(socketBinding.includes('socketCompletionRecv'));
    strict_1.default.ok(socketBinding.includes('socketSubmitSend'));
    strict_1.default.ok(completionOwner.includes('class CompletionEntry'));
    strict_1.default.ok(completionOwner.includes('byToken'));
    strict_1.default.ok(completionOwner.includes('byId'));
    strict_1.default.ok(completionOwner.includes('retries'));
    strict_1.default.ok(completionOwner.includes('COMPLETION_WRITABLE'));
    strict_1.default.ok(completionOwner.includes('awaitWritable'));
    strict_1.default.ok(completionOwner.includes('transferToPublic'));
    strict_1.default.ok(completionOwner.includes('socketReadableWatchStart'));
    strict_1.default.equal(completionOwner.includes('COMPLETION_SEND'), false);
    strict_1.default.equal(completionOwner.includes('setInterval('), false);
    strict_1.default.equal(completionOwner.includes('setTimeout('), false);
    strict_1.default.equal(completionOwner.includes('setImmediate('), false);
    strict_1.default.equal(completionOwner.includes('Atomics.wait'), false);
    strict_1.default.ok(poller.includes('instanceof MonitorSocket'));
    strict_1.default.ok(poller.includes('_registrationsByToken'));
    strict_1.default.ok(poller.includes('socketRegistration?.owner'));
    strict_1.default.ok(poller.includes('owner.drain(this)'));
    strict_1.default.ok(nativeBridge.includes('completion_close_guard_t guard'));
    strict_1.default.ok(nativeBridge.includes('uv_poll_start'));
});
(0, node_test_1.default)('native multipart replies use inline staging without changing rejection ownership', () => {
    const nativeBridge = node_fs_1.default.readFileSync(node_path_1.default.resolve(__dirname, '../../native/src/addon_core.cc'), 'utf8');
    for (const symbol of ['napi_value socket_reply']) {
        const start = nativeBridge.indexOf(symbol);
        strict_1.default.ok(start >= 0, symbol);
        const body = nativeBridge.slice(start, nativeBridge.indexOf('\nnapi_value ', start + symbol.length));
        strict_1.default.ok(body.includes('small_msg_storage_t parts'), `${symbol} inline storage`);
        strict_1.default.ok(body.includes('parts.release ()'), `${symbol} must not double-close rejected slots`);
        strict_1.default.equal(body.includes('std::vector<zlink_msg_t> parts'), false, `${symbol} must not allocate a vector for multipart replies`);
    }
});
(0, node_test_1.default)('bindings samples stay on the Core 0.16 raw socket boundary', () => {
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
