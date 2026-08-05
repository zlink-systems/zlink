// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');
const { defaultSingleMsgSizes, DEFAULT_SINGLE_TRANSPORTS, hasPrimaryMetricsFromResultLines, medianMetrics, parseCommonArgs, primaryMetricsFromResultLines, resolveSinglePatternNames } = require('../common/perf_metrics');
const { buildSingleOptionItems, effectiveOptionLines, resultDataLines, singleTableHeaderLine, singleTableSeparatorLine, singleTableRowLine } = require('../common/perf_c_emitter');
const { autoHwmDetailTableLines, parseAutoHwmDetailLine } = require('./perf_single_auto_hwm');
const PATTERNS = {
    PAIR: { script: 'perf_pair.js' },
    PUBSUB: { script: 'perf_pubsub.js' },
    DEALER_DEALER: { script: 'perf_dealer_dealer.js' },
    DEALER_ROUTER: { script: 'perf_dealer_router.js' },
    DEALER_ROUTER_REQREP: { script: 'perf_dealer_router_reqrep.js' },
    ROUTER_ROUTER: { script: 'perf_router_router.js' },
    ROUTER_ROUTER_REQREP: { script: 'perf_router_router_reqrep.js' }
};
function policyTransports(_pattern) {
    const raw = ['tcp', 'tls', 'ws', 'wss', 'inproc', 'ipc'];
    if (process.platform === 'win32') {
        return raw.filter((transport) => transport !== 'ipc');
    }
    return raw;
}
function usage() {
    console.log(`Usage: bindings/node/perf/run_benchmarks.sh [options]

Measure current zlink Node single-pattern performance.

Options:
  -h, --help            Show this help.
  --pattern NAME        Pattern list (comma-separated) or ALL.
  --build-dir PATH      Accepted for policy compatibility.
  --results-dir PATH    Override result root directory.
  --results-tag NAME    Optional tag in saved result filename.
  --output PATH         Tee final rendered output to a file.
  --runs N              Iterations per configuration (default: 1).
  --duration N          Override single duration seconds (default: 5).
  --msg-sizes LIST      Comma-separated sizes (default: 64,256,1024,65536,131072,262144).
  --transports LIST     Comma-separated transports (default: policy transport set).
  --pin-cpu             Pin child benchmark processes to CPU 0 on Linux.
  --io-threads N        Override PERF_IO_THREADS for child cases.
  --hwm N               Override PERF_SINGLE_HWM.
  --send-hwm N          Override PERF_SINGLE_SNDHWM.
  --recv-hwm N          Override PERF_SINGLE_RCVHWM.
  --buf SIZE            Override both PERF_SINGLE_SNDBUF and PERF_SINGLE_RCVBUF.
  --sndbuf SIZE         Override PERF_SINGLE_SNDBUF.
  --rcvbuf SIZE         Override PERF_SINGLE_RCVBUF.
  --sndtimeo N          Override PERF_SINGLE_SNDTIMEO_MS.
  --rcvtimeo N          Override PERF_SINGLE_RCVTIMEO_MS.
  --send-timeout-ms N   Alias of --sndtimeo.
  --recv-timeout-ms N   Alias of --rcvtimeo.
  --auto-hwm-profile N  Set auto-HWM profile.

Notes:
  - result is saved under perf/results/single/report/ as
    perf_node_single_<platform>_YYYYMMDD_HHMMSS[_<tag>].txt.`);
}
function pinCpuEnabled(options) {
    return Boolean(options.pinCpu || process.env.PERF_TASKSET === '1');
}
function buildPinnedSpawn(command, args, options) {
    if (!pinCpuEnabled(options)) {
        return { command, args };
    }
    if (process.platform !== 'linux') {
        throw new Error('--pin-cpu is only supported on Linux in this runner');
    }
    return {
        command: 'taskset',
        args: ['-c', '0', command, ...args]
    };
}
function isPlatformSkip(_pattern, transport) {
    return process.platform === 'win32' && transport === 'ipc';
}
function isUnsupportedLines(lines) {
    return lines.some((line) => line.startsWith('UNSUPPORTED,'));
}
function errorText(error) {
    return String(error && error.message ? error.message : error);
}
function buildBinaryEnv(options) {
    const env = {
        ...process.env,
        PERF_SINGLE_DURATION_SECONDS: String(options.duration)
    };
    if (Number.isFinite(options.hwm)) {
        env.PERF_SINGLE_HWM = String(options.hwm);
    }
    if (Number.isFinite(options.sendHwm)) {
        env.PERF_SINGLE_SNDHWM = String(options.sendHwm);
    }
    if (Number.isFinite(options.recvHwm)) {
        env.PERF_SINGLE_RCVHWM = String(options.recvHwm);
    }
    if (options.sndbuf) {
        env.PERF_SINGLE_SNDBUF = String(options.sndbuf);
    }
    if (options.rcvbuf) {
        env.PERF_SINGLE_RCVBUF = String(options.rcvbuf);
    }
    if (options.autoHwmProfile) {
        env.PERF_CTX_AUTO_HWM_PROFILE = String(options.autoHwmProfile);
    }
    if (Number.isFinite(options.sendTimeoutMs)) {
        env.PERF_SINGLE_SNDTIMEO_MS = String(options.sendTimeoutMs);
    }
    if (Number.isFinite(options.recvTimeoutMs)) {
        env.PERF_SINGLE_RCVTIMEO_MS = String(options.recvTimeoutMs);
    }
    if (Number.isFinite(options.ioThreads)) {
        env.PERF_IO_THREADS = String(options.ioThreads);
    }
    return env;
}
function resolveSingleTimeoutSeconds(durationSeconds) {
    const override = Number(process.env.PERF_SINGLE_TIMEOUT_SECONDS || 0);
    if (Number.isFinite(override) && override > 0) {
        return Math.trunc(override);
    }
    return Math.max(30, Math.ceil(Number(durationSeconds) * 6) + 15);
}
async function runSingleCase(scriptName, transport, msgSize, options) {
    const scriptPath = path.join(__dirname, scriptName);
    const spawnSpec = buildPinnedSpawn(process.execPath, [scriptPath, 'current', transport, String(msgSize)], options);
    const child = spawn(spawnSpec.command, spawnSpec.args, {
        cwd: process.cwd(),
        env: buildBinaryEnv(options),
        stdio: ['ignore', 'pipe', 'pipe']
    });
    const stdoutLines = [];
    const stderrLines = [];
    let stdoutBuffer = '';
    let stderrBuffer = '';
    let timedOut = false;
    const timeoutSeconds = Number.isFinite(options.timeoutSeconds)
        ? options.timeoutSeconds
        : resolveSingleTimeoutSeconds(options.duration);
    const timeoutMs = timeoutSeconds * 1000;
    const timeout = setTimeout(() => {
        timedOut = true;
        child.kill('SIGTERM');
        setTimeout(() => child.kill('SIGKILL'), 1000).unref();
    }, timeoutMs);
    child.stdout.setEncoding('utf8');
    child.stdout.on('data', (chunk) => {
        stdoutBuffer += chunk;
        while (true) {
            const newline = stdoutBuffer.indexOf('\n');
            if (newline === -1) {
                break;
            }
            const line = stdoutBuffer.slice(0, newline).trim();
            stdoutBuffer = stdoutBuffer.slice(newline + 1);
            if (line) {
                stdoutLines.push(line);
            }
        }
    });
    child.stderr.setEncoding('utf8');
    child.stderr.on('data', (chunk) => {
        stderrBuffer += chunk;
        while (true) {
            const newline = stderrBuffer.indexOf('\n');
            if (newline === -1) {
                break;
            }
            const line = stderrBuffer.slice(0, newline).trim();
            stderrBuffer = stderrBuffer.slice(newline + 1);
            if (line) {
                stderrLines.push(line);
            }
        }
    });
    const exitCode = await new Promise((resolve, reject) => {
        child.once('error', reject);
        child.once('exit', (code) => resolve(code ?? 1));
    });
    clearTimeout(timeout);
    if (stdoutBuffer.trim()) {
        stdoutLines.push(stdoutBuffer.trim());
    }
    if (stderrBuffer.trim()) {
        stderrLines.push(stderrBuffer.trim());
    }
    if (timedOut) {
        const stderrText = stderrLines.join('\n');
        throw new Error(stderrText
            ? `single case timeout after ${timeoutMs}ms\n${stderrText}`
            : `single case timeout after ${timeoutMs}ms`);
    }
    if (exitCode !== 0) {
        const stderrText = stderrLines.join('\n');
        throw new Error(stderrText ? `single case failed: ${stderrText}` : `single case failed: ${exitCode}`);
    }
    return stdoutLines;
}
async function main() {
    const options = parseCommonArgs(process.argv.slice(2), {
        pattern: 'ALL',
        duration: 5,
        msgSizes: defaultSingleMsgSizes(),
        resultsDir: path.join(process.cwd(), 'perf', 'results'),
        transports: DEFAULT_SINGLE_TRANSPORTS
    });
    if (options.helpRequested) {
        usage();
        return;
    }
    const names = resolveSinglePatternNames(options.pattern);
    const failFast = process.env.PERF_FAIL_FAST === '1';
    const autoHwmDetails = [];
    // C single TeeStream: the saved report file and stdout receive the
    // EXACT same byte stream. We buffer every emitted line and write the
    // file at the end so node's report is byte-identical to C's.
    const out = [];
    const emit = (line = '') => {
        console.log(line);
        out.push(line);
    };
    // Python `print("\nX")` = blank line then X. emitSection mirrors that.
    const emitSection = (line) => {
        emit('');
        emit(line);
    };
    // C single timeout policy for raw socket patterns.
    const durationSeconds = Number(process.env.PERF_SINGLE_DURATION_SECONDS || options.duration);
    let timeoutSeconds = Math.max(30, durationSeconds * 6 + 15);
    const timeoutOverride = Number(process.env.PERF_SINGLE_TIMEOUT_SECONDS || 0);
    if (Number.isFinite(timeoutOverride) && timeoutOverride > 0) {
        timeoutSeconds = Math.trunc(timeoutOverride);
    }
    options.timeoutSeconds = timeoutSeconds;
    const optionItems = buildSingleOptionItems({
        runs: options.runs,
        duration: options.duration,
        timeoutSeconds,
        patterns: names,
        transports: options.transports,
        msgSizes: options.msgSizes
    });
    // C: print_effective_options does print("\n## Effective Options ...").
    // The report file's first line is therefore an empty line.
    emit('');
    if (process.env.ZLINK_PERF_RUNTIME_LIBZLINK) {
        emit(`META,runtime_libzlink,${process.env.ZLINK_PERF_RUNTIME_LIBZLINK}`);
        emit('');
    }
    for (const line of effectiveOptionLines('node', 'single', 'start', optionItems)) {
        emit(line);
    }
    // combo_results keyed (pattern, transport, size) -> ComboRecord.
    const comboResults = new Map();
    const comboKey = (pattern, transport, size) => `${pattern}|${transport}|${size}`;
    const allFailures = [];
    let requestedComboCount = 0;
    const transportTransitionMs = Math.max(0, Number(process.env.PERF_TRANSPORT_TRANSITION_MS || 3000));
    let tablesEmitted = false;
    for (const name of names) {
        const runner = PATTERNS[name];
        if (!runner) {
            throw new Error(`unsupported single pattern: ${name}`);
        }
        const transports = options.transports.filter((transport) => policyTransports(name).includes(transport));
        const sizes = options.msgSizes;
        requestedComboCount += transports.length * sizes.length;
        if (tablesEmitted) {
            emit('');
            emit('===============================================================================');
            emit('');
        }
        emit(`## PATTERN: ${name} (${name.endsWith('_REQREP') ? 'request/reply' : 'one-way'})`);
        tablesEmitted = true;
        if (transports.length === 0) {
            emit(`  Skipping ${name}: no matching transports.`);
            continue;
        }
        if (sizes.length === 0) {
            emit(`  Skipping ${name}: no message sizes configured.`);
            continue;
        }
        emit(`  > Benchmarking current for ${name}...`);
        for (let transportIdx = 0; transportIdx < transports.length; transportIdx += 1) {
            const transport = transports[transportIdx];
            const hasNextTransport = (transportIdx + 1) < transports.length;
            emit(`    Testing ${transport}:`);
            const showRunLabels = options.runs > 1;
            if (!showRunLabels) {
                emit(`      ${singleTableHeaderLine()}`);
                emit(`      ${singleTableSeparatorLine()}`);
            }
            const samples = new Map(sizes.map((size) => [size, []]));
            const failedSizes = new Map();
            const failedRecords = new Map();
            let transportUnsupported = false;
            for (let run = 0; run < options.runs && !transportUnsupported; run += 1) {
                let rowIndent = '      ';
                if (showRunLabels) {
                    emit(`      run ${run + 1}/${options.runs}:`);
                    emit(`        ${singleTableHeaderLine()}`);
                    emit(`        ${singleTableSeparatorLine()}`);
                    rowIndent = '        ';
                }
                let aborted = false;
                for (let sizeIdx = 0; sizeIdx < sizes.length; sizeIdx += 1) {
                    const msgSize = sizes[sizeIdx];
                    let lines;
                    try {
                        lines = await runSingleCase(runner.script, transport, msgSize, options);
                    }
                    catch (error) {
                        const reason = String(error && error.message ? error.message : error)
                            .toLowerCase().includes('timeout') ? 'timeout' : 'fail';
                        failedSizes.set(msgSize, reason);
                        allFailures.push([name, transport, msgSize, reason]);
                        const rec = { status: 'fail' };
                        failedRecords.set(msgSize, rec);
                        emit(`${rowIndent}${singleTableRowLine(msgSize, 'fail', rec, name)}`);
                        if (failFast) {
                            aborted = true;
                            break;
                        }
                        continue;
                    }
                    for (const line of lines) {
                        const detail = parseAutoHwmDetailLine(line);
                        if (detail) {
                            autoHwmDetails.push(detail);
                        }
                    }
                    if (!hasPrimaryMetricsFromResultLines(name, msgSize, lines)
                        && isUnsupportedLines(lines)) {
                        transportUnsupported = true;
                        for (const remain of sizes.slice(sizeIdx)) {
                            emit(`${rowIndent}${singleTableRowLine(remain, 'unsupported', null, name)}`);
                        }
                        break;
                    }
                    const metrics = primaryMetricsFromResultLines(name, msgSize, lines);
                    samples.get(msgSize).push(metrics);
                    emit(`${rowIndent}${singleTableRowLine(msgSize, 'success', {
                        throughput: metrics.throughput,
                        bandwidth: metrics.bandwidth,
                        latency: metrics.latency,
                        latency_p95: metrics.latency_p95,
                        latency_p99: metrics.latency_p99
                    }, name)}`);
                }
                if (aborted) {
                    break;
                }
            }
            if (transportUnsupported) {
                for (const size of sizes) {
                    comboResults.set(comboKey(name, transport, size), { status: 'unsupported' });
                }
            }
            else {
                for (const size of sizes) {
                    const list = samples.get(size);
                    if (failedSizes.has(size) || !list || list.length === 0) {
                        comboResults.set(comboKey(name, transport, size), failedRecords.get(size) || { status: 'fail' });
                        if (!failedSizes.has(size)) {
                            allFailures.push([name, transport, size, 'no_data']);
                        }
                        continue;
                    }
                    const merged = medianMetrics(list);
                    comboResults.set(comboKey(name, transport, size), {
                        status: 'success',
                        throughput: merged.throughput,
                        bandwidth: merged.bandwidth,
                        latency: merged.latency,
                        latency_p95: merged.latency_p95,
                        latency_p99: merged.latency_p99
                    });
                }
            }
            if (showRunLabels) {
                emit('      median:');
                emit(`        ${singleTableHeaderLine()}`);
                emit(`        ${singleTableSeparatorLine()}`);
                for (const size of sizes) {
                    const record = comboResults.get(comboKey(name, transport, size));
                    if (record && record.status === 'success') {
                        emit(`        ${singleTableRowLine(size, 'success', record, name)}`);
                    }
                    else if (record && record.status === 'unsupported') {
                        emit(`        ${singleTableRowLine(size, 'unsupported', null, name)}`);
                    }
                    else {
                        emit(`        ${singleTableRowLine(size, 'fail', record, name)}`);
                    }
                }
            }
            emit(`    Testing ${transport}: Done`);
            if (transportTransitionMs > 0 && hasNextTransport) {
                let cooldownMs = transportTransitionMs;
                const nextTransport = transports[transportIdx + 1];
                if (nextTransport === 'inproc' && (transport === 'ws' || transport === 'wss')) {
                    cooldownMs = Math.max(cooldownMs, 30000);
                }
                emit(`    [transport cooldown ${cooldownMs}ms]`);
                await new Promise((resolve) => setTimeout(resolve, cooldownMs));
            }
            if (failFast && allFailures.length > 0) {
                break;
            }
        }
        if (failFast && allFailures.length > 0) {
            break;
        }
    }
    if (allFailures.length > 0) {
        emitSection('## Failures');
        for (const [pattern, transport, size, reason] of allFailures) {
            emit(`- ${pattern} current ${transport} ${size}B: ${reason}`);
        }
    }
    for (const name of names) {
        for (const line of autoHwmDetailTableLines(autoHwmDetails, name)) {
            emit(line);
        }
    }
    let unsupportedComboCount = 0;
    let skipComboCount = 0;
    let actualResultLines = 0;
    const successRecords = [];
    for (const [key, record] of comboResults) {
        if (record.status === 'unsupported') {
            unsupportedComboCount += 1;
        }
        else if (record.status === 'skip') {
            skipComboCount += 1;
        }
        else if (record.status === 'success') {
            actualResultLines += 5;
            const [pattern, transport, size] = key.split('|');
            successRecords.push({
                pattern,
                transport,
                size: Number(size),
                throughput: record.throughput,
                bandwidth: record.bandwidth,
                latency: record.latency,
                latency_p95: record.latency_p95,
                latency_p99: record.latency_p99
            });
        }
    }
    const expectedResultLines = Math.max(0, (requestedComboCount - unsupportedComboCount - skipComboCount) * 5);
    const status = expectedResultLines === actualResultLines ? 'complete' : 'partial';
    emit('');
    for (const line of effectiveOptionLines('node', 'single', 'result', optionItems)) {
        emit(line);
    }
    if (successRecords.length > 0) {
        emitSection('## Result Data');
        for (const line of resultDataLines(successRecords)) {
            emit(line);
        }
    }
    emitSection('## Completion');
    emit(`- status: ${status}`);
    emit(`- expected_result_lines: ${expectedResultLines}`);
    emit(`- actual_result_lines: ${actualResultLines}`);
    const reportDir = path.join(options.resultsDir, 'single', 'report');
    fs.mkdirSync(reportDir, { recursive: true });
    const stamp = formatStamp(new Date());
    const tag = options.resultsTag ? `_${sanitizeTag(options.resultsTag)}` : '';
    const resultFile = path.join(reportDir, `perf_node_single_${platformTag()}_${stamp}${tag}.txt`);
    emit('');
    emit(`Saved result file: ${resultFile} (status=${status})`);
    fs.writeFileSync(resultFile, `${out.join('\n')}\n`, 'utf8');
    if (options.output) {
        fs.writeFileSync(options.output, `${out.join('\n')}\n`, 'utf8');
    }
    if (status !== 'complete') {
        process.exitCode = 1;
    }
}
function formatStamp(date) {
    const pad = (value) => String(value).padStart(2, '0');
    return (`${date.getFullYear()}${pad(date.getMonth() + 1)}${pad(date.getDate())}`
        + `_${pad(date.getHours())}${pad(date.getMinutes())}${pad(date.getSeconds())}`);
}
function sanitizeTag(value) {
    return String(value).trim().replace(/[^a-zA-Z0-9._-]+/g, '_');
}
function platformTag() {
    if (process.platform === 'win32') {
        return 'windows';
    }
    if (process.platform === 'darwin') {
        return 'macos';
    }
    return 'linux';
}
main().catch((error) => {
    console.error(error);
    process.exitCode = 1;
});
