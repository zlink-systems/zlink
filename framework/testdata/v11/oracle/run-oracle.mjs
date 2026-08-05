#!/usr/bin/env node

import { createHash, randomBytes } from "node:crypto";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { dirname, isAbsolute, join, resolve } from "node:path";
import { spawnSync } from "node:child_process";
import { tmpdir } from "node:os";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));

function fail(message) {
  process.stderr.write(`${message}\n`);
  process.exitCode = 2;
  return false;
}

function sha256(path) {
  return createHash("sha256").update(readFileSync(path)).digest("hex");
}

function parseArgs(argv) {
  if (argv.length === 1 && argv[0] === "--self-test") return { selfTest: true };
  const result = {};
  for (let i = 0; i < argv.length; i += 2) {
    const key = argv[i];
    const value = argv[i + 1];
    if (!value || !["--manifest", "--scenario", "--output"].includes(key)) {
      throw new Error("usage: run-oracle.sh --manifest <path> --scenario <exact-id> --output <absolute-path>");
    }
    result[key.slice(2)] = value;
  }
  if (!result.manifest || !result.scenario || !result.output) {
    throw new Error("manifest, scenario and output are required");
  }
  return result;
}

function validateManifest(manifest, manifestPath) {
  if (manifest.schema !== "zlink-v11-oracle-manifest-v1") throw new Error("unsupported oracle manifest schema");
  if (manifest.artifact?.readOnly !== true || manifest.artifact?.version !== "10.x-frozen") {
    throw new Error("oracle artifact must be a read-only frozen 10.x artifact");
  }
  if (manifest.archivePolicy?.eligibleForActiveBaseline !== false
      || manifest.archivePolicy?.excludedFromCandidateBuildLinkLoad !== true) {
    throw new Error("oracle archive exclusion policy is not fail-closed");
  }
  if (!Array.isArray(manifest.scenarios) || manifest.scenarios.length < 2) {
    throw new Error("oracle manifest must contain representative scenarios");
  }
  const outcomes = new Set(manifest.scenarios.map((item) => item.outcome));
  if (!outcomes.has("success") || !outcomes.has("error")) {
    throw new Error("oracle manifest must contain success and error traces");
  }
  for (const scenario of manifest.scenarios) {
    if (!/^V11-E2E-(?:M\d{2}|L\d{2})$/.test(scenario.id)
        || scenario.disposition !== "verified-baseline"
        || !Array.isArray(scenario.expectedTests)
        || scenario.expectedTests.length === 0) {
      throw new Error(`invalid frozen scenario: ${scenario.id ?? "<missing>"}`);
    }
    const executablePath = resolve(dirname(manifestPath), scenario.runner?.path ?? "");
    if (!executablePath.includes("/core/build/bin/") || sha256(executablePath) !== scenario.runner.sha256) {
      throw new Error(`oracle executable SHA-256 does not match frozen manifest: ${scenario.id}`);
    }
  }
  const childPath = resolve(dirname(manifestPath), manifest.artifact.child.path);
  if (childPath !== join(dirname(manifestPath), "oracle-child.mjs")) {
    throw new Error("oracle child must be the sibling frozen artifact");
  }
  if (sha256(childPath) !== manifest.artifact.child.sha256) {
    throw new Error("oracle child SHA-256 does not match frozen manifest");
  }
  return childPath;
}

function run({ manifest: manifestArg, scenario: scenarioId, output }) {
  if (!isAbsolute(output)) throw new Error("output path must be absolute");
  const manifestPath = resolve(manifestArg);
  const manifest = JSON.parse(readFileSync(manifestPath, "utf8"));
  const childPath = validateManifest(manifest, manifestPath);
  if (!manifest.scenarios.some((item) => item.id === scenarioId)) {
    throw new Error(`scenario is not frozen in the manifest: ${scenarioId}`);
  }

  const token = randomBytes(24).toString("hex");
  const child = spawnSync(process.execPath, [childPath, manifestPath, scenarioId], {
    encoding: "utf8",
    env: { ...process.env, ZLINK_V11_ORACLE_CHILD_TOKEN: token },
    stdio: ["ignore", "pipe", "pipe"]
  });
  if (child.status !== 0) throw new Error(`oracle child failed (${child.status}): ${child.stderr.trim()}`);
  const envelope = JSON.parse(child.stdout);
  if (envelope.protocol !== "zlink-v11-oracle-child-v1" || envelope.token !== token) {
    throw new Error("oracle child protocol validation failed");
  }
  writeFileSync(output, `${JSON.stringify(envelope.trace, null, 2)}\n`, { flag: "wx" });
}

function selfTest() {
  const manifestPath = join(here, "oracle-manifest-v1.json");
  const work = mkdtempSync(join(tmpdir(), "zlink-v11-oracle-"));
  try {
    for (const scenario of ["V11-E2E-M17", "V11-E2E-M12"]) {
      const output = join(work, `${scenario}.json`);
      run({ manifest: manifestPath, scenario, output });
      const trace = JSON.parse(readFileSync(output, "utf8"));
      if (trace.scenarioId !== scenario || trace.provenance.oracleVersion !== "10.x-frozen") {
        throw new Error(`self-test trace mismatch: ${scenario}`);
      }
    }
    const manifest = JSON.parse(readFileSync(manifestPath, "utf8"));
    const executableTamper = structuredClone(manifest);
    executableTamper.scenarios[0].runner.sha256 = "0".repeat(64);
    let executableRejected = false;
    try { validateManifest(executableTamper, manifestPath); }
    catch (error) { executableRejected = error.message.includes("executable SHA-256"); }
    if (!executableRejected) throw new Error("self-test expected executable provenance rejection");

    manifest.archivePolicy.eligibleForActiveBaseline = true;
    const badManifest = join(work, "bad-manifest.json");
    writeFileSync(badManifest, JSON.stringify(manifest));
    let rejected = false;
    try { run({ manifest: badManifest, scenario: "V11-E2E-M17", output: join(work, "bad.json") }); }
    catch (error) { rejected = error.message.includes("archive exclusion"); }
    if (!rejected) throw new Error("self-test expected archive-policy rejection");
    process.stdout.write("V11 oracle self-test passed: child protocol, normalized traces, provenance, and archive exclusion\n");
  } finally {
    rmSync(work, { recursive: true, force: true });
  }
}

try {
  const args = parseArgs(process.argv.slice(2));
  if (args.selfTest) selfTest(); else run(args);
} catch (error) {
  fail(error.message);
}
