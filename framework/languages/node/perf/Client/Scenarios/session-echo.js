'use strict';
const { PerfEchoRequest, PerfBindRequest, validation } = require('../../Shared/contracts');
const { now } = require('../../Shared/measurement');

async function createSessionScenario(manifest, measure, index) {
  const w = manifest.workload, total = w.connections;
  const setupStarted = now(), setupSignal = AbortSignal.timeout(w.setupTimeoutMs);
  const quotient = Math.floor(total / w.clientCount), remainder = total % w.clientCount;
  const count = quotient + (index < remainder ? 1 : 0), first = index * quotient + Math.min(index, remainder);
  measure.requestedConnections = count;
  const endpoint = manifest.roles.find(role => role.streamEndpoint)?.streamEndpoint;
  if (!endpoint || !endpoint.startsWith('ws://')) throw new Error('CS perf uses public WebSocket connector and ws:// endpoint.');
  if (typeof globalThis.WebSocket !== 'function') throw new Error('Node >=22 with platform WebSocket is required.');
  const connectors = [], owned = [], evidence = [], sequences = Array.from({ length: count }, () => 0n);
  measure.setupEvidence = evidence;
  const connectorPackage = await import('@zlink-systems/stream-connector');
  let next = 0;
  async function connectLane() {
    while (next < count && !setupSignal.aborted) {
      const local = next++, id = first + local, started = now();
      try {
        const connector = connectorPackage.zlinkStreamConnectorFactory.create({ endpoint, dispatchMode: connectorPackage.ZlinkStreamDispatchMode.Immediate, requestTimeoutMs: w.requestTimeoutMs, connectTimeoutMs: w.setupTimeoutMs, diagnosticsLevel: connectorPackage.ZlinkStreamDiagnosticsLevel.Off });
        owned.push(connector);
        await connector.connect(setupSignal);
        if (manifest.scenario !== 'session-echo-only') {
          const binding = await connector.request(new PerfBindRequest(manifest.runId, manifest.cellId, id)).packetName('PerfBindRequest').timeout(w.setupTimeoutMs).submit(setupSignal);
          if (!binding.bound) throw validation('IdentityMismatch', 'Session bind rejected.');
        }
        const request = measure.request(id, ++sequences[local], true);
        const reply = await connector.request(request, PerfEchoRequest).packetName('PerfEchoRequest').timeout(w.requestTimeoutMs).submit(setupSignal);
        measure.validateReply(request, reply);
        if (!connector.isConnected) throw validation('IdentityMismatch', 'Connector disconnected after probe.');
        connectors.push({ id, local, connector }); measure.connected++;
        evidence.push({ kind: 'connectorSetupAndTypedProbe', source: 'public connect/isConnected/request.submit + typed full validation', observedValue: { clientId: id, setupLatencyNs: String(now() - started), state: connector.state } });
      } catch (error) { measure.connectionFailures++; measure.error(error); evidence.push({ kind: 'connectorSetupFailure', source: error.name, observedValue: { clientId: id, message: error.message, setupLatencyNs: String(now() - started) } }); }
    }
  }
  await Promise.all(Array.from({ length: Math.min(count, w.connectConcurrency) }, connectLane));
  if (setupSignal.aborted) {
    measure.error(setupSignal.reason, false, true);
    evidence.push({ kind: 'setupBudgetExpired', source: 'shared public AbortSignal across connect/bind/probe', observedValue: { requested: count, attempted: next, connected: measure.connected, notAttempted: count - next, setupTimeoutMs: w.setupTimeoutMs, observedDurationNs: String(now() - setupStarted) } });
  }
  return {
    async run() {
      await Promise.all(connectors.flatMap(({ id, local, connector }) => Array.from({ length: w.inflight }, async () => {
        while (measure.canIssue) {
          const request = measure.request(id, ++sequences[local]), op = measure.begin(request);
          if (!op) return;
          measure.directional('request', w.requestPayloadBytes);
          try { const reply = await connector.request(request, PerfEchoRequest).packetName('PerfEchoRequest').timeout(w.requestTimeoutMs).submit(); measure.validateReply(request, reply); measure.finish(op); }
          catch (error) { measure.finish(op, error); }
          await new Promise(resolve => setImmediate(resolve));
        }
      })));
    },
    async close() { await Promise.all(owned.map(connector => connector.close())); }
  };
}
module.exports = { createSessionScenario };
