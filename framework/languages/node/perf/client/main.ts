import * as readline from 'node:readline';
import { EndpointManifest, PerfTriggerRequest, readJson, ResetRequest, toJson } from '../shared/contracts';
import { Measurement } from '../shared/measurement';
import { MetricsClient } from './metrics-client';
import { CsLocalSessionActorEchoScenario } from './scenarios/cs-local-session-actor-echo-scenario';
import { CsRemoteSessionActorEchoScenario } from './scenarios/cs-remote-session-actor-echo-scenario';
import { SessionEchoOnlyScenario } from './scenarios/session-echo-only-scenario';

// The CS client and the phase trigger client of the common runner (perf §6.5, §16). The runner drives it through JSON
// lines on stdin/stdout; stdout carries control JSON only.
async function main(argv: string[]): Promise<void> {
  if (argv.length !== 4 || argv[0] !== '--endpoint-config' || argv[2] !== '--client-index' || !/^\d+$/.test(argv[3])) {
    throw new Error('Client requires --endpoint-config <file> --client-index <index>.');
  }
  const manifest = readJson<EndpointManifest>(argv[1]);
  const index = Number(argv[3]);
  if (index >= manifest.workload.clientCount) throw new RangeError('client index out of range');
  const cs = manifest.roles.some((role) => role.streamEndpoint !== null);
  const measurement = new Measurement({ runId: manifest.runId, cellId: manifest.cellId, configHash: manifest.configHash, role: 'client', roleInstance: index,
    workload: manifest.workload, provenance: manifest.provenance }, cs);
  // Every CS cell shares the connector loop of the baseline; the cell id names which standard scenario runs.
  const scenarioName = manifest.cellId.split('/')[0];
  const scenario = !cs ? undefined
    : scenarioName === 'cs-local-session-actor-echo' ? new CsLocalSessionActorEchoScenario(manifest, measurement, index)
      : scenarioName === 'cs-remote-session-actor-echo' ? new CsRemoteSessionActorEchoScenario(manifest, measurement, index)
        : new SessionEchoOnlyScenario(manifest, measurement, index);
  const admin = new MetricsClient(manifest);
  const write = (value: unknown): void => { process.stdout.write(toJson(value) + '\n'); };
  await scenario?.prepare();
  write({ type: 'prepared', ok: !measurement.hasErrors, snapshot: measurement.snapshot(null) });
  const lines = readline.createInterface({ input: process.stdin });
  for await (const line of lines) {
    try {
      const message = JSON.parse(line) as { command: string; request?: unknown };
      let response: unknown;
      switch (message.command) {
        case 'start': response = measurement.startPhase(message.request as PerfTriggerRequest, scenario?.run); break;
        case 'triggerRoles': response = await admin.triggerRoles(message.request as PerfTriggerRequest); break;
        case 'resetRoles': response = await admin.resetRoles(message.request as ResetRequest); break;
        case 'reset': response = measurement.resetPhase(message.request as ResetRequest, undefined); break;
        case 'wait':
          await measurement.phaseTask;
          response = { ok: !measurement.hasErrors, phase: measurement.phase };
          break;
        case 'stats': response = measurement.snapshot(null); break;
        case 'stop':
          await scenario?.dispose();
          process.exit(0);
          return;
        default: throw new SyntaxError('Unknown control command.');
      }
      write({ ok: true, response });
    } catch (error) {
      measurement.recordDiagnostic(error);
      write({ ok: false, errorType: error instanceof Error ? error.constructor.name : typeof error, message: error instanceof Error ? error.message : String(error) });
    }
  }
}

main(process.argv.slice(2)).catch((error: unknown) => {
  console.error(error);
  process.exit(1);
});
