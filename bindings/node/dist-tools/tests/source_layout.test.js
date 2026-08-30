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
(0, node_test_1.default)('Node async surfaces use Core completion callbacks and have no readiness admission files', () => {
    const nativeRoot = node_path_1.default.resolve(__dirname, '../../native/src');
    const sourceRoot = node_path_1.default.resolve(__dirname, '../../src/zlink');
    const gyp = node_fs_1.default.readFileSync(node_path_1.default.resolve(__dirname, '../../binding.gyp'), 'utf8');
    const nativeBridge = node_fs_1.default.readFileSync(node_path_1.default.join(nativeRoot, 'addon_core.cc'), 'utf8');
    const requestBridge = node_fs_1.default.readFileSync(node_path_1.default.join(nativeRoot, 'addon_request_callbacks.cc'), 'utf8');
    const socketBinding = node_fs_1.default.readFileSync(node_path_1.default.join(sourceRoot, 'runtime/native/binding_socket.ts'), 'utf8');
    const sendCompletion = node_fs_1.default.readFileSync(node_path_1.default.join(sourceRoot, 'runtime/messaging/send_completion.ts'), 'utf8');
    strict_1.default.equal(node_fs_1.default.existsSync(node_path_1.default.join(nativeRoot, 'addon_routed_admission.cc')), false);
    strict_1.default.equal(node_fs_1.default.existsSync(node_path_1.default.join(sourceRoot, 'runtime/sockets/routed_admission.ts')), false);
    strict_1.default.equal(node_fs_1.default.existsSync(node_path_1.default.join(sourceRoot, 'runtime/sockets/publisher_admission.ts')), false);
    strict_1.default.equal(node_fs_1.default.existsSync(node_path_1.default.join(sourceRoot, 'runtime/messaging/request_progress.ts')), false);
    strict_1.default.equal(gyp.includes('addon_routed_admission.cc'), false);
    for (const symbol of [
        'zlink_send_complete_handler',
        'zlink_send_async',
        'napi_create_threadsafe_function',
        'socket_send_async',
        'dealer_request',
        'router_request'
    ]) {
        strict_1.default.ok(nativeBridge.includes(symbol), symbol);
    }
    strict_1.default.ok(requestBridge.includes('napi_call_threadsafe_function'));
    strict_1.default.ok(socketBinding.includes('socketSendCompletionHandler'));
    strict_1.default.ok(socketBinding.includes('socketSendAsync'));
    strict_1.default.ok(sendCompletion.includes('new Map<bigint, PendingSend>()'));
    strict_1.default.ok(nativeBridge.includes('struct send_completion_js_payload_t'));
    strict_1.default.ok(nativeBridge.includes('std::unique_ptr<send_completion_js_payload_t> payload'));
    strict_1.default.ok(nativeBridge.includes('tsfn, payload.get (), napi_tsfn_nonblocking'));
    strict_1.default.ok(nativeBridge.includes('payload->completion = operation->completion'));
    strict_1.default.ok(nativeBridge.includes('send_completion_delivery_accounting_t accounting'));
    strict_1.default.ok(nativeBridge.indexOf('send_completion_delivery_accounting_t accounting')
        < nativeBridge.indexOf('if (!env || !js_callback || !payload)'));
    strict_1.default.equal(nativeBridge.includes('send_ready'), false);
    strict_1.default.equal(nativeBridge.includes('routed_send_ready'), false);
    strict_1.default.equal(socketBinding.includes('sendReady'), false);
});
(0, node_test_1.default)('native multipart replies use inline staging without changing rejection ownership', () => {
    const nativeBridge = node_fs_1.default.readFileSync(node_path_1.default.resolve(__dirname, '../../native/src/addon_core.cc'), 'utf8');
    for (const symbol of ['napi_value dealer_reply', 'napi_value router_reply']) {
        const start = nativeBridge.indexOf(symbol);
        strict_1.default.ok(start >= 0, symbol);
        const body = nativeBridge.slice(start, nativeBridge.indexOf('\nnapi_value ', start + symbol.length));
        strict_1.default.ok(body.includes('small_msg_storage_t parts'), `${symbol} inline storage`);
        strict_1.default.ok(body.includes('parts.release ()'), `${symbol} must not double-close rejected slots`);
        strict_1.default.equal(body.includes('std::vector<zlink_msg_t> parts'), false, `${symbol} must not allocate a vector for multipart replies`);
    }
});
(0, node_test_1.default)('bindings samples stay on the Core 0.13.1 raw socket boundary', () => {
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
