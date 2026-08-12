// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const { once } = require('node:events');
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');
const { benchmarkEndpoint, reservePort } = require('./perf_multi_common');
const processCaptureStates = new WeakMap();
function processCaptureState(processRef) {
    const state = processCaptureStates.get(processRef);
    if (!state) {
        throw new Error('process output is not managed by perf_multi_orchestrator');
    }
    return state;
}
function collectLines(stream, onLine) {
    let buffered = '';
    stream.setEncoding('utf8');
    stream.on('data', (chunk) => {
        buffered += chunk;
        while (true) {
            const newline = buffered.indexOf('\n');
            if (newline === -1) {
                break;
            }
            const line = buffered.slice(0, newline).trim();
            buffered = buffered.slice(newline + 1);
            if (line) {
                onLine(line);
            }
        }
    });
}
async function waitForLine(processRef, expected, label, timeoutMs) {
    return new Promise((resolve, reject) => {
        let done = false;
        const state = processCaptureState(processRef);
        if (state.seenLines.includes(expected)) {
            resolve();
            return;
        }
        const timeout = setTimeout(() => {
            if (!done) {
                done = true;
                reject(new Error(`${label} timeout waiting for ${expected}`));
            }
        }, timeoutMs);
        processRef.once('exit', (code) => {
            if (!done) {
                done = true;
                clearTimeout(timeout);
                reject(new Error(`${label} exited before ${expected}: ${code}`));
            }
        });
        state.waiters.push((line) => {
            if (!done && line === expected) {
                done = true;
                clearTimeout(timeout);
                resolve();
                return true;
            }
            return false;
        });
    });
}
async function waitForPrefix(processRef, prefix, label, timeoutMs) {
    return new Promise((resolve, reject) => {
        let done = false;
        const state = processCaptureState(processRef);
        const seen = state.seenLines.find((line) => line.startsWith(prefix));
        if (seen) {
            resolve(seen);
            return;
        }
        const timeout = setTimeout(() => {
            if (!done) {
                done = true;
                reject(new Error(`${label} timeout waiting for ${prefix}`));
            }
        }, timeoutMs);
        processRef.once('exit', (code) => {
            if (!done) {
                done = true;
                clearTimeout(timeout);
                reject(new Error(`${label} exited before ${prefix}: ${code}`));
            }
        });
        state.waiters.push((line) => {
            if (!done && line.startsWith(prefix)) {
                done = true;
                clearTimeout(timeout);
                resolve(line);
                return true;
            }
            return false;
        });
    });
}
function isControlLine(line) {
    return line.startsWith('READY,')
        || line.startsWith('CONTROL_READY,')
        || line.startsWith('CLIENT_READY,')
        || line.startsWith('CLIENT_DONE,')
        || line.startsWith('CLIENT_CONTROL_ENDPOINT,')
        || line.startsWith('CONTROL_CONNECTED,')
        || line.startsWith('DATA_ENDPOINT,');
}
function attachProcessCapture(child, resultLines, resultPrefix = 'RESULT,') {
    processCaptureStates.set(child, { seenLines: [], stderrLines: [], waiters: [] });
    collectLines(child.stdout, (line) => {
        const state = processCaptureState(child);
        state.seenLines.push(line);
        for (const waiter of state.waiters) {
            if (waiter(line)) {
                return;
            }
        }
        if (line.startsWith(resultPrefix)
            || line.startsWith('UNSUPPORTED,')
            || line.startsWith('SKIP,')
            || line.startsWith('AUTO_HWM_DETAIL,')) {
            resultLines.push(line);
            return;
        }
        if (!isControlLine(line)) {
            console.log(line);
        }
    });
    collectLines(child.stderr, (line) => {
        processCaptureState(child).stderrLines.push(line);
        console.error(line);
    });
}
function hasProtocolUnsupported(processRef) {
    return processCaptureState(processRef).stderrLines
        .some((line) => line.toLowerCase().includes('protocol not supported'));
}
function hasAddressInUse(processRef) {
    return processCaptureState(processRef).stderrLines
        .some((line) => /address already in use/i.test(line));
}
function stderrText(processRef) {
    return processCaptureState(processRef).stderrLines.join('\n');
}
function startupBindRetryCount(args) {
    const value = Number(args.__startupBindRetry || 0);
    return Number.isFinite(value) && value > 0 ? Math.trunc(value) : 0;
}
function positiveIntegerEnv(...names) {
    for (const name of names) {
        const value = Number(process.env[name] || NaN);
        if (Number.isFinite(value) && value > 0) {
            return Math.trunc(value);
        }
    }
    return null;
}
function processRssKb(pid) {
    try {
        const status = fs.readFileSync(`/proc/${pid}/status`, 'utf8');
        const match = status.match(/^VmRSS:\s+(\d+)\s+kB$/m);
        return match ? Number(match[1]) : 0;
    }
    catch {
        return 0;
    }
}
function processGroupId(pid) {
    try {
        const stat = fs.readFileSync(`/proc/${pid}/stat`, 'utf8');
        const end = stat.lastIndexOf(') ');
        if (end === -1) {
            return null;
        }
        const fields = stat.slice(end + 2).trim().split(/\s+/);
        const groupId = Number(fields[2]);
        return Number.isFinite(groupId) ? groupId : null;
    }
    catch {
        return null;
    }
}
function processGroupRssKb(rootPid) {
    if (process.platform !== 'linux') {
        return processRssKb(rootPid);
    }
    let total = 0;
    try {
        for (const entry of fs.readdirSync('/proc')) {
            if (!/^\d+$/.test(entry)) {
                continue;
            }
            const pid = Number(entry);
            if (processGroupId(pid) === rootPid) {
                total += processRssKb(pid);
            }
        }
    }
    catch {
        return processRssKb(rootPid);
    }
    return total || processRssKb(rootPid);
}
function startRssGuard(processRefs, label) {
    const limitKb = positiveIntegerEnv('PERF_MULTI_RSS_LIMIT_KB', 'PERF_RSS_LIMIT_KB');
    if (!limitKb) {
        return {
            promise: new Promise(() => { }),
            stop() { }
        };
    }
    const pollMs = positiveIntegerEnv('PERF_MULTI_RSS_POLL_MS', 'PERF_RSS_POLL_MS') || 250;
    let timer = null;
    let stopped = false;
    const promise = new Promise((_, reject) => {
        timer = setInterval(() => {
            if (stopped) {
                return;
            }
            const samples = [];
            let cellRssKb = 0;
            for (const [name, processRef] of processRefs) {
                if (!processRef || !processRef.pid) {
                    continue;
                }
                if (processRef.exitCode !== null || processRef.signalCode !== null) {
                    continue;
                }
                const rssKb = processGroupRssKb(processRef.pid);
                samples.push(`${name}=${rssKb}KB`);
                cellRssKb += rssKb;
            }
            if (cellRssKb > limitKb) {
                stopped = true;
                clearInterval(timer);
                reject(new Error(`rss guard exceeded for ${label}: cell=${cellRssKb}KB,limit=${limitKb}KB,${samples.join(',')}`));
            }
        }, pollMs);
        if (timer.unref) {
            timer.unref();
        }
    });
    return {
        promise,
        stop() {
            stopped = true;
            if (timer) {
                clearInterval(timer);
            }
        }
    };
}
async function waitForExit(processRef) {
    if (processRef.exitCode !== null || processRef.signalCode !== null) {
        return processRef.exitCode;
    }
    const [code] = await once(processRef, 'exit');
    return code;
}
async function flushProcessOutput() {
    await new Promise((resolve) => setTimeout(resolve, 50));
}
function isClosedPipeError(error) {
    return error
        && (error.code === 'EPIPE'
            || error.code === 'ERR_STREAM_DESTROYED'
            || error.code === 'ERR_STREAM_WRITE_AFTER_END');
}
function ensureChildStdinErrorHandler(processRef) {
    const stdin = processRef?.stdin;
    if (!stdin || processRef.__stdinErrorHandlerAttached) {
        return;
    }
    stdin.on('error', (error) => {
        if (!isClosedPipeError(error)) {
            processRef.__stdinWriteError = error;
        }
    });
    processRef.__stdinErrorHandlerAttached = true;
}
function writeChildLine(processRef, line, options = {}) {
    const stdin = processRef?.stdin;
    if (!stdin || !stdin.writable || stdin.destroyed) {
        return;
    }
    ensureChildStdinErrorHandler(processRef);
    try {
        stdin.write(line, (error) => {
            if (error && !isClosedPipeError(error)) {
                processRef.__stdinWriteError = error;
            }
        });
        if (options.end) {
            stdin.end();
        }
    }
    catch (error) {
        if (!isClosedPipeError(error)) {
            throw error;
        }
    }
}
async function terminateProcessTree(processRef, timeoutMs = 5000) {
    if (processRef.exitCode !== null || processRef.signalCode !== null) {
        return;
    }
    try {
        process.platform === 'win32' ? processRef.kill('SIGTERM') : process.kill(-processRef.pid, 'SIGTERM');
    }
    catch (error) {
        if (!error || error.code !== 'ESRCH') {
            throw error;
        }
        return;
    }
    const graceful = await Promise.race([
        waitForExit(processRef).then(() => true),
        new Promise((resolve) => setTimeout(() => resolve(false), timeoutMs))
    ]);
    if (graceful) {
        return;
    }
    try {
        process.platform === 'win32' ? processRef.kill('SIGKILL') : process.kill(-processRef.pid, 'SIGKILL');
    }
    catch (error) {
        if (!error || error.code !== 'ESRCH') {
            throw error;
        }
        return;
    }
    await waitForExit(processRef);
}
async function stopServer(server, label, timeoutMs = 5000) {
    writeChildLine(server, 'STOP\n', { end: true });
    try {
        const graceful = await Promise.race([
            once(server, 'exit'),
            new Promise((resolve) => setTimeout(() => resolve(null), timeoutMs))
        ]);
        if (graceful !== null) {
            const [code] = graceful;
            if (code !== 0) {
                throw new Error(`${label} failed: ${code}`);
            }
            return;
        }
        try {
            process.platform === 'win32' ? server.kill('SIGTERM') : process.kill(-server.pid, 'SIGTERM');
        }
        catch (error) {
            if (!error || error.code !== 'ESRCH') {
                throw error;
            }
        }
        const terminated = await Promise.race([
            waitForExit(server).then(() => true),
            new Promise((resolve) => setTimeout(() => resolve(false), timeoutMs))
        ]);
        if (terminated) {
            return;
        }
        try {
            process.platform === 'win32' ? server.kill('SIGKILL') : process.kill(-server.pid, 'SIGKILL');
        }
        catch (error) {
            if (!error || error.code !== 'ESRCH') {
                throw error;
            }
        }
        await waitForExit(server);
    }
    catch (error) {
        const stderr = stderrText(server);
        if (stderr) {
            error.message = `${error.message}\n${stderr}`;
        }
        throw error;
    }
}
function clientReadyLine(msgSize) {
    return `CLIENT_READY,${msgSize}`;
}
function startLine(msgSize) {
    return `START,${msgSize}`;
}
function needsClientReady(pattern) {
    return pattern === 'MULTI_DEALER_DEALER'
        || pattern === 'MULTI_PUBSUB'
        || pattern === 'MULTI_SPOT'
        || pattern === 'MULTI_SPOT_REQREP'
        || pattern === 'MULTI_SPOT_SENDSEND';
}
function needsRunnerStart(pattern) {
    return pattern === 'MULTI_DEALER_DEALER'
        || pattern === 'MULTI_PUBSUB'
        || pattern === 'MULTI_SPOT'
        || pattern === 'MULTI_SPOT_REQREP'
        || pattern === 'MULTI_SPOT_SENDSEND';
}
function childEnv(args, component) {
    const env = { ...process.env };
    if (args.extraEnv && typeof args.extraEnv === 'object') {
        Object.assign(env, args.extraEnv);
    }
    if (args.transport === 'wss' && env.NODE_TLS_REJECT_UNAUTHORIZED === undefined) {
        env.NODE_TLS_REJECT_UNAUTHORIZED = '0';
        env.NODE_NO_WARNINGS = env.NODE_NO_WARNINGS || '1';
    }
    env.PERF_MULTI_PATTERN = String(args.pattern || env.PERF_MULTI_PATTERN || '');
    env.PERF_MULTI_TRANSPORT = String(args.transport || env.PERF_MULTI_TRANSPORT || '');
    // C parity (bindings/c/perf/run_comparison.py ~L1830): each spawned
    // perf process is tagged with its component (server|client) so the
    // AUTO_HWM_DETAIL lines carry per-component identity and the
    // `## Auto-HWM Detail` table emits per-component (client/server) rows
    // instead of a single `process` row.
    if (component) {
        env.PERF_MULTI_COMPONENT = String(component);
    }
    if (Number.isFinite(args.hwm)) {
        env.PERF_MULTI_HWM = String(args.hwm);
    }
    if (Number.isFinite(args.sendHwm)) {
        env.PERF_MULTI_SNDHWM = String(args.sendHwm);
    }
    if (Number.isFinite(args.recvHwm)) {
        env.PERF_MULTI_RCVHWM = String(args.recvHwm);
    }
    if (args.sndbuf) {
        env.PERF_MULTI_SNDBUF = String(args.sndbuf);
    }
    if (args.rcvbuf) {
        env.PERF_MULTI_RCVBUF = String(args.rcvbuf);
    }
    if (args.autoHwmProfile) {
        env.PERF_CTX_AUTO_HWM_PROFILE = String(args.autoHwmProfile);
    }
    if (Number.isFinite(args.sendTimeoutMs)) {
        env.PERF_MULTI_SNDTIMEO_MS = String(args.sendTimeoutMs);
    }
    if (Number.isFinite(args.recvTimeoutMs)) {
        env.PERF_MULTI_RCVTIMEO_MS = String(args.recvTimeoutMs);
    }
    if (Number.isFinite(args.connectReadyTimeoutMs)) {
        env.PERF_MULTI_CONNECT_READY_TIMEOUT_MS = String(args.connectReadyTimeoutMs);
    }
    if (Number.isFinite(args.monitorHwm)) {
        env.PERF_MULTI_MONITOR_HWM = String(args.monitorHwm);
    }
    if (Number.isFinite(args.connectConcurrency)) {
        env.PERF_MULTI_CONNECT_CONCURRENCY = String(args.connectConcurrency);
    }
    if (Number.isFinite(args.ioThreads)) {
        env.PERF_IO_THREADS = String(args.ioThreads);
    }
    if (Number.isFinite(args.serverIoThreads)) {
        env.PERF_MULTI_SERVER_IO_THREADS = String(args.serverIoThreads);
    }
    if (Number.isFinite(args.clientIoThreads)) {
        env.PERF_MULTI_CLIENT_IO_THREADS = String(args.clientIoThreads);
    }
    return env;
}
function pinCpuEnabled(args) {
    return Boolean(args.pinCpu || process.env.PERF_TASKSET === '1');
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
function resolveSharedStreamClientBinary() {
    if (process.env.PERF_STREAM_CLIENT_BINARY) {
        return process.env.PERF_STREAM_CLIENT_BINARY;
    }
    let cursor = __dirname;
    for (let i = 0; i < 8; i += 1) {
        const candidate = path.join(cursor, 'bindings', 'c', 'build', 'perf', 'perf_stream_client');
        if (require('node:fs').existsSync(candidate)) {
            return candidate;
        }
        const next = path.dirname(cursor);
        if (next === cursor) {
            break;
        }
        cursor = next;
    }
    throw new Error('shared perf_stream_client not found; build bindings/c first');
}
function buildClientSpawn(clientPath, clientArgs, args) {
    if (args.pattern !== 'MULTI_STREAM') {
        return buildPinnedSpawn(process.execPath, [clientPath, ...clientArgs], args);
    }
    let streamClients = args.clients;
    const nonTcpMax = Number(process.env.PERF_STREAM_NON_TCP_CLIENTS_MAX || process.env.PERF_MULTI_STREAM_NON_TCP_CLIENTS_MAX || 10000);
    if (args.transport !== 'tcp' && Number.isFinite(streamClients) && Number.isFinite(nonTcpMax) && streamClients > nonTcpMax) {
        streamClients = Math.trunc(nonTcpMax);
    }
    const streamClientArgs = [
        '--endpoint', clientArgs[1],
        '--transport', args.transport,
        '--pattern', 'MULTI_STREAM',
        '--ccu', String(streamClients),
        '--sizes', String(args.msgSize),
        '--runs', '1',
        '--duration', String(args.duration),
        '--completion-wait-ms',
        process.env.PERF_MULTI_STREAM_COMPLETION_WAIT_MS
            || process.env.PERF_STREAM_COMPLETION_WAIT_MS
            || '2000',
        '--send-stop-token', '1'
    ];
    const ioThreads = Number.isFinite(args.clientIoThreads)
        ? args.clientIoThreads
        : args.ioThreads;
    if (Number.isFinite(ioThreads) && ioThreads > 0) {
        streamClientArgs.push('--io-threads', String(ioThreads));
    }
    return buildPinnedSpawn(resolveSharedStreamClientBinary(), streamClientArgs, args);
}
function resolveMultiTimeoutSeconds(args) {
    const override = Number(process.env.PERF_MULTI_TIMEOUT_SECONDS || process.env.PERF_TIMEOUT_SECONDS || 0);
    if (Number.isFinite(override) && override > 0) {
        return Math.trunc(override);
    }
    const duration = Math.max(Number(args.duration) || 0, 1);
    const msgSize = Math.max(Number(args.msgSize) || 0, 64);
    if (args.pattern === 'MULTI_STREAM') {
        return Math.max(45, Math.floor(duration * 3) + 20);
    }
    if (args.pattern === 'MULTI_SPOT'
        || args.pattern === 'MULTI_SPOT_REQREP'
        || args.pattern === 'MULTI_SPOT_SENDSEND') {
        return Math.max(90, Math.floor(duration * 6) + 30);
    }
    if ((args.transport === 'tls' || args.transport === 'wss') && msgSize >= 131072) {
        return Math.max(90, Math.floor(duration * 6) + 30);
    }
    return Math.max(45, Math.floor(duration * 3) + 20);
}
function resolveClientReadyTimeoutMs(args) {
    const configured = Number.isFinite(args.connectReadyTimeoutMs)
        ? args.connectReadyTimeoutMs
        : 5000;
    if (args.pattern === 'MULTI_SPOT'
        || args.pattern === 'MULTI_SPOT_REQREP'
        || args.pattern === 'MULTI_SPOT_SENDSEND') {
        const spotReadyTimeout = Number(process.env.PERF_MULTI_SPOT_READY_TIMEOUT_MS);
        const minimumSpotReadyTimeout = Number.isFinite(spotReadyTimeout)
            ? spotReadyTimeout
            : Math.max(5000, configured * 6);
        return Math.max(configured, minimumSpotReadyTimeout);
    }
    return configured;
}
async function spawnMultiPair(serverScript, clientScript, args) {
    const serverPath = path.join(__dirname, serverScript);
    // MULTI_STREAM has no Node client script: buildClientSpawn spawns the
    // shared C `perf_stream_client` binary instead (parity with cpp/dotnet).
    const clientPath = clientScript ? path.join(__dirname, clientScript) : null;
    const resultLines = [];
    const endpoint = await benchmarkEndpoint(args.transport, `${String(args.pattern || 'multi').toLowerCase()}-${args.msgSize}`, args.serverBindPort);
    let serverArgs = [
        '--endpoint', endpoint,
        '--transport', args.transport,
        '--msg-size', String(args.msgSize),
        '--duration', String(args.duration),
        '--clients', String(args.clients)
    ];
    let clientArgs = [...serverArgs];
    if (args.pattern === 'MULTI_SPOT'
        || args.pattern === 'MULTI_SPOT_REQREP'
        || args.pattern === 'MULTI_SPOT_SENDSEND') {
        const serverControlEndpoint = `tcp://127.0.0.1:${await reservePort()}`;
        const clientControlEndpoint = `tcp://127.0.0.1:${await reservePort()}`;
        const peerEndpoint = args.pattern === 'MULTI_SPOT'
            ? await benchmarkEndpoint(args.transport, `${String(args.pattern).toLowerCase()}-data-${args.msgSize}`)
            : endpoint;
        serverArgs = [
            '--endpoint', endpoint,
            '--transport', args.transport,
            '--peer-endpoint', peerEndpoint,
            '--control-endpoint', serverControlEndpoint,
            '--msg-size', String(args.msgSize),
            '--duration', String(args.duration),
            '--clients', String(args.clients)
        ];
        clientArgs = [
            '--endpoint', endpoint,
            '--transport', args.transport,
            '--peer-endpoint', peerEndpoint,
            '--control-endpoint', clientControlEndpoint,
            '--server-control-endpoint', serverControlEndpoint,
            '--msg-size', String(args.msgSize),
            '--duration', String(args.duration),
            '--clients', String(args.clients)
        ];
    }
    const serverSpawn = buildPinnedSpawn(process.execPath, [serverPath, ...serverArgs], args);
    const server = spawn(serverSpawn.command, serverSpawn.args, {
        cwd: process.cwd(),
        env: childEnv(args, 'server'),
        stdio: ['pipe', 'pipe', 'pipe'],
        detached: true
    });
    attachProcessCapture(server, resultLines);
    try {
        await waitForLine(server, `READY,${endpoint}`, serverScript, Number.isFinite(args.serverReadyTimeoutMs) ? args.serverReadyTimeoutMs : 10_000);
    }
    catch (error) {
        await Promise.allSettled([terminateProcessTree(server, 1000)]);
        if (hasAddressInUse(server) && startupBindRetryCount(args) < 3) {
            return spawnMultiPair(serverScript, clientScript, {
                ...args,
                __startupBindRetry: startupBindRetryCount(args) + 1
            });
        }
        const stderr = stderrText(server);
        if (stderr) {
            error.message = `${error.message}\n${stderr}`;
        }
        throw error;
    }
    if (args.pattern === 'MULTI_SPOT'
        || args.pattern === 'MULTI_SPOT_REQREP'
        || args.pattern === 'MULTI_SPOT_SENDSEND') {
        try {
            await waitForPrefix(server, 'CONTROL_READY,', serverScript, 10_000);
        }
        catch (error) {
            await Promise.allSettled([terminateProcessTree(server, 1000)]);
            if (hasAddressInUse(server) && startupBindRetryCount(args) < 3) {
                return spawnMultiPair(serverScript, clientScript, {
                    ...args,
                    __startupBindRetry: startupBindRetryCount(args) + 1
                });
            }
            const stderr = stderrText(server);
            if (stderr) {
                error.message = `${error.message}\n${stderr}`;
            }
            throw error;
        }
    }
    const clientSpawn = buildClientSpawn(clientPath, clientArgs, args);
    const client = spawn(clientSpawn.command, clientSpawn.args, {
        cwd: process.cwd(),
        env: childEnv(args, 'client'),
        stdio: ['pipe', 'pipe', 'pipe'],
        detached: true
    });
    attachProcessCapture(client, resultLines);
    if (args.pattern === 'MULTI_SPOT'
        || args.pattern === 'MULTI_SPOT_REQREP'
        || args.pattern === 'MULTI_SPOT_SENDSEND') {
        const clientControlLine = await waitForPrefix(client, 'CLIENT_CONTROL_ENDPOINT,', clientScript, resolveClientReadyTimeoutMs(args));
        const clientControlEndpoint = clientControlLine.split(',')[1];
        writeChildLine(server, `CONNECT_CONTROL,${clientControlEndpoint}\n`);
        const controlConnectedLine = await waitForPrefix(server, 'CONTROL_CONNECTED,', serverScript, resolveClientReadyTimeoutMs(args));
        writeChildLine(client, `${controlConnectedLine}\n`);
    }
    if (needsClientReady(args.pattern)) {
        await waitForLine(client, clientReadyLine(args.msgSize), clientScript, resolveClientReadyTimeoutMs(args));
    }
    if (needsRunnerStart(args.pattern)) {
        writeChildLine(server, `${startLine(args.msgSize)}\n`);
        writeChildLine(client, `${startLine(args.msgSize)}\n`);
    }
    const clientTimeoutMs = resolveMultiTimeoutSeconds(args) * 1000;
    const rssGuard = startRssGuard([['server', server], ['client', client]], `${args.pattern} ${args.transport} ${args.msgSize}B`);
    let clientExitCode;
    try {
        clientExitCode = await Promise.race([
            waitForExit(client),
            new Promise((_, reject) => {
                setTimeout(() => reject(new Error(`client timeout after ${clientTimeoutMs}ms`)), clientTimeoutMs);
            }),
            rssGuard.promise
        ]);
    }
    catch (error) {
        await Promise.allSettled([terminateProcessTree(server, 1000), terminateProcessTree(client, 1000)]);
        const stderr = stderrText(client);
        if (stderr) {
            error.message = `${error.message}\n${stderr}`;
        }
        throw error;
    }
    finally {
        rssGuard.stop();
    }
    if (clientExitCode !== 0) {
        const stderr = stderrText(client);
        await Promise.allSettled([terminateProcessTree(server, 1000)]);
        const error = new Error(`client failed (${clientScript}): ${clientExitCode}`);
        if (stderr) {
            error.message = `${error.message}\n${stderr}`;
        }
        throw error;
    }
    await flushProcessOutput();
    // C parity: for patterns where the SERVER is the measurer (e.g.
    // MULTI_DEALER_DEALER after the C-correct role re-alignment — C's
    // perf_multi_dealer_dealer_server.cpp prints RESULT), the sender client
    // exits first while the server is still finishing its measure window +
    // tail drain. The C engine (run_comparison.py) waits for the measuring
    // process to finish; stopping the server on the default short grace
    // would kill it before it emits RESULT. So if no RESULT has been
    // captured yet, wait for the server to emit one (or exit) before
    // shutdown. For client-measurer patterns resultLines already holds the
    // RESULT and this resolves immediately.
    if (!resultLines.some((line) => line.startsWith('RESULT,'))
        && server.exitCode === null && server.signalCode === null) {
        // Poll resultLines; a waiter would consume RESULT before capture stores it.
        // intercept the RESULT line before attachProcessCapture pushes it).
        const resultDeadline = Date.now() + clientTimeoutMs;
        while (Date.now() < resultDeadline
            && !resultLines.some((line) => line.startsWith('RESULT,'))
            && server.exitCode === null && server.signalCode === null) {
            await new Promise((resolve) => setTimeout(resolve, 100));
        }
        await flushProcessOutput();
    }
    try {
        await stopServer(server, serverScript, Number.isFinite(args.serverShutdownTimeoutMs) ? args.serverShutdownTimeoutMs : 5000);
        await flushProcessOutput();
        const hasResultLines = resultLines.some((line) => line.startsWith('RESULT,'));
        if (!hasResultLines && (hasProtocolUnsupported(client) || hasProtocolUnsupported(server))) {
            resultLines.push(`UNSUPPORTED,current,${args.pattern},${args.transport}`);
        }
        return resultLines;
    }
    finally {
        await Promise.allSettled([terminateProcessTree(server, 1000), terminateProcessTree(client, 1000)]);
    }
}
module.exports = {
    attachProcessCapture,
    spawnMultiPair,
    stopServer,
    waitForLine,
    waitForPrefix
};
