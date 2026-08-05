// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
// Faithful port of the C perf report emitters so that node perf report
// output is byte-identical to the C reference for matching
// patterns/sizes (presentation only — measured values are unchanged):
//   - single: bindings/c/perf/single/run_comparison.py
//   - multi:  bindings/c/perf/run_comparison.py
// Only the human-readable layout is reproduced here; measurement,
// aggregation and the (already-fixed) termination logic stay in the
// runners.
const os = require('node:os');
const fs = require('node:fs');
const { execFileSync } = require('node:child_process');
const path = require('node:path');
const { resolveLatencyTriplet, fixed, padStart, padEnd } = require('./perf_c_format');
const STREAM_VARIANT_PATTERNS = new Set(['MULTI_STREAM']);
function envGet(name) {
    const value = process.env[name];
    return value ? value : '';
}
function parseEnvInt(name, fallback) {
    const value = envGet(name);
    if (!value) {
        return fallback;
    }
    const parsed = Number.parseInt(value, 10);
    return Number.isNaN(parsed) ? fallback : parsed;
}
function parseEnvPairInt(primary, fallbackName, fallback) {
    return parseEnvInt(primary, parseEnvInt(fallbackName, fallback));
}
// --- single table format (C single/run_comparison.py) -------------------
function singleTableHeaderLine() {
    return ('| Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |'
        + '   Lat.P95(ms) |   Lat.P99(ms) |');
}
function singleTableSeparatorLine() {
    return ('|----------|------------------|--------------|--------------|--------------|'
        + '--------------|');
}
function singleFormatThroughput(pattern, throughputPerSec) {
    // f"{tp/1e3:6.2f} Kmsg/s"
    const unit = pattern.endsWith('_REQREP') ? 'Kops/s' : 'Kmsg/s';
    return `${fixed(throughputPerSec / 1e3, 2, 6)} ${unit}`;
}
function singleFormatBandwidth(bandwidthMbS) {
    return `${fixed(bandwidthMbS, 2, 8)} MB/s`;
}
function singleFormatLatencyMs(latencyMs) {
    return `${fixed(latencyMs, 3, 8)} ms`;
}
function singleTableRowLine(size, status, record, pattern) {
    let tp;
    let bw;
    let lat;
    let lat95;
    let lat99;
    if (status === 'success' && record) {
        const [, p95, p99] = resolveLatencyTriplet(record.latency, record.latency_p95, record.latency_p99);
        tp = singleFormatThroughput(pattern, record.throughput);
        bw = singleFormatBandwidth(record.bandwidth);
        lat = singleFormatLatencyMs(record.latency);
        lat95 = singleFormatLatencyMs(p95 !== undefined && p95 !== null ? p95 : 0.0);
        lat99 = singleFormatLatencyMs(p99 !== undefined && p99 !== null ? p99 : 0.0);
    }
    else if (status === 'unsupported') {
        tp = bw = lat = lat95 = lat99 = 'UNSUPPORTED';
    }
    else {
        tp = bw = lat = lat95 = lat99 = 'FAIL';
    }
    return (`| ${padEnd(`${size}B`, 8)} | ${padStart(tp, 16)} | ${padStart(bw, 12)} | `
        + `${padStart(lat, 12)} | ${padStart(lat95, 12)} | ${padStart(lat99, 12)} |`);
}
// --- multi table format (C run_comparison.py _emit_table_*) -------------
function multiTableHeaderLine() {
    // Size:8, Throughput:>18, Bandwidth:>14, Lat.* :>13
    return (`| ${padEnd('Size', 8)} | ${padStart('Throughput', 18)} | `
        + `${padStart('Bandwidth', 14)} | ${padStart('Lat.Mean(ms)', 13)} | `
        + `${padStart('Lat.P95(ms)', 13)} | ${padStart('Lat.P99(ms)', 13)} |`);
}
function multiTableSeparatorLine() {
    return (`|${'-'.repeat(10)}|${'-'.repeat(20)}|${'-'.repeat(16)}|`
        + `${'-'.repeat(15)}|${'-'.repeat(15)}|${'-'.repeat(15)}|`);
}
function isEchoPattern(pattern) {
    return pattern === 'MULTI_DEALER_ROUTER'
        || pattern === 'MULTI_DEALER_ROUTER_REQREP'
        || pattern === 'MULTI_ROUTER_ROUTER'
        || pattern === 'MULTI_ROUTER_ROUTER_REQREP'
        || pattern === 'MULTI_STREAM'
        || pattern === 'MULTI_SPOT_REQREP'
        || pattern === 'MULTI_SPOT_SENDSEND';
}
function multiFormatThroughput(pattern, throughputPerSec) {
    const unit = isEchoPattern(pattern) ? 'Kops/s' : 'Kmsg/s';
    return `${fixed(throughputPerSec / 1e3, 3, 8)} ${unit}`;
}
function multiFormatBandwidth(bandwidthMbS) {
    return `${fixed(bandwidthMbS, 3, 10)} MB/s`;
}
function multiTableRowLine(pattern, size, status, record) {
    let tp;
    let bw;
    let lat;
    let lat95;
    let lat99;
    if (status === 'success' && record) {
        const [mean, p95, p99] = resolveLatencyTriplet(record.latency, record.latency_p95, record.latency_p99);
        tp = multiFormatThroughput(pattern, record.throughput !== undefined ? record.throughput : 0.0);
        bw = multiFormatBandwidth(record.bandwidth !== undefined ? record.bandwidth : 0.0);
        lat = `${fixed(mean !== undefined && mean !== null ? mean : 0.0, 3, 9)} ms`;
        lat95 = `${fixed(p95 !== undefined && p95 !== null ? p95 : 0.0, 3, 9)} ms`;
        lat99 = `${fixed(p99 !== undefined && p99 !== null ? p99 : 0.0, 3, 9)} ms`;
    }
    else {
        const token = status === 'unsupported' ? 'UNSUPPORTED' : 'FAIL';
        tp = bw = lat = lat95 = lat99 = token;
    }
    return (`| ${padEnd(`${size}B`, 8)} | ${padStart(tp, 16)} | ${padStart(bw, 12)} | `
        + `${padStart(lat, 12)} | ${padStart(lat95, 12)} | ${padStart(lat99, 12)} |`);
}
// --- META preamble (C run_comparison.py build_meta_items) ---------------
function getOsLabel() {
    const system = (() => {
        switch (process.platform) {
            case 'linux': return 'Linux';
            case 'darwin': return 'Darwin';
            case 'win32': return 'Windows';
            default: return process.platform;
        }
    })();
    const release = os.release();
    if (system && release) {
        return `${system} ${release}`;
    }
    return `${system}`;
}
function getCpuModel() {
    try {
        if (process.platform === 'linux') {
            const cpuinfo = fs.readFileSync('/proc/cpuinfo', 'utf8');
            for (const line of cpuinfo.split('\n')) {
                if (line.toLowerCase().startsWith('model name')) {
                    const idx = line.indexOf(':');
                    if (idx >= 0) {
                        const model = line.slice(idx + 1).trim();
                        if (model) {
                            return model;
                        }
                    }
                }
            }
        }
        else if (process.platform === 'darwin') {
            const out = execFileSync('sysctl', ['-n', 'machdep.cpu.brand_string'], {
                encoding: 'utf8',
                stdio: ['ignore', 'pipe', 'ignore']
            }).trim();
            if (out) {
                return out;
            }
        }
        const cpus = os.cpus();
        if (cpus && cpus.length > 0 && cpus[0].model) {
            return cpus[0].model.trim();
        }
    }
    catch {
        // fall through
    }
    return 'unknown';
}
function getLoadAvg() {
    try {
        const vals = os.loadavg();
        if (!vals || vals.length === 0) {
            return '';
        }
        return vals.map((value) => fixed(value, 2, 0)).join(' ');
    }
    catch {
        return '';
    }
}
function getCommitShortSha(rootDir) {
    try {
        return execFileSync('git', ['-C', rootDir, 'rev-parse', '--short', 'HEAD'], {
            encoding: 'utf8',
            stdio: ['ignore', 'pipe', 'ignore']
        }).trim();
    }
    catch {
        return 'unknown';
    }
}
function detectBuildType() {
    return 'Release';
}
function resolveClientsMeta(selectedPatterns, clientsOverride = undefined) {
    if (Number.isFinite(clientsOverride) && clientsOverride > 0) {
        return String(Math.trunc(clientsOverride));
    }
    const envClients = envGet('PERF_MULTI_CLIENTS') || envGet('PERF_CLIENTS');
    if (/^\d+$/.test(envClients)) {
        return envClients;
    }
    if (!selectedPatterns || selectedPatterns.length === 0) {
        return '';
    }
    const streamDefault = envGet('PERF_MULTI_DEFAULT_STREAM_CLIENTS') || envGet('PERF_STREAM_DEFAULT_CLIENTS') || '10000';
    const generalDefault = envGet('PERF_MULTI_DEFAULT_CLIENTS') || envGet('PERF_DEFAULT_CLIENTS') || '100';
    if (selectedPatterns.every((p) => STREAM_VARIANT_PATTERNS.has(p))) {
        return streamDefault;
    }
    if (selectedPatterns.some((p) => STREAM_VARIANT_PATTERNS.has(p))) {
        return `${generalDefault} (stream=${streamDefault})`;
    }
    return generalDefault;
}
function buildMetaItems(lang, numRuns, selectedPatterns, rootDir, clientsOverride = undefined) {
    const items = [];
    items.push(['os', getOsLabel()]);
    items.push(['cpu', getCpuModel()]);
    items.push(['cores', String(os.cpus() ? os.cpus().length : 0)]);
    items.push(['build', detectBuildType()]);
    items.push(['commit', getCommitShortSha(rootDir)]);
    const now = new Date();
    items.push(['timestamp', isoWithOffset(now)]);
    const loadAvg = getLoadAvg();
    if (loadAvg) {
        items.push(['load_avg', loadAvg]);
    }
    items.push(['runs', String(numRuns)]);
    const clients = resolveClientsMeta(selectedPatterns, clientsOverride);
    if (clients) {
        items.push(['clients', clients]);
    }
    return items;
}
function isoWithOffset(date) {
    const pad = (value) => String(value).padStart(2, '0');
    const tzMin = -date.getTimezoneOffset();
    const sign = tzMin >= 0 ? '+' : '-';
    const abs = Math.abs(tzMin);
    const tz = `${sign}${pad(Math.floor(abs / 60))}:${pad(abs % 60)}`;
    return (`${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())}`
        + `T${pad(date.getHours())}:${pad(date.getMinutes())}:${pad(date.getSeconds())}${tz}`);
}
// --- effective options --------------------------------------------------
function buildSingleOptionItems(opts) {
    const ioThreads = Math.max(1, parseEnvInt('PERF_IO_THREADS', 1));
    return [
        ['runs', String(opts.runs)],
        ['duration_seconds', String(parseEnvInt('PERF_SINGLE_DURATION_SECONDS', opts.duration))],
        ['timeout_seconds', String(opts.timeoutSeconds)],
        ['fail_fast', envGet('PERF_FAIL_FAST') === '1' ? '1' : '0'],
        ['io_threads', String(ioThreads)],
        ['hwm', 'auto-hwm'],
        ['sndhwm', 'auto-hwm'],
        ['rcvhwm', 'auto-hwm'],
        ['sndbuf', '-1'],
        ['rcvbuf', '-1'],
        ['sndtimeo_ms', String(parseEnvInt('PERF_SINGLE_SNDTIMEO_MS', 200))],
        ['rcvtimeo_ms', String(parseEnvInt('PERF_SINGLE_RCVTIMEO_MS', 200))],
        ['ctx_auto_hwm_enable', envGet('PERF_CTX_AUTO_HWM_ENABLE') || 'core-default'],
        ['ctx_auto_hwm_profile', envGet('PERF_CTX_AUTO_HWM_PROFILE') || 'balanced'],
        ['patterns', opts.patterns.join(',')],
        ['transports', opts.transports.length > 0 ? [...new Set(opts.transports)].sort().join(',') : 'none'],
        ['msg_sizes', opts.msgSizes.length > 0 ? [...new Set(opts.msgSizes)].sort((a, b) => a - b).join(',') : 'none']
    ];
}
function buildMultiOptionItems(opts) {
    const transports = [...new Set(opts.transports)].sort();
    const sizes = [...new Set(opts.msgSizes)].sort((a, b) => a - b);
    const clientsMeta = resolveClientsMeta(opts.patterns, opts.clientsOverride) || '100';
    const clientsForConnect = Math.max(1, Number.parseInt(clientsMeta, 10) || 100);
    const connectRaw = Number.isFinite(opts.connectConcurrency)
        ? String(opts.connectConcurrency)
        : envGet('PERF_MULTI_CONNECT_CONCURRENCY');
    const connectDisplay = connectRaw
        ? connectRaw
        : `${clientsForConnect >= 10000 ? 1024 : 128} (default)`;
    const serviceClients = parseEnvInt('PERF_MULTI_SERVICE_CLIENTS', 0);
    const timeoutOverride = parseEnvPairInt('PERF_MULTI_TIMEOUT_SECONDS', 'PERF_TIMEOUT_SECONDS', 0);
    const roleIoDisplay = (role) => {
        const explicit = role === 'server' ? opts.serverIoThreads : opts.clientIoThreads;
        if (Number.isFinite(explicit)) {
            return String(explicit);
        }
        if (Number.isFinite(opts.ioThreads)) {
            return String(opts.ioThreads);
        }
        const roleKey = role === 'server' ? 'SERVER' : 'CLIENT';
        const values = new Set(opts.patterns.map((pattern) => {
            const streamSpecific = STREAM_VARIANT_PATTERNS.has(pattern)
                ? envGet(`PERF_MULTI_STREAM_${roleKey}_IO_THREADS`)
                : '';
            return streamSpecific
                || envGet(`PERF_MULTI_${roleKey}_IO_THREADS`)
                || envGet('PERF_IO_THREADS')
                || envGet('PERF_MULTI_DEFAULT_IO_THREADS')
                || envGet('PERF_DEFAULT_IO_THREADS')
                || '4';
        }));
        return values.size === 1
            ? `${[...values][0]} (default)`
            : `mixed-default(${[...values].sort().join(',')})`;
    };
    const optionValue = (explicit, envName, fallback) => {
        if (Number.isFinite(explicit)) {
            return String(explicit);
        }
        return String(parseEnvInt(envName, fallback));
    };
    return [
        ['runs', String(opts.runs)],
        ['patterns', opts.patterns.join(',')],
        ['transports', transports.length > 0 ? transports.join(',') : 'none'],
        ['msg_sizes', sizes.length > 0 ? sizes.join(',') : 'none'],
        ['duration_seconds', String(opts.duration)],
        ['fail_fast', envGet('PERF_FAIL_FAST') === '1' ? '1' : '0'],
        ['clients', clientsMeta],
        ['default_clients', envGet('PERF_MULTI_DEFAULT_CLIENTS') || envGet('PERF_DEFAULT_CLIENTS') || '100'],
        ['default_stream_clients', envGet('PERF_MULTI_DEFAULT_STREAM_CLIENTS') || envGet('PERF_STREAM_DEFAULT_CLIENTS') || '10000'],
        ['service_clients', serviceClients > 0 ? String(serviceClients) : 'auto'],
        ['server_io_threads', roleIoDisplay('server')],
        ['client_io_threads', roleIoDisplay('client')],
        ['hwm', 'auto-hwm'],
        ['sndhwm', 'auto-hwm'],
        ['rcvhwm', 'auto-hwm'],
        ['sndbuf', '-1'],
        ['rcvbuf', '-1'],
        ['ctx_auto_hwm_enable', envGet('PERF_CTX_AUTO_HWM_ENABLE') || '1'],
        ['ctx_auto_hwm_profile', envGet('PERF_MULTI_CTX_AUTO_HWM_PROFILE') || envGet('PERF_CTX_AUTO_HWM_PROFILE') || 'balanced'],
        ['sndtimeo_ms', optionValue(opts.sendTimeoutMs, 'PERF_MULTI_SNDTIMEO_MS', 200)],
        ['rcvtimeo_ms', optionValue(opts.recvTimeoutMs, 'PERF_MULTI_RCVTIMEO_MS', 200)],
        ['connect_concurrency', connectDisplay],
        ['connect_ready_timeout_ms', optionValue(opts.connectReadyTimeoutMs, 'PERF_MULTI_CONNECT_READY_TIMEOUT_MS', parseEnvInt('PERF_CONNECT_READY_TIMEOUT_MS', 5000))],
        ['monitor_hwm', optionValue(opts.monitorHwm, 'PERF_MULTI_MONITOR_HWM', 1000)],
        ['server_ready_timeout_ms', optionValue(opts.serverReadyTimeoutMs, 'PERF_MULTI_SERVER_READY_TIMEOUT_MS', parseEnvInt('PERF_SERVER_READY_TIMEOUT_MS', 10000))],
        ['server_shutdown_timeout_ms', optionValue(opts.serverShutdownTimeoutMs, 'PERF_MULTI_SERVER_SHUTDOWN_TIMEOUT_MS', parseEnvInt('PERF_SERVER_SHUTDOWN_TIMEOUT_MS', 5000))],
        ['server_bind_port', optionValue(opts.serverBindPort, 'PERF_MULTI_SERVER_BIND_PORT', 0)],
        ['transport_transition_ms', optionValue(opts.transportTransitionMs, 'PERF_MULTI_TRANSPORT_TRANSITION_MS', parseEnvInt('PERF_TRANSPORT_TRANSITION_MS', 3000))],
        ['pattern_transition_ms', optionValue(opts.patternTransitionMs, 'PERF_MULTI_PATTERN_TRANSITION_MS', parseEnvInt('PERF_PATTERN_TRANSITION_MS', 3000))],
        ['lat_timeout_ms', String(parseEnvInt('PERF_MULTI_LAT_TIMEOUT_MS', 5000))],
        ['stream_non_tcp_clients_max', String(parseEnvPairInt('PERF_STREAM_NON_TCP_CLIENTS_MAX', 'PERF_MULTI_STREAM_NON_TCP_CLIENTS_MAX', 1000))],
        ['disable_resource_metrics', String(Math.max(0, parseEnvInt('PERF_DISABLE_RESOURCE_METRICS', 0)))],
        ['timeout_seconds', timeoutOverride > 0 ? String(timeoutOverride) : 'auto']
    ];
}
function effectiveOptionLines(lang, suite, label, items) {
    const lines = [`## Effective Options (${label})`, `- lang: ${lang}`, `- suite: ${suite}`];
    for (const [key, value] of items) {
        lines.push(`- ${key}: ${value}`);
    }
    return lines;
}
// --- result data block (C emit_result_lines) ---------------------------
function resultDataLines(records) {
    // records: array of { pattern, transport, size, throughput, bandwidth,
    // latency, latency_p95, latency_p99 } that succeeded.
    const sorted = [...records].sort((a, b) => {
        if (a.pattern !== b.pattern)
            return a.pattern < b.pattern ? -1 : 1;
        if (a.transport !== b.transport)
            return a.transport < b.transport ? -1 : 1;
        return a.size - b.size;
    });
    const lines = [];
    for (const r of sorted) {
        // C single run_comparison.py emit_result_lines(): key is
        // (pattern,transport,size) only, then metrics are emitted in this
        // FIXED order. (C *multi* differs — see multiResultDataLines.)
        const metrics = [
            ['throughput', r.throughput],
            ['bandwidth', r.bandwidth],
            ['latency', r.latency],
            ['latency_p95', r.latency_p95],
            ['latency_p99', r.latency_p99]
        ];
        for (const [name, value] of metrics) {
            lines.push(`RESULT,current,${r.pattern},${r.transport},${r.size},${name},${fixed(value, 3, 0)}`);
        }
    }
    return lines;
}
// --- multi META preamble lines (C print_meta_lines) ---------------------
function metaLines(items) {
    return items.map(([key, value]) => `META,${key},${value}`);
}
// --- multi RESULT data block (C emit_result_lines) ----------------------
// C MULTI renders the RESULT block via the root run_comparison.py
// emit_result_lines(): `for key in sorted(result_map.keys())` where the
// key tuple is (pattern, transport, size, metric). Python's full-tuple
// sort therefore orders the per-(pattern,transport,size) metric lines
// ALPHABETICALLY by metric name:
//   bandwidth < latency < latency_p95 < latency_p99 < throughput
// This differs from C single (single/run_comparison.py keys only
// (pattern,transport,size) and emits a fixed throughput-first order —
// see resultDataLines). Emit the alphabetical order here so the multi
// RESULT block is byte-identical to the C multi reference.
function multiResultDataLines(records) {
    const sorted = [...records].sort((a, b) => {
        if (a.pattern !== b.pattern)
            return a.pattern < b.pattern ? -1 : 1;
        if (a.transport !== b.transport)
            return a.transport < b.transport ? -1 : 1;
        return a.size - b.size;
    });
    const lines = [];
    for (const r of sorted) {
        const metrics = [
            ['bandwidth', r.bandwidth],
            ['latency', r.latency],
            ['latency_p95', r.latency_p95],
            ['latency_p99', r.latency_p99],
            ['throughput', r.throughput]
        ];
        for (const [name, value] of metrics) {
            lines.push(`RESULT,current,${r.pattern},${r.transport},${r.size},${name},${fixed(value, 3, 0)}`);
        }
    }
    return lines;
}
// --- multi Auto-HWM detail tables (C run_comparison.py) -----------------
// Faithful port of bindings/c/perf/run_comparison.py auto-hwm detail
// emitters (emit_auto_hwm_detail_line .. emit_auto_hwm_detail_table,
// ~lines 813-1395). Presentation only.
const SPOT_CONTROL_PATTERNS = new Set(['SPOT', 'SPOT_REQREP', 'SPOT_SENDSEND']);
function normalizeMultiPatternName(patternName) {
    let pattern = String(patternName || '').trim().toUpperCase();
    if (pattern.startsWith('MULTI_')) {
        pattern = pattern.slice(6);
    }
    return pattern;
}
function autoHwmParseDetailLine(line) {
    const stripped = String(line || '').trim();
    if (!stripped.startsWith('AUTO_HWM_DETAIL,')) {
        return null;
    }
    const fields = {};
    for (const item of stripped.split(',').slice(1)) {
        const pos = item.indexOf('=');
        if (pos < 0) {
            continue;
        }
        fields[item.slice(0, pos).trim()] = item.slice(pos + 1).trim();
    }
    return fields;
}
function autoHwmDedupKey(fields) {
    return [
        'pattern', 'transport', 'component', 'label', 'msg_size', 'source',
        'role', 'managed_connections', 'active_connections', 'scope',
        'scope_count', 'sndhwm', 'rcvhwm', 'effective_message_bytes',
        'effective_sndbuf', 'effective_rcvbuf', 'socket_message_slots',
        'auto_buffer_bytes', 'manual_buffer_bytes', 'unit_budget_bytes'
    ].map((name) => fields[name] || '').join('');
}
// Stateful collector mirroring the module-level C lists/sets.
function createAutoHwmCollector() {
    const rows = [];
    const seen = new Set();
    const tableSeen = new Set();
    function addLine(line) {
        const fields = autoHwmParseDetailLine(line);
        if (!fields) {
            return;
        }
        const dedup = autoHwmDedupKey(fields);
        if (seen.has(dedup)) {
            return;
        }
        seen.add(dedup);
        fields._dedup_key = dedup;
        rows.push(fields);
    }
    function cellWidths(tableRows, columns) {
        return columns.map(([header, key]) => tableRows.reduce((w, f) => Math.max(w, String(f[key] !== undefined ? f[key] : '?').length), header.length));
    }
    function emitMarkdownTable(out, indent, columns, tableRows) {
        const widths = cellWidths(tableRows, columns);
        const headerCells = columns.map(([header], i) => ` ${String(header).padEnd(widths[i])} `);
        const sepCells = widths.map((w) => '-'.repeat(w + 2));
        out.push(`${indent}|${headerCells.join('|')}|`);
        out.push(`${indent}|${sepCells.join('|')}|`);
        for (const f of tableRows) {
            const cells = columns.map(([, key], i) => ` ${String(f[key] !== undefined ? f[key] : '?').padEnd(widths[i])} `);
            out.push(`${indent}|${cells.join('|')}|`);
        }
    }
    function parseInt10(value, fallback) {
        const n = Number.parseInt(String(value).trim(), 10);
        return Number.isNaN(n) ? fallback : n;
    }
    function bytesToKbDisplay(value) {
        const parsed = parseInt10(value, -1);
        if (parsed < 0) {
            return '';
        }
        return String(Math.trunc(parsed / 1024));
    }
    function patternIsSpot(patternName) {
        return SPOT_CONTROL_PATTERNS.has(normalizeMultiPatternName(patternName));
    }
    function spotScopeAndSocket(label) {
        const raw = String(label || '').trim();
        if (raw.startsWith('spotnode_')) {
            return ['shared', raw.slice('spotnode_'.length)];
        }
        if (raw.startsWith('spotend_')) {
            return ['per-spot', raw.slice('spotend_'.length)];
        }
        return ['', raw || 'socket'];
    }
    function activeHwmFields(row) {
        const socketType = String(row.socket_type || row.type || '').toLowerCase();
        const role = String(row.role || '').toLowerCase();
        let sendActive = true;
        let recvActive = true;
        if ((socketType === 'pub' || socketType === 'xpub')
            && (role === 'spot_data' || role === 'control')) {
            recvActive = false;
        }
        if ((socketType === 'sub' || socketType === 'xsub')
            && (role === 'recv_ingress' || role === 'control')) {
            sendActive = false;
        }
        return [sendActive, recvActive];
    }
    function applyActiveHwmDisplay(row) {
        const display = { ...row };
        const [sendActive, recvActive] = activeHwmFields(display);
        if (!sendActive) {
            display.sndhwm = '-';
        }
        if (!recvActive) {
            display.rcvhwm = '-';
        }
        return display;
    }
    function spotDisplayRows(spotRows, scope) {
        const displayRows = [];
        for (const fields of spotRows) {
            const [rs, socket] = spotScopeAndSocket(fields.label || '');
            const rowScope = fields.scope || rs;
            if (rowScope !== scope) {
                continue;
            }
            const display = { ...fields };
            display.scope = rowScope;
            display.socket = socket;
            display.managed = fields.managed_connections || '';
            display.active = fields.active_connections || '';
            displayRows.push(applyActiveHwmDisplay(display));
        }
        return displayRows;
    }
    function emitSpotSocketTable(out, title, tableRows) {
        if (tableRows.length === 0) {
            return false;
        }
        const displayRows = [];
        const localSeen = new Set();
        for (const row of tableRows) {
            const key = ['transport', 'socket', 'scope', 'source', 'role',
                'sndhwm', 'rcvhwm', 'effective_sndbuf', 'effective_rcvbuf']
                .map((n) => row[n] || '').join('');
            if (localSeen.has(key)) {
                continue;
            }
            localSeen.add(key);
            displayRows.push(row);
        }
        out.push(`    ${title}:`);
        emitMarkdownTable(out, '      ', [
            ['Socket', 'socket'], ['Scope', 'scope'], ['Source', 'source'],
            ['Role', 'role'], ['SNDHWM', 'sndhwm'], ['RCVHWM', 'rcvhwm'],
            ['SNDBUF', 'effective_sndbuf'], ['RCVBUF', 'effective_rcvbuf']
        ], displayRows);
        return true;
    }
    function emitSpotSnapshotSocketTable(out, title, tableRows) {
        if (tableRows.length === 0) {
            return false;
        }
        const sorted = [...tableRows].sort((a, b) => (parseInt10(a.msg_size || '0', 0) - parseInt10(b.msg_size || '0', 0)
            || parseInt10(a.owner_id || '0', 0) - parseInt10(b.owner_id || '0', 0)
            || String(a.socket || '').localeCompare(String(b.socket || ''))
            || String(a.role || '').localeCompare(String(b.role || ''))));
        const displayRows = [];
        const localSeen = new Set();
        for (const row of sorted) {
            let display = { ...row };
            display.managed = row.managed_connections || '';
            display.active = row.active_connections || '';
            display.type = row.socket_type || '';
            display = applyActiveHwmDisplay(display);
            const key = ['msg_size', 'effective_message_bytes', 'socket', 'type',
                'role', 'sndhwm', 'rcvhwm', 'effective_sndbuf', 'effective_rcvbuf']
                .map((n) => (display[n] !== undefined ? display[n] : '')).join('');
            if (localSeen.has(key)) {
                continue;
            }
            localSeen.add(key);
            displayRows.push(display);
        }
        if (displayRows.length === 0) {
            return false;
        }
        out.push(`    ${title}:`);
        const grouped = new Map();
        const groupOrder = [];
        for (const row of displayRows) {
            const gk = `${row.msg_size || ''}${row.effective_message_bytes || ''}`;
            if (!grouped.has(gk)) {
                grouped.set(gk, []);
                groupOrder.push(gk);
            }
            grouped.get(gk).push(row);
        }
        for (let index = 0; index < groupOrder.length; index += 1) {
            const gk = groupOrder[index];
            const [msgSize, msgUnit] = gk.split('');
            out.push(`      - Size(B)=${msgSize}, MsgUnit(B)=${msgUnit}`);
            emitMarkdownTable(out, '      ', [
                ['Socket', 'socket'], ['Type', 'type'], ['Role', 'role'],
                ['SNDHWM', 'sndhwm'], ['RCVHWM', 'rcvhwm'],
                ['SNDBUF', 'effective_sndbuf'], ['RCVBUF', 'effective_rcvbuf']
            ], grouped.get(gk));
            if (index + 1 < groupOrder.length) {
                out.push('      ');
            }
        }
        return true;
    }
    function emitSpotCommonTable(out, spotRows) {
        const commonRows = [];
        const localSeen = new Set();
        for (const fields of spotRows) {
            const key = ['unit_budget_bytes', 'auto_buffer_bytes',
                'manual_buffer_bytes', 'buffer_connections', 'runtime_reserve_bytes',
                'effective_message_bytes', 'enabled']
                .map((n) => fields[n] || '').join('');
            if (localSeen.has(key)) {
                continue;
            }
            localSeen.add(key);
            const enabled = fields.enabled || '';
            commonRows.push({
                unit_budget_bytes: fields.unit_budget_bytes || '',
                auto_buffer_bytes: fields.auto_buffer_bytes || '',
                manual_buffer_bytes: fields.manual_buffer_bytes || '',
                buffer_connections: fields.buffer_connections || '',
                runtime_reserve_bytes: fields.runtime_reserve_bytes || '',
                effective_message_bytes: fields.effective_message_bytes || '',
                policy: enabled === '1' ? 'auto-hwm' : 'off'
            });
        }
        if (commonRows.length === 0) {
            return false;
        }
        out.push('    Auto-HWM common:');
        emitMarkdownTable(out, '      ', [
            ['UnitBudget(B)', 'unit_budget_bytes'],
            ['AutoBuffer(B)', 'auto_buffer_bytes'],
            ['ManualBuffer(B)', 'manual_buffer_bytes'],
            ['BufferConn', 'buffer_connections'],
            ['Runtime(B)', 'runtime_reserve_bytes'],
            ['MsgUnit(B)', 'effective_message_bytes'],
            ['Policy', 'policy']
        ], commonRows);
        return true;
    }
    function emitSpotPolicyTable(out, spotRows) {
        const policyRows = [];
        const localSeen = new Set();
        for (const fields of spotRows) {
            const [sc] = spotScopeAndSocket(fields.label || '');
            const scope = fields.scope || sc;
            if (!scope) {
                continue;
            }
            let row = {
                transport: fields.transport || '',
                scope,
                scope_count: fields.scope_count || '',
                role: fields.role || '',
                unit_budget_bytes: fields.unit_budget_bytes || '',
                socket_message_slots: fields.socket_message_slots || '',
                effective_message_bytes: fields.effective_message_bytes || '',
                sndhwm: fields.sndhwm || '',
                rcvhwm: fields.rcvhwm || '',
                size_cap: fields.size_cap || '',
                managed: fields.managed_connections || '',
                active: fields.active_connections || '',
                base: fields.base_floor_per_connection || '',
                reason: fields.last_recalc_reason || ''
            };
            row = applyActiveHwmDisplay(row);
            const key = ['scope', 'scope_count', 'role', 'unit_budget_bytes',
                'socket_message_slots', 'effective_message_bytes', 'sndhwm',
                'rcvhwm', 'size_cap', 'base', 'reason']
                .map((n) => (row[n] !== undefined ? row[n] : '')).join('');
            if (localSeen.has(key)) {
                continue;
            }
            localSeen.add(key);
            policyRows.push(row);
        }
        if (policyRows.length === 0) {
            return false;
        }
        out.push('    Auto-HWM policy:');
        emitMarkdownTable(out, '      ', [
            ['Scope', 'scope'], ['ScopeCount', 'scope_count'], ['Role', 'role'],
            ['UnitBudget(B)', 'unit_budget_bytes'],
            ['MsgUnit(B)', 'effective_message_bytes'],
            ['MsgSlots', 'socket_message_slots'], ['SNDHWM', 'sndhwm'],
            ['RCVHWM', 'rcvhwm'], ['SizeCap', 'size_cap'], ['Base', 'base'],
            ['Reason', 'reason']
        ], policyRows);
        return true;
    }
    function emitSpotTables(out, tableRows) {
        const snapshotRows = tableRows.filter((f) => f.source === 'spotnode_snapshot' && f.socket);
        if (snapshotRows.length > 0) {
            const nodeRows = snapshotRows.filter((f) => f.owner === 'node');
            const spotR = snapshotRows.filter((f) => f.owner === 'spot');
            let emitted = false;
            emitted = emitSpotSnapshotSocketTable(out, 'Auto-HWM spotnode', nodeRows) || emitted;
            emitted = emitSpotSnapshotSocketTable(out, 'Auto-HWM spot handles', spotR) || emitted;
            return emitted;
        }
        const spotRows = tableRows.filter((f) => f.source !== 'option_fallback'
            && spotScopeAndSocket(f.label || '')[0]);
        if (spotRows.length === 0) {
            return false;
        }
        let emitted = false;
        emitted = emitSpotCommonTable(out, spotRows) || emitted;
        emitted = emitSpotSocketTable(out, 'Auto-HWM spotnode', spotDisplayRows(spotRows, 'shared')) || emitted;
        emitted = emitSpotSocketTable(out, 'Auto-HWM spot handles', spotDisplayRows(spotRows, 'per-spot')) || emitted;
        emitted = emitSpotPolicyTable(out, spotRows) || emitted;
        return emitted;
    }
    function expectedHwm(fields) {
        const unitBudget = parseInt10(fields.unit_budget_bytes || '', 0);
        const msgUnit = parseInt10(fields.effective_message_bytes || '', 0);
        const sizeCap = parseInt10(fields.size_cap || '', 0);
        if (unitBudget <= 0 || msgUnit <= 0) {
            return null;
        }
        let hwm = Math.trunc((unitBudget + msgUnit - 1) / msgUnit);
        if (hwm < 1) {
            hwm = 1;
        }
        if (sizeCap > 0) {
            hwm = Math.min(hwm, sizeCap);
        }
        return hwm;
    }
    function expectedMatchScore(fields) {
        const expected = expectedHwm(fields);
        if (expected === null) {
            return 2;
        }
        const sndhwm = parseInt10(fields.sndhwm || '', -1);
        const rcvhwm = parseInt10(fields.rcvhwm || '', -1);
        let matches = 0;
        let visible = 0;
        if (sndhwm >= 0) {
            visible += 1;
            if (sndhwm === expected) {
                matches += 1;
            }
        }
        if (rcvhwm >= 0) {
            visible += 1;
            if (rcvhwm === expected) {
                matches += 1;
            }
        }
        if (visible === 0) {
            return 2;
        }
        if (matches === visible) {
            return 0;
        }
        return matches > 0 ? 1 : 2;
    }
    function selectNonSpotDisplayRows(tableRows) {
        const selected = new Map();
        tableRows.forEach((fields, index) => {
            const logicalKey = ['msg_size', 'component', 'socket_type',
                'unit_budget_bytes', 'effective_message_bytes']
                .map((n) => fields[n] || '').join('');
            const score = expectedMatchScore(fields);
            const previous = selected.get(logicalKey);
            if (previous === undefined) {
                selected.set(logicalKey, [score, index, fields]);
                return;
            }
            const [prevScore, prevIndex] = previous;
            if (score < prevScore || (score === prevScore && index > prevIndex)) {
                selected.set(logicalKey, [score, index, fields]);
            }
        });
        return [...selected.values()].map((item) => item[2]);
    }
    // C emit_auto_hwm_detail_table(emit, pattern_name)
    function emitDetailTable(out, patternName) {
        const pattern = normalizeMultiPatternName(patternName);
        let patternRows = [];
        for (const fields of rows) {
            if (normalizeMultiPatternName(fields.pattern || '') !== pattern) {
                continue;
            }
            if (tableSeen.has(`${pattern}${fields._dedup_key}`)) {
                continue;
            }
            patternRows.push(fields);
        }
        if (patternRows.length === 0) {
            return false;
        }
        if (patternIsSpot(pattern)) {
            if (!emitSpotTables(out, patternRows)) {
                return false;
            }
            for (const fields of patternRows) {
                tableSeen.add(`${pattern}${fields._dedup_key}`);
            }
            return true;
        }
        patternRows = patternRows.filter((f) => f.msg_size && f.msg_size !== '0');
        if (patternRows.length === 0) {
            return false;
        }
        patternRows.sort((a, b) => (parseInt10(a.msg_size || '', 0) - parseInt10(b.msg_size || '', 0)
            || String(a.component || '').localeCompare(String(b.component || ''))
            || String(a.socket_type || '').localeCompare(String(b.socket_type || ''))));
        out.push('    Auto-HWM detail:');
        const displayRows = [];
        const seenDisplay = new Set();
        for (const fields of selectNonSpotDisplayRows(patternRows)) {
            const display = { ...fields };
            display.type = fields.socket_type || '';
            const msgSize = fields.msg_size || '';
            display.msg_size_display = (msgSize && msgSize !== '0') ? msgSize : '?';
            display.unit_budget_kb = bytesToKbDisplay(fields.unit_budget_bytes || '');
            display.effective_sndbuf_kb = bytesToKbDisplay(fields.effective_sndbuf || '');
            display.effective_rcvbuf_kb = bytesToKbDisplay(fields.effective_rcvbuf || '');
            const displayKey = ['msg_size_display', 'component', 'type',
                'unit_budget_kb', 'effective_message_bytes', 'sndhwm', 'rcvhwm',
                'effective_sndbuf_kb', 'effective_rcvbuf_kb']
                .map((n) => (display[n] !== undefined ? display[n] : '')).join('');
            if (seenDisplay.has(displayKey)) {
                continue;
            }
            seenDisplay.add(displayKey);
            displayRows.push(display);
        }
        emitMarkdownTable(out, '      ', [
            ['Size(B)', 'msg_size_display'],
            ['Component', 'component'],
            ['Type', 'type'],
            ['UnitBudget(KB)', 'unit_budget_kb'],
            ['MsgUnit(B)', 'effective_message_bytes'],
            ['SNDHWM', 'sndhwm'],
            ['RCVHWM', 'rcvhwm'],
            ['SNDBUF(KB)', 'effective_sndbuf_kb'],
            ['RCVBUF(KB)', 'effective_rcvbuf_kb']
        ], displayRows);
        for (const fields of patternRows) {
            tableSeen.add(`${pattern}${fields._dedup_key}`);
        }
        return true;
    }
    return { addLine, emitDetailTable };
}
module.exports = {
    STREAM_VARIANT_PATTERNS,
    SPOT_CONTROL_PATTERNS,
    resolveLatencyTriplet,
    singleTableHeaderLine,
    singleTableSeparatorLine,
    singleTableRowLine,
    multiTableHeaderLine,
    multiTableSeparatorLine,
    multiTableRowLine,
    isEchoPattern,
    normalizeMultiPatternName,
    buildMetaItems,
    metaLines,
    buildSingleOptionItems,
    buildMultiOptionItems,
    effectiveOptionLines,
    resultDataLines,
    multiResultDataLines,
    createAutoHwmCollector
};
