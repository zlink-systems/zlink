#!/usr/bin/env node

import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { spawnSync } from "node:child_process";

function fail(message) {
  process.stderr.write(`${message}\n`);
  process.exit(2);
}

const [manifestPath, scenarioId] = process.argv.slice(2);
const token = process.env.ZLINK_V11_ORACLE_CHILD_TOKEN;
if (!manifestPath || !scenarioId || !token) {
  fail("oracle child must be started by run-oracle.sh");
}

const manifestBytes = readFileSync(manifestPath);
const manifest = JSON.parse(manifestBytes);
const scenario = manifest.scenarios?.find((item) => item.id === scenarioId);
if (!scenario) {
  fail(`unknown oracle scenario: ${scenarioId}`);
}

const executablePath = resolve(dirname(manifestPath), scenario.runner.path);
const executable = spawnSync(executablePath, [], {
  encoding: "utf8",
  env: { ...process.env },
  stdio: ["ignore", "pipe", "pipe"],
  timeout: 120000
});
if (executable.error) {
  fail(`frozen oracle executable failed to start: ${executable.error.message}`);
}
if (executable.status !== 0) {
  fail(`frozen oracle executable failed (${executable.status}): ${executable.stderr.trim()}`);
}

const observations = scenario.expectedTests.map((expected, index) => {
  const marker = `:${expected.test}:PASS`;
  if (!executable.stdout.includes(marker)) {
    fail(`frozen oracle executable omitted expected result: ${expected.test}`);
  }
  return {
    sequence: index + 1,
    event: expected.event,
    result: expected.result,
    sourceTest: expected.test,
    sourceResult: "PASS"
  };
});

const trace = {
  schema: "zlink-v11-normalized-oracle-trace-v1",
  scenarioId,
  disposition: scenario.disposition,
  outcome: scenario.outcome,
  observations,
  provenance: {
    oracleArtifactId: manifest.artifact.id,
    oracleVersion: manifest.artifact.version,
    childSha256: manifest.artifact.child.sha256,
    executablePathAtFreeze: scenario.runner.path,
    executableSha256: scenario.runner.sha256,
    executableExitCode: executable.status,
    sourceRevision: manifest.artifact.sourceRevision,
    manifestSha256: createHash("sha256").update(manifestBytes).digest("hex")
  }
};

process.stdout.write(JSON.stringify({
  protocol: "zlink-v11-oracle-child-v1",
  token,
  trace
}));
