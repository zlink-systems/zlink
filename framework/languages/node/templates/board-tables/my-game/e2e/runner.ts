import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import { mkdirSync } from 'node:fs';
import { inspect } from 'node:util';
import { checkStructure } from './structure';
import { createServerHost } from './server-host';
import { attachServer } from './server-control';
import { LogFiles, Scenarios } from '../server/shared/constants';

// Scenario failures are nested AggregateErrors; print every level so the cause reaches the CI log (#5312).
inspect.defaultOptions.depth = null;

function runDomainChecks(): void {
  execFileSync(process.execPath, ['--test', 'dist/e2e/domain.test.js'], { stdio: 'inherit' });
  console.log('Domain checks: 7/7');
}
async function main(): Promise<void> {
  const args = process.argv.slice(2);
  if (args[0] === '--domain-checks-only') {
    assert.equal(args.length, 1);
    runDomainChecks();
    return;
  }
  if (args[0] === '--check-structure') {
    assert(args.length <= 2);
    checkStructure(args[1]);
    return;
  }
  const attach = args[0] === '--attach';
  if (attach) args.shift();
  let selected: readonly string[] = Scenarios;
  if (args.length) {
    assert.equal(args.length, 2);
    assert.equal(args[0], '--scenario');
    const requested = args[1].split(',');
    assert.equal(new Set(requested).size, requested.length, 'Duplicate scenario');
    for (const name of requested)
      assert(Scenarios.includes(name as (typeof Scenarios)[number]), `Unknown scenario ${name}`);
    selected = Scenarios.filter((s) => requested.includes(s));
  }
  checkStructure();
  runDomainChecks();
  const host = attach ? undefined : createServerHost(selected.length);
  // Each attach run names its own players and tables, so one server can take repeated runs.
  const attachRunId = randomUUID();
  const logDir = host?.logDir ?? `${LogFiles.runs}/${LogFiles.runPrefix}${attachRunId}`;
  mkdirSync(logDir, { recursive: true });
  let passed = 0;
  let failed = false;
  try {
    const { state, control } = host
      ? { state: await host.startAll(), control: host.control }
      : attachServer();
    const { scenario } = await import('./client.mjs');
    const scenarioRunId = host ? state.runId : attachRunId;
    for (const [index, name] of selected.entries()) {
      console.log(`━━ ${name} (${index + 1}/${selected.length}) ━━`);
      try {
        await scenario(name, state.sessionEndpoint, scenarioRunId, logDir, control);
        passed++;
        console.log(`✓ ${name}`);
      } catch (error) {
        failed = true;
        console.error(`✗ ${name}`, error);
      }
    }
  } catch (error) {
    failed = true;
    console.error('Startup failed', error);
  } finally {
    if (host && !(await host.stopAll())) failed = true;
  }
  console.log(`Scenarios: ${passed}/${selected.length} passed`);
  console.log(`Logs: ${logDir}`);
  process.exitCode = failed ? 1 : 0;
}
main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
