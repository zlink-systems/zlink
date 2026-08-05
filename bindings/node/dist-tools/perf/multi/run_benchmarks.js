// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const fs = require('node:fs');
const path = require('node:path');
const { defaultMultiMsgSizes, DEFAULT_MULTI_TRANSPORTS, hasPrimaryMetricsFromResultLines, medianMetrics, parseCommonArgs, primaryMetricsFromResultLines, resolveMultiPatternNames } = require('../common/perf_metrics');
const { buildMetaItems, metaLines, buildMultiOptionItems, effectiveOptionLines, multiResultDataLines, multiTableHeaderLine, multiTableSeparatorLine, multiTableRowLine, isEchoPattern, createAutoHwmCollector } = require('../common/perf_c_emitter');
const { spawnMultiPair } = require('./perf_multi_orchestrator');
const { MULTI_PATTERN_RUNNERS, POLICY_TRANSPORTS, patternMsgSizes, defaultClientsForPattern } = require('./perf_multi_policy');
const { explicitClientCount, resolvePatternClients, resolveTransportClients } = require('./perf_multi_guards');
// C parity: bindings/c/perf/run_comparison.py pattern_direction_label
// — "echo" for the echo patterns, "one-way" otherwise.
function patternDirectionLabel(patternName) {
    return isEchoPattern(patternName) ? 'echo' : 'one-way';
}
const PATTERN_SEPARATOR = '===============================================================================';
function usage() {
    console.log(`Usage: bindings/node/perf/run_benchmarks_multi.sh [options]

Measure current zlink Node multi-pattern performance.

Options:
  -h, --help            Show this help.
  --pattern NAME        Pattern list (comma-separated) or ALL.
  --build-dir PATH      Accepted for policy compatibility.
  --results-dir PATH    Override result root directory.
  --results-tag NAME    Optional tag in saved result filename.
  --output PATH         Tee final rendered output to a file.
  --runs N              Iterations per configuration (default: 1).
  --duration N          Override multi duration seconds (default: 5).
  --msg-sizes LIST      Comma-separated sizes.
  --transports LIST     Comma-separated transports (default: policy transport set).
  --clients N           Override number of client sockets per pattern (default: 100, stream=10000).
  --hwm N               Override PERF_MULTI_HWM (default baseline: auto-HWM).
  --send-hwm N          Override PERF_MULTI_SNDHWM.
  --recv-hwm N          Override PERF_MULTI_RCVHWM.
  --buf SIZE            Override both PERF_MULTI_SNDBUF and PERF_MULTI_RCVBUF.
  --sndbuf SIZE         Override PERF_MULTI_SNDBUF.
  --rcvbuf SIZE         Override PERF_MULTI_RCVBUF.
  --sndtimeo N          Override PERF_MULTI_SNDTIMEO_MS.
  --rcvtimeo N          Override PERF_MULTI_RCVTIMEO_MS.
  --send-timeout-ms N   Alias of --sndtimeo.
  --recv-timeout-ms N   Alias of --rcvtimeo.
  --connect-concurrency N
                       Override PERF_MULTI_CONNECT_CONCURRENCY.
  --transport-transition-ms N
                       Override transport cooldown.
  --pattern-transition-ms N
                       Override pattern cooldown.
  --server-ready-timeout-ms N
                       Override READY wait timeout.
  --connect-ready-timeout-ms N
                       Override connection-ready wait timeout.
  --monitor-hwm N       Override PERF_MULTI_MONITOR_HWM.
  --server-shutdown-timeout-ms N
                       Override graceful server shutdown wait.
  --server-bind-port N Override benchmark bind port (default: 0 = auto).
  --auto-hwm-profile N Set auto-HWM profile.
  --pin-cpu            Pin server/client benchmark processes to CPU 0 on Linux.`);
}
function errorText(error) {
    return String(error && error.message ? error.message : error);
}
async function spawnMeasuredPair(runner, options) {
    const lines = await spawnMultiPair(runner.server, runner.client, options);
    if (!hasPrimaryMetricsFromResultLines(options.pattern, options.msgSize, lines)) {
        return { lines, metrics: null };
    }
    const metrics = primaryMetricsFromResultLines(options.pattern, options.msgSize, lines);
    return { lines, metrics };
}
function isUnsupported(error) {
    return errorText(error).toLowerCase().includes('protocol not supported');
}
function hasUnsupportedToken(lines) {
    return lines.some((line) => line.startsWith('UNSUPPORTED,'));
}
function hasSkipToken(lines) {
    return lines.some((line) => line.startsWith('SKIP,'));
}
async function sleepMs(milliseconds) {
    await new Promise((resolve) => setTimeout(resolve, milliseconds));
}
async function main() {
    const options = parseCommonArgs(process.argv.slice(2), {
        pattern: 'ALL',
        duration: 5,
        msgSizes: defaultMultiMsgSizes(['MULTI_DEALER_DEALER'], false),
        resultsDir: path.join(process.cwd(), 'perf', 'results'),
        transports: DEFAULT_MULTI_TRANSPORTS,
        clients: defaultClientsForPattern('MULTI_DEALER_DEALER')
    });
    if (options.helpRequested) {
        usage();
        return;
    }
    const patternNames = resolveMultiPatternNames(options.pattern);
    const defaultMsgSizes = defaultMultiMsgSizes(patternNames, options.msgSizesExplicit);
    if (defaultMsgSizes !== null) {
        options.msgSizes = defaultMsgSizes;
    }
    const envClients = explicitClientCount();
    const clientSource = options.clientsExplicit
        ? 'cli'
        : (envClients !== null ? 'env' : 'policy');
    if (clientSource === 'env') {
        options.clients = envClients;
    }
    const clientsOverride = clientSource === 'policy' ? undefined : options.clients;
    const failFast = process.env.PERF_FAIL_FAST === '1';
    const runCooldownMs = Number(process.env.PERF_MULTI_RUN_COOLDOWN_MS ?? process.env.PERF_RUN_COOLDOWN_MS ?? 3000);
    const transportCooldownMs = Number.isFinite(options.transportTransitionMs)
        ? options.transportTransitionMs
        : Number(process.env.PERF_MULTI_TRANSPORT_TRANSITION_MS ?? process.env.PERF_TRANSPORT_TRANSITION_MS ?? 3000);
    const patternCooldownMs = Number.isFinite(options.patternTransitionMs)
        ? options.patternTransitionMs
        : Number(process.env.PERF_MULTI_PATTERN_TRANSITION_MS ?? process.env.PERF_PATTERN_TRANSITION_MS ?? 3000);
    // C parity: bindings/c/perf/run_comparison.py main() — single TeeStream
    // so the saved report and stdout receive the EXACT same byte stream.
    // Buffer every emitted line and write the file at the end so node's
    // multi report is byte-identical to C's.
    const out = [];
    const emit = (line = '') => {
        console.log(line);
        out.push(line);
    };
    // Python `print("\nX")` = blank line then X.
    const emitSection = (line) => {
        emit('');
        emit(line);
    };
    // C: status_counts + current_results + all_failures + all_skips.
    const statusCounts = { success: 0, unsupported: 0, skip: 0, fail: 0 };
    const successRecords = [];
    const allFailures = [];
    const allSkips = [];
    let expectedResultLines = 0;
    let actualResultLines = 0;
    const autoHwm = createAutoHwmCollector();
    const runnablePatterns = [];
    for (const patternName of patternNames) {
        const runner = MULTI_PATTERN_RUNNERS[patternName];
        if (!runner) {
            throw new Error(`unsupported multi pattern: ${patternName}`);
        }
        const resolution = resolvePatternClients(patternName, options, clientSource);
        if (resolution.skipReason) {
            allSkips.push([patternName, resolution.skipReason]);
            statusCounts.skip += 1;
            continue;
        }
        runnablePatterns.push({
            patternName,
            runner,
            clients: resolution.clients
        });
    }
    // C parity: build_meta_items + print_meta_lines, then a blank line
    // (print_effective_options does print("\n## Effective Options")), then
    // the effective options block. The report file's first lines are the
    // META preamble (multi suite only).
    const metaItems = buildMetaItems('node', options.runs, patternNames, process.cwd(), clientsOverride);
    for (const line of metaLines(metaItems)) {
        emit(line);
    }
    if (process.env.ZLINK_PERF_RUNTIME_LIBZLINK) {
        emit(`META,runtime_libzlink,${process.env.ZLINK_PERF_RUNTIME_LIBZLINK}`);
    }
    const optionItems = buildMultiOptionItems({
        runs: options.runs,
        duration: options.duration,
        patterns: patternNames,
        transports: options.transports,
        msgSizes: options.msgSizes,
        clientsOverride,
        ioThreads: options.ioThreads,
        serverIoThreads: options.serverIoThreads,
        clientIoThreads: options.clientIoThreads,
        sendTimeoutMs: options.sendTimeoutMs,
        recvTimeoutMs: options.recvTimeoutMs,
        connectConcurrency: options.connectConcurrency,
        connectReadyTimeoutMs: options.connectReadyTimeoutMs,
        monitorHwm: options.monitorHwm,
        serverReadyTimeoutMs: options.serverReadyTimeoutMs,
        serverShutdownTimeoutMs: options.serverShutdownTimeoutMs,
        serverBindPort: options.serverBindPort,
        transportTransitionMs: options.transportTransitionMs,
        patternTransitionMs: options.patternTransitionMs
    });
    emit('');
    for (const line of effectiveOptionLines('node', 'multi', 'start', optionItems)) {
        emit(line);
    }
    let tablesEmitted = false;
    for (let patternIndex = 0; patternIndex < runnablePatterns.length; patternIndex += 1) {
        const { patternName, runner, clients } = runnablePatterns[patternIndex];
        if (tablesEmitted) {
            emit('');
            emit(PATTERN_SEPARATOR);
            emit('');
        }
        emit(`## PATTERN: ${patternName} (${patternDirectionLabel(patternName)})`);
        tablesEmitted = true;
        const transports = options.transports.filter((transport) => POLICY_TRANSPORTS[patternName].includes(transport));
        if (transports.length === 0) {
            emit(`  Skipping ${patternName}: no matching transports.`);
            allSkips.push([patternName, 'no_matching_transports']);
            statusCounts.skip += 1;
            continue;
        }
        emit(`  > Benchmarking current for ${patternName}...`);
        const msgSizes = patternMsgSizes(patternName, options.msgSizes);
        const showRunLabels = options.runs > 1;
        for (let transportIdx = 0; transportIdx < transports.length; transportIdx += 1) {
            const transport = transports[transportIdx];
            const hasNextTransport = (transportIdx + 1) < transports.length;
            emit(`    Testing ${transport}:`);
            if (!showRunLabels) {
                emit(`      ${multiTableHeaderLine()}`);
                emit(`      ${multiTableSeparatorLine()}`);
            }
            const samples = new Map(msgSizes.map((size) => [size, []]));
            const failedSizes = new Map();
            const sectionEmitted = new Set();
            let transportUnsupported = false;
            const emitSizeSection = (size, indent) => {
                if (sectionEmitted.has(size)) {
                    return;
                }
                emit(`    Testing ${transport} | ${size}B:`);
                sectionEmitted.add(size);
                if (showRunLabels) {
                    emit(`${indent}${multiTableHeaderLine()}`);
                    emit(`${indent}${multiTableSeparatorLine()}`);
                }
            };
            for (let run = 0; run < options.runs && !transportUnsupported; run += 1) {
                let rowIndent = '      ';
                if (showRunLabels) {
                    emit(`      run ${run + 1}/${options.runs}:`);
                    rowIndent = '        ';
                }
                for (const msgSize of msgSizes) {
                    if (transportUnsupported) {
                        break;
                    }
                    const caseOptions = {
                        ...options,
                        pattern: patternName,
                        transport,
                        msgSize,
                        clients: resolveTransportClients(patternName, transport, clients)
                    };
                    try {
                        const { lines, metrics: activeMetrics } = await spawnMeasuredPair(runner, caseOptions);
                        for (const line of lines) {
                            autoHwm.addLine(line);
                        }
                        const hasMetrics = hasPrimaryMetricsFromResultLines(patternName, msgSize, lines);
                        if (!hasMetrics && hasUnsupportedToken(lines)) {
                            transportUnsupported = true;
                            break;
                        }
                        if (!hasMetrics && hasSkipToken(lines)) {
                            failedSizes.set(msgSize, 'skip');
                            allSkips.push([
                                `${patternName} ${transport} ${msgSize}B`, 'skip'
                            ]);
                            emitSizeSection(msgSize, rowIndent);
                            emit(`${rowIndent}${multiTableRowLine(patternName, msgSize, 'fail', null)}`);
                            continue;
                        }
                        const metrics = activeMetrics;
                        samples.get(msgSize).push(metrics);
                        emitSizeSection(msgSize, rowIndent);
                        emit(`${rowIndent}${multiTableRowLine(patternName, msgSize, 'success', {
                            throughput: metrics.throughput,
                            bandwidth: metrics.bandwidth,
                            latency: metrics.latency,
                            latency_p95: metrics.latency_p95,
                            latency_p99: metrics.latency_p99
                        })}`);
                    }
                    catch (error) {
                        if (isUnsupported(error)) {
                            transportUnsupported = true;
                            break;
                        }
                        failedSizes.set(msgSize, errorText(error));
                        allFailures.push([
                            patternName, 'current', transport, msgSize, errorText(error)
                        ]);
                        emitSizeSection(msgSize, rowIndent);
                        emit(`${rowIndent}${multiTableRowLine(patternName, msgSize, 'fail', null)}`);
                        if (failFast) {
                            break;
                        }
                    }
                }
                if (showRunLabels && run + 1 < options.runs && !transportUnsupported
                    && !(failFast && allFailures.length > 0)) {
                    emit(`      [cooldown ${runCooldownMs}ms]`);
                    if (runCooldownMs > 0) {
                        await sleepMs(runCooldownMs);
                    }
                }
            }
            if (showRunLabels && !transportUnsupported) {
                emit('      median:');
                emit(`        ${multiTableHeaderLine()}`);
                emit(`        ${multiTableSeparatorLine()}`);
                for (const msgSize of msgSizes) {
                    const list = samples.get(msgSize);
                    if (failedSizes.has(msgSize) || !list || list.length !== options.runs) {
                        emit(`        ${multiTableRowLine(patternName, msgSize, 'fail', null)}`);
                        continue;
                    }
                    const metrics = medianMetrics(list);
                    samples.set(msgSize, [metrics]);
                    emit(`        ${multiTableRowLine(patternName, msgSize, 'success', {
                        throughput: metrics.throughput,
                        bandwidth: metrics.bandwidth,
                        latency: metrics.latency,
                        latency_p95: metrics.latency_p95,
                        latency_p99: metrics.latency_p99
                    })}`);
                }
            }
            // Classify per (transport, size): C main loop status counting.
            for (const msgSize of msgSizes) {
                if (transportUnsupported) {
                    statusCounts.unsupported += 1;
                    continue;
                }
                const skipped = allSkips.some(([label]) => label === `${patternName} ${transport} ${msgSize}B`);
                if (skipped) {
                    statusCounts.skip += 1;
                    continue;
                }
                const list = samples.get(msgSize);
                if (failedSizes.has(msgSize) || !list || list.length === 0) {
                    statusCounts.fail += 1;
                    expectedResultLines += 5;
                    continue;
                }
                const metrics = options.runs > 1 ? list[0] : medianMetrics(list);
                const [, p95, p99] = require('../common/perf_c_emitter')
                    .resolveLatencyTriplet(metrics.latency, metrics.latency_p95, metrics.latency_p99);
                successRecords.push({
                    pattern: patternName,
                    transport,
                    size: msgSize,
                    throughput: metrics.throughput,
                    bandwidth: metrics.bandwidth,
                    latency: metrics.latency,
                    latency_p95: p95,
                    latency_p99: p99
                });
                statusCounts.success += 1;
                expectedResultLines += 5;
                actualResultLines += 5;
            }
            emit(`    Testing ${transport}: Done`);
            if (transportCooldownMs > 0 && hasNextTransport
                && !(failFast && allFailures.length > 0)) {
                emit(`    [transport cooldown ${transportCooldownMs}ms]`);
                await sleepMs(transportCooldownMs);
            }
            if (failFast && allFailures.length > 0) {
                break;
            }
        }
        // C parity: emit_auto_hwm_detail_table(emit, pattern) after collect.
        {
            const tableLines = [];
            autoHwm.emitDetailTable(tableLines, patternName);
            for (const line of tableLines) {
                emit(line);
            }
        }
        if (patternIndex + 1 < runnablePatterns.length && patternCooldownMs > 0
            && !(failFast && allFailures.length > 0)) {
            emit(`[pattern cooldown ${patternCooldownMs}ms]`);
            await sleepMs(patternCooldownMs);
        }
        if (failFast && allFailures.length > 0) {
            break;
        }
    }
    // C parity: print_effective_options("result"); Result Data; Completion;
    // Skips; Failures; Saved result file.
    emit('');
    for (const line of effectiveOptionLines('node', 'multi', 'result', optionItems)) {
        emit(line);
    }
    if (successRecords.length > 0) {
        emitSection('## Result Data');
        for (const line of multiResultDataLines(successRecords)) {
            emit(line);
        }
    }
    const status = expectedResultLines === actualResultLines ? 'complete' : 'partial';
    emitSection('## Completion');
    emit(`- success: ${statusCounts.success}`);
    emit(`- unsupported: ${statusCounts.unsupported}`);
    emit(`- skip: ${statusCounts.skip}`);
    emit(`- fail: ${statusCounts.fail}`);
    emit(`- status: ${status}`);
    emit(`- expected_result_lines: ${expectedResultLines}`);
    emit(`- actual_result_lines: ${actualResultLines}`);
    if (allSkips.length > 0) {
        emitSection('## Skips');
        for (const [label, reason] of allSkips) {
            emit(`- ${label}: ${reason}`);
        }
    }
    if (allFailures.length > 0) {
        emitSection('## Failures');
        // C parity: sorted(set(all_failures), key=(pattern, size, tr, reason)).
        const seenFailures = new Set();
        const unique = [];
        for (const f of allFailures) {
            const dedup = f.join(' ');
            if (seenFailures.has(dedup)) {
                continue;
            }
            seenFailures.add(dedup);
            unique.push(f);
        }
        unique.sort((a, b) => (String(a[0]).localeCompare(String(b[0]))
            || Number(a[3]) - Number(b[3])
            || String(a[2]).localeCompare(String(b[2]))
            || String(a[4]).localeCompare(String(b[4]))));
        for (const [pattern, libName, tr, sz, reason] of unique) {
            emit(`- ${pattern} ${libName} ${tr} ${sz}B: ${reason}`);
        }
    }
    const reportDir = path.join(options.resultsDir, 'multi', 'report');
    fs.mkdirSync(reportDir, { recursive: true });
    const stamp = formatStamp(new Date());
    const tag = options.resultsTag ? `_${sanitizeTag(options.resultsTag)}` : '';
    const resultFile = path.join(reportDir, `perf_node_multi_${platformTag()}_${stamp}${tag}.txt`);
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
function exitAfterFlush(code) {
    process.stdout.write('', () => {
        process.stderr.write('', () => {
            process.exit(code);
        });
    });
}
main()
    .then(() => {
    exitAfterFlush(process.exitCode || 0);
})
    .catch((error) => {
    console.error(error);
    exitAfterFlush(1);
});
