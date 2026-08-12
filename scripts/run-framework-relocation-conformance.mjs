#!/usr/bin/env node

import assert from 'node:assert/strict';
import { access, readFile } from 'node:fs/promises';
import { spawn } from 'node:child_process';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const scriptDirectory = dirname(fileURLToPath(import.meta.url));
const repositoryRoot = resolve(scriptDirectory, '..');
const conformanceDirectory = resolve(
  repositoryRoot,
  'framework/runtime/conformance'
);
const readJson = async (name) => JSON.parse(await readFile(
  resolve(conformanceDirectory, name),
  'utf8'
));

const fixture = await readJson('relocation-behavior-v1.json');
const boundSession = await readJson('bound-session-relocation-v1.json');
const inventory = await readJson('relocation-conformance-adapters-v1.json');
const e2e = await readJson('relocation-e2e-scenarios-v1.json');
await import('../framework/runtime/conformance/validate-runtime-conformance-fixtures.mjs');
const requiredLanguages = ['cpp', 'dotnet', 'java', 'kotlin', 'node'];
const terminationGraceMilliseconds = 2_000;

assert.equal(fixture.fixture, 'zlink.framework.relocation-behavior');
assert.equal(boundSession.fixture, 'zlink.framework.bound-session-relocation');
assert.equal(inventory.fixture, 'zlink.framework.relocation-conformance-adapters');
assert.equal(e2e.fixture, 'zlink.framework.relocation-e2e-scenarios');
assert.deepEqual(inventory.requiredLanguages, requiredLanguages);
assert.deepEqual(e2e.requiredLanguages, requiredLanguages);
assert.deepEqual(
  inventory.adapters.map(({ language }) => language),
  requiredLanguages
);
assert.deepEqual(
  e2e.languageScenarioMatrix.rows.map(({ language }) => language),
  requiredLanguages
);
assert.deepEqual(
  inventory.requiredBehaviorGroups,
  fixture.adapterContract.requiredBehaviorGroups
);
assert.equal(inventory.focusedCommandTimeoutSeconds, 600);
assert.equal(
  inventory.boundSessionFixture,
  'framework/runtime/conformance/bound-session-relocation-v1.json'
);
assert.equal(
  boundSession.behaviorFixture,
  'framework/runtime/conformance/relocation-behavior-v1.json'
);
assert.equal(e2e.executionPolicy, 'inventoryOnly');
assert.equal(e2e.runFromFocusedConformance, false);

const allowedCoverage = new Set(['covered', 'partial', 'pending', 'e2eRequired']);
for (const adapter of inventory.adapters) {
  assert.ok(['direct', 'mapped', 'pending'].includes(adapter.fixtureConnection));
  assert.deepEqual(Object.keys(adapter.coverage), inventory.requiredBehaviorGroups);
  for (const status of Object.values(adapter.coverage))
    assert.ok(allowedCoverage.has(status), `${adapter.language}: ${status}`);
  assert.ok(Array.isArray(adapter.evidence) && adapter.evidence.length > 0);
  for (const evidence of adapter.evidence) {
    await access(resolve(repositoryRoot, evidence.path));
    assert.ok(Array.isArray(evidence.tests) && evidence.tests.length > 0);
  }
  assert.ok(Array.isArray(adapter.focusedCommands)
    && adapter.focusedCommands.length > 0);
}

const args = process.argv.slice(2);
const languageIndex = args.indexOf('--language');
const selectedLanguage = languageIndex < 0 ? undefined : args[languageIndex + 1];
const listOnly = args.includes('--list');
const requireComplete = args.includes('--require-complete');
assert.equal(
  listOnly && requireComplete,
  false,
  '--list does not run the complete conformance gate'
);
assert.equal(
  selectedLanguage !== undefined && requireComplete,
  false,
  '--require-complete always covers all required languages'
);

if (requireComplete) {
  const incomplete = inventory.adapters.filter(adapter =>
    adapter.fixtureConnection !== 'direct'
    || Object.values(adapter.coverage).some(status => status !== 'covered'));
  assert.deepEqual(
    incomplete.map(({ language }) => language),
    [],
    `incomplete relocation adapters: ${incomplete.map(({ language }) => language).join(', ')}`
  );
}

if (listOnly) {
  for (const adapter of inventory.adapters) {
    const summary = inventory.requiredBehaviorGroups
      .map(group => `${group}=${adapter.coverage[group]}`)
      .join(', ');
    console.log(`${adapter.language}: ${adapter.fixtureConnection}; ${summary}`);
  }
  console.log(`e2e: inventoryOnly; ${e2e.scenarios.length} scenarios; not executed`);
  process.exit(0);
}

const runAdapter = async (adapter) => {
  const selectedLanguage = adapter.language;
  assert.notEqual(
    adapter.fixtureConnection,
    'pending',
    `${selectedLanguage} adapter has no runnable focused evidence`
  );
  const workingLanguage = selectedLanguage === 'kotlin'
    ? 'java'
    : selectedLanguage;
  for (const [command, ...commandArgs] of adapter.focusedCommands) {
    await new Promise((resolveCommand, rejectCommand) => {
      let timedOut = false;
      let terminationError;
      const ownsProcessGroup = process.platform !== 'win32';
      const child = spawn(command, commandArgs, {
        cwd: resolve(repositoryRoot, `framework/languages/${workingLanguage}`),
        stdio: 'inherit',
        shell: false,
        detached: ownsProcessGroup
      });
      const terminateProcessTree = (signal) => {
        try {
          if (ownsProcessGroup && child.pid !== undefined)
            process.kill(-child.pid, signal);
          else
            child.kill(signal);
        } catch (error) {
          if (error?.code !== 'ESRCH') terminationError = error;
        }
      };
      let forceKill;
      const timeout = setTimeout(() => {
        timedOut = true;
        terminateProcessTree('SIGTERM');
        forceKill = setTimeout(() => {
          terminateProcessTree('SIGKILL');
          rejectCommand(new Error(
            `${selectedLanguage} focused command exceeded `
            + `${inventory.focusedCommandTimeoutSeconds}s`
            + (terminationError === undefined
              ? ''
              : `; process-tree termination failed: ${terminationError.message}`)
          ));
        }, terminationGraceMilliseconds);
      }, inventory.focusedCommandTimeoutSeconds * 1_000);
      child.once('error', error => {
        clearTimeout(timeout);
        clearTimeout(forceKill);
        rejectCommand(error);
      });
      child.once('exit', (code, signal) => {
        clearTimeout(timeout);
        if (timedOut) return;
        clearTimeout(forceKill);
        if (code === 0) resolveCommand();
        else rejectCommand(new Error(
          `${selectedLanguage} focused command failed (code=${code}, signal=${signal ?? 'none'})`
        ));
      });
    });
  }
  console.log(requireComplete
    ? `${selectedLanguage}: direct focused conformance passed`
    : `${selectedLanguage} adapter classification: ${adapter.fixtureConnection}; `
      + 'focused evidence passed, complete conformance is not implied');
};

if (requireComplete) {
  for (const adapter of inventory.adapters)
    await runAdapter(adapter);
} else if (selectedLanguage !== undefined) {
  const adapter = inventory.adapters.find(
    ({ language }) => language === selectedLanguage
  );
  assert.ok(adapter, `unknown language '${selectedLanguage}'`);
  await runAdapter(adapter);
} else {
  for (const adapter of inventory.adapters)
    await runAdapter(adapter);
}

if (requireComplete)
  console.log('framework relocation focused conformance: COMPLETE');
console.log('framework relocation conformance inventory: PASS');
console.log('relocation E2E scenarios: inventory validated, not executed');
