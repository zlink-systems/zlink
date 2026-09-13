'use strict';
const readline = require('node:readline');
const { Measurement } = require('../Shared/measurement');
const { json } = require('../Shared/contracts');
const { createSessionScenario } = require('./Scenarios/session-echo');

async function runClient(manifest, index) {
  if (!Number.isInteger(index) || index < 0 || index >= manifest.workload.clientCount) throw new Error('client-index outside configured pool.');
  const config = { runId: manifest.runId, cellId: manifest.cellId, configHash: manifest.configHash, role: 'client', roleInstance: index, scenario: manifest.scenario, mode: manifest.mode, terminal: 'ordinary', source: true, workload: manifest.workload, provenance: manifest.provenance };
  const measurement = new Measurement(config, true);
  const write = value => process.stdout.write(`${json(value)}\n`);
  let scenario;
  try { scenario = await createSessionScenario(manifest, measurement, index); }
  catch (error) { measurement.error(error, false, true); write({ type: 'prepared', ok: false, snapshot: measurement.snapshot() }); return; }
  write({ type: 'prepared', ok: !measurement.hasErrors, snapshot: measurement.snapshot() });
  const input = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });
  try {
    for await (const line of input) {
      try {
        const command = JSON.parse(line); let response;
        switch (command.command) {
          case 'start': response = measurement.startPhase(command.request, () => scenario.run()); break;
          case 'reset': response = measurement.reset(command.request); break;
          case 'wait': await measurement.phaseTask; response = { ok: !measurement.hasErrors, phase: measurement.phase }; break;
          case 'stats': response = measurement.snapshot(); break;
          case 'stop': return;
          case 'triggerRoles': case 'resetRoles': {
            const reset = command.command === 'resetRoles';
            response = await Promise.all(manifest.roles.map(async role => {
              const url = reset ? `${role.metrics.baseUrl}/perf/reset` : role.applicationTriggerUrl;
              const result = await fetch(url, { method: 'POST', headers: { 'content-type': 'application/json' }, body: json(command.request), signal: AbortSignal.timeout(manifest.workload.adminTimeoutMs) });
              const value = await result.json(); if (!result.ok) throw new Error(`HTTP ${result.status}: ${json(value)}`); return value;
            })); break;
          }
          default: throw new Error(`Unknown command ${command.command}.`);
        }
        write({ ok: true, response });
      } catch (error) { measurement.error(error); write({ ok: false, errorType: error.name, message: error.message }); }
    }
  } finally { input.close(); await scenario.close(); }
}
module.exports = { runClient };
