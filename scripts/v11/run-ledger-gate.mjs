#!/usr/bin/env node

import { createHash } from "node:crypto";
import { existsSync, lstatSync, mkdtempSync, readFileSync, readlinkSync, rmSync, writeFileSync } from "node:fs";
import { dirname, isAbsolute, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { tmpdir } from "node:os";
import { spawnSync } from "node:child_process";

const SHA = /^[0-9a-f]{64}$/;
const REVISION = /^[0-9a-f]{40}$/;
const CANDIDATE_KEYS = new Set(["schema", "ledgerId", "baseRevision", "ownedPaths", "directInputs", "pathCount", "aggregateSha256", "files"]);
const FILE_KEYS = new Set(["path", "status", "mode", "contentSha256", "baseContentSha256"]);
const DIRECT_INPUT_KEYS = new Set(["path", "contentSha256"]);
const OWNED_KEYS = new Set(["schema", "ledgerId", "ownedPaths", "contentSha256"]);
const EVIDENCE_KEYS = new Set(["schema", "ledgerId", "status", "sourceRevision", "candidateManifestSha256", "ownedPathManifestSha256", "commands", "completedAt", "details", "issues"]);
const here = dirname(fileURLToPath(import.meta.url));

function shaBytes(value) { return createHash("sha256").update(value).digest("hex"); }
function shaFile(path) { return shaBytes(readFileSync(path)); }
function readJson(path) { return JSON.parse(readFileSync(path, "utf8")); }
function assert(condition, message) { if (!condition) throw new Error(message); }
function exactKeys(value, allowed, label) {
  for (const key of Object.keys(value)) assert(allowed.has(key), `${label} contains unknown key: ${key}`);
}
function safePath(path) {
  return typeof path === "string" && path.length > 0 && !isAbsolute(path)
    && !path.split(/[\\/]/).includes("..") && !path.includes("\\");
}
function inside(path, owner) { return path === owner || path.startsWith(`${owner}/`); }
function fileBytes(path) {
  const stat = lstatSync(path);
  return stat.isSymbolicLink() ? Buffer.from(readlinkSync(path), "utf8") : readFileSync(path);
}
function fileMode(path) { return (lstatSync(path).mode & 0o777).toString(8).padStart(6, "0"); }
function git(cwd, args, expected = 0) {
  const result = spawnSync("git", args, { cwd, encoding: "utf8", maxBuffer: 128 * 1024 * 1024 });
  assert(result.status === expected, `git ${args.join(" ")} exited ${result.status}: ${result.stderr.trim()}`);
  return result.stdout;
}
function gitBytes(cwd, args) {
  const result = spawnSync("git", args, { cwd, encoding: null, maxBuffer: 128 * 1024 * 1024 });
  assert(result.status === 0, `git ${args.join(" ")} exited ${result.status}: ${result.stderr.toString("utf8").trim()}`);
  return result.stdout;
}
function schemaRef(root, ref) {
  assert(ref.startsWith("#/$defs/"), `unsupported schema reference: ${ref}`);
  return root.$defs[ref.slice("#/$defs/".length)];
}
function validateJsonSchema(value, schema, root = schema, label = "value") {
  root ??= schema;
  if (schema.$ref) return validateJsonSchema(value, schemaRef(root, schema.$ref), root, label);
  if (schema.anyOf) {
    const accepted = schema.anyOf.some((item) => { try { validateJsonSchema(value, item, root, label); return true; } catch { return false; } });
    assert(accepted, `${label} does not match any allowed schema`); return;
  }
  if (schema.const !== undefined) assert(value === schema.const, `${label} const mismatch`);
  if (schema.enum) assert(schema.enum.includes(value), `${label} enum mismatch`);
  if (schema.type) {
    const types = Array.isArray(schema.type) ? schema.type : [schema.type];
    const actual = value === null ? "null" : Array.isArray(value) ? "array" : Number.isInteger(value) ? "integer" : typeof value;
    assert(types.includes(actual) || (actual === "integer" && types.includes("number")), `${label} type mismatch`);
  }
  if (typeof value === "string") {
    if (schema.minLength !== undefined) assert(value.length >= schema.minLength, `${label} is too short`);
    if (schema.pattern) assert(new RegExp(schema.pattern).test(value), `${label} pattern mismatch`);
  }
  if (Number.isInteger(value) && schema.minimum !== undefined) assert(value >= schema.minimum, `${label} is below minimum`);
  if (Array.isArray(value)) {
    if (schema.minItems !== undefined) assert(value.length >= schema.minItems, `${label} has too few items`);
    if (schema.uniqueItems) assert(new Set(value.map((item) => JSON.stringify(item))).size === value.length, `${label} has duplicate items`);
    if (schema.items) value.forEach((item, index) => validateJsonSchema(item, schema.items, root, `${label}[${index}]`));
  }
  if (value && typeof value === "object" && !Array.isArray(value)) {
    for (const required of schema.required ?? []) assert(Object.hasOwn(value, required), `${label} missing required key: ${required}`);
    if (schema.additionalProperties === false) for (const key of Object.keys(value)) assert(Object.hasOwn(schema.properties ?? {}, key), `${label} unknown key: ${key}`);
    for (const [key, child] of Object.entries(schema.properties ?? {})) if (Object.hasOwn(value, key)) validateJsonSchema(value[key], child, root, `${label}.${key}`);
  }
}
function parseArgs(argv) {
  if (argv.length === 1 && argv[0] === "--self-test") return { selfTest: true };
  const result = {};
  for (let i = 0; i < argv.length; i += 2) {
    const key = argv[i];
    const value = argv[i + 1];
    assert(value && ["--id", "--candidate-manifest", "--owned-path-manifest", "--evidence"].includes(key),
      "usage: run-ledger-gate.sh --id <exact-id> --candidate-manifest <absolute.json> --owned-path-manifest <absolute.json> --evidence <absolute.json>");
    result[key.slice(2)] = value;
  }
  for (const key of ["id", "candidate-manifest", "owned-path-manifest", "evidence"]) assert(result[key], `missing --${key}`);
  for (const key of ["candidate-manifest", "owned-path-manifest", "evidence"]) assert(isAbsolute(result[key]), `--${key} must be absolute`);
  return result;
}
function validateOwned(value, id) {
  exactKeys(value, OWNED_KEYS, "owned-path manifest");
  assert(value.schema === "zlink-v11-owned-paths-v1", "unsupported owned-path schema");
  assert(value.ledgerId === id, "owned-path ledgerId mismatch");
  assert(Array.isArray(value.ownedPaths) && value.ownedPaths.length > 0, "ownedPaths must not be empty");
  assert(value.ownedPaths.every(safePath), "ownedPaths contains an unsafe path");
  assert(JSON.stringify([...value.ownedPaths].sort()) === JSON.stringify(value.ownedPaths), "ownedPaths must be sorted");
  assert(new Set(value.ownedPaths).size === value.ownedPaths.length, "ownedPaths contains duplicates");
  assert(value.contentSha256 === shaBytes(JSON.stringify(value.ownedPaths)), "ownedPaths contentSha256 mismatch");
}
function validateCandidate(value, id, owned, repoRoot) {
  exactKeys(value, CANDIDATE_KEYS, "candidate manifest");
  assert(value.schema === "zlink-v11-ledger-candidate-v1", "unsupported candidate schema");
  assert(value.ledgerId === id, "candidate ledgerId mismatch");
  assert(REVISION.test(value.baseRevision), "candidate baseRevision must be a full Git revision");
  git(repoRoot, ["cat-file", "-e", `${value.baseRevision}^{commit}`]);
  assert(JSON.stringify(value.ownedPaths) === JSON.stringify(owned.ownedPaths), "candidate and owned-path manifests disagree");
  assert(Array.isArray(value.directInputs), "candidate directInputs must be an array");
  assert(JSON.stringify(value.directInputs.map((input) => input.path)) === JSON.stringify(value.directInputs.map((input) => input.path).sort()), "candidate directInputs must be sorted");
  assert(new Set(value.directInputs.map((input) => input.path)).size === value.directInputs.length, "candidate directInputs contains duplicates");
  for (const input of value.directInputs) {
    exactKeys(input, DIRECT_INPUT_KEYS, `direct input ${input.path}`);
    assert(safePath(input.path) && existsSync(join(repoRoot, input.path)), `candidate direct input is missing or unsafe: ${input.path}`);
    assert(SHA.test(input.contentSha256) && input.contentSha256 === shaBytes(fileBytes(join(repoRoot, input.path))), `candidate direct input drift: ${input.path}`);
  }
  assert(Array.isArray(value.files) && value.files.length > 0, "candidate files must not be empty");
  assert(value.pathCount === value.files.length, "candidate pathCount mismatch");
  assert(JSON.stringify(value.files.map((file) => file.path)) === JSON.stringify(value.files.map((file) => file.path).sort()), "candidate files must be sorted");
  assert(new Set(value.files.map((file) => file.path)).size === value.files.length, "candidate files contains duplicates");
  for (const file of value.files) {
    exactKeys(file, FILE_KEYS, `candidate file ${file.path}`);
    assert(safePath(file.path), `candidate contains unsafe path: ${file.path}`);
    assert(owned.ownedPaths.some((owner) => inside(file.path, owner)), `candidate path is outside ownership: ${file.path}`);
    assert(["added", "modified", "deleted", "observed"].includes(file.status), `invalid candidate status: ${file.path}`);
    const absolute = join(repoRoot, file.path);
    if (file.status === "deleted") {
      assert(!existsSync(absolute), `deleted candidate path still exists: ${file.path}`);
      assert(file.mode === null && file.contentSha256 === null, `deleted candidate must have null mode and content hash: ${file.path}`);
    } else {
      assert(existsSync(absolute), `candidate path does not exist: ${file.path}`);
      assert(/^(100644|100755)$/.test(file.mode), `candidate mode is invalid: ${file.path}`);
      assert(file.mode === `100${fileMode(absolute).slice(-3)}`, `candidate mode drift: ${file.path}`);
      assert(SHA.test(file.contentSha256) && file.contentSha256 === shaBytes(fileBytes(absolute)), `candidate content drift: ${file.path}`);
    }
    if (["modified", "deleted"].includes(file.status)) {
      assert(SHA.test(file.baseContentSha256), `candidate base hash missing: ${file.path}`);
      const base = gitBytes(repoRoot, ["show", `${value.baseRevision}:${file.path}`]);
      assert(file.baseContentSha256 === shaBytes(base), `candidate base hash mismatch: ${file.path}`);
    } else {
      assert(file.baseContentSha256 === null, `added/observed candidate must have null base hash: ${file.path}`);
    }
  }
  assert(value.aggregateSha256 === shaBytes(JSON.stringify(value.files)), "candidate aggregateSha256 mismatch");

  const changed = new Set(git(repoRoot, ["diff", "--name-only", value.baseRevision, "--", ...owned.ownedPaths]).trim().split("\n").filter(Boolean));
  const untracked = git(repoRoot, ["ls-files", "--others", "--exclude-standard", "--", ...owned.ownedPaths]).trim().split("\n").filter(Boolean);
  for (const path of untracked) changed.add(path);
  const declared = new Set(value.files.filter((file) => file.status !== "observed").map((file) => file.path));
  for (const path of changed) assert(declared.has(path), `changed owned path is missing from candidate: ${path}`);
  const diff = spawnSync("git", ["diff", "--check", "--", ...owned.ownedPaths], { cwd: repoRoot, encoding: "utf8" });
  assert(diff.status === 0, `git diff --check failed: ${diff.stdout}${diff.stderr}`);
}
function validateEvidence(value, id, candidatePath, ownedPath, evidencePath, candidate, repoRoot) {
  exactKeys(value, EVIDENCE_KEYS, "evidence");
  assert(value.schema === "zlink-v11-ledger-evidence-v1", "unsupported evidence schema");
  assert(value.ledgerId === id && value.status === "passed", "evidence does not record a passed exact ledger ID");
  assert(REVISION.test(value.sourceRevision), "evidence sourceRevision must be a full Git revision");
  git(repoRoot, ["cat-file", "-e", `${value.sourceRevision}^{commit}`]);
  assert(value.candidateManifestSha256 === shaFile(candidatePath), "evidence candidate provenance mismatch");
  assert(value.ownedPathManifestSha256 === shaFile(ownedPath), "evidence owned-path provenance mismatch");
  assert(Array.isArray(value.commands) && value.commands.length > 0, "evidence commands must not be empty");
  for (const command of value.commands) {
    assert(typeof command.name === "string" && Number.isInteger(command.exitCode) && typeof command.required === "boolean", "invalid evidence command");
    assert(command.required, `passed evidence contains a non-required command: ${command.name}`);
    assert(command.exitCode === 0, `completion command failed: ${command.name}`);
  }
  assert(typeof value.completedAt === "string" && !Number.isNaN(Date.parse(value.completedAt)), "invalid evidence completedAt");
  const completedAt = Date.parse(value.completedAt);
  const candidateMtime = lstatSync(candidatePath).mtimeMs;
  const ownedMtime = lstatSync(ownedPath).mtimeMs;
  const evidenceMtime = lstatSync(evidencePath).mtimeMs;
  assert(evidenceMtime >= candidateMtime && evidenceMtime >= ownedMtime, "evidence file predates its manifests");
  assert(completedAt + 1000 >= candidateMtime && completedAt + 1000 >= ownedMtime, "evidence completedAt predates its manifests");
  assert(completedAt <= Date.now() + 300000, "evidence completedAt is in the future");
  for (const file of candidate.files) {
    if (file.status !== "deleted" && existsSync(join(repoRoot, file.path))) {
      const fileMtime = lstatSync(join(repoRoot, file.path)).mtimeMs;
      assert(candidateMtime >= fileMtime, `candidate manifest is older than candidate input: ${file.path}`);
      assert(evidenceMtime >= fileMtime && completedAt + 1000 >= fileMtime,
        `evidence is older than candidate input: ${file.path}`);
    }
  }
  for (const input of candidate.directInputs) {
    const inputMtime = lstatSync(join(repoRoot, input.path)).mtimeMs;
    assert(candidateMtime >= inputMtime, `candidate manifest is older than direct input: ${input.path}`);
    assert(evidenceMtime >= inputMtime && completedAt + 1000 >= inputMtime, `evidence is older than direct input: ${input.path}`);
  }
}
function runGate(args, repoRoot = process.cwd()) {
  const candidatePath = resolve(args["candidate-manifest"]);
  const ownedPath = resolve(args["owned-path-manifest"]);
  const evidencePath = resolve(args.evidence);
  const candidate = readJson(candidatePath);
  const owned = readJson(ownedPath);
  const evidence = readJson(evidencePath);
  validateJsonSchema(candidate, readJson(join(here, "schema/ledger-candidate-v1.schema.json")), undefined, "candidate");
  validateJsonSchema(owned, readJson(join(here, "schema/owned-paths-v1.schema.json")), undefined, "owned-path manifest");
  validateJsonSchema(evidence, readJson(join(here, "schema/ledger-evidence-v1.schema.json")), undefined, "evidence");
  validateOwned(owned, args.id);
  validateCandidate(candidate, args.id, owned, repoRoot);
  validateEvidence(evidence, args.id, candidatePath, ownedPath, evidencePath, candidate, repoRoot);
  process.stdout.write(`ledger gate passed: ${args.id} files=${candidate.pathCount} commands=${evidence.commands.length}\n`);
}
function selfTest() {
  const work = mkdtempSync(join(tmpdir(), "zlink-v11-ledger-gate-"));
  try {
    git(work, ["init", "-q"]);
    git(work, ["config", "user.email", "v11-gate@example.invalid"]);
    git(work, ["config", "user.name", "V11 Gate"]);
    writeFileSync(join(work, "owned.txt"), "base\n");
    git(work, ["add", "owned.txt"]); git(work, ["commit", "-qm", "base"]);
    const base = git(work, ["rev-parse", "HEAD"]).trim();
    writeFileSync(join(work, "owned.txt"), "candidate\n");
    const files = [{ path: "owned.txt", status: "modified", mode: "100644", contentSha256: shaFile(join(work, "owned.txt")), baseContentSha256: shaBytes("base\n") }];
    const owned = { schema: "zlink-v11-owned-paths-v1", ledgerId: "SELF-TEST", ownedPaths: ["owned.txt"] };
    owned.contentSha256 = shaBytes(JSON.stringify(owned.ownedPaths));
    const candidate = { schema: "zlink-v11-ledger-candidate-v1", ledgerId: "SELF-TEST", baseRevision: base, ownedPaths: owned.ownedPaths, directInputs: [], pathCount: 1, aggregateSha256: shaBytes(JSON.stringify(files)), files };
    const candidatePath = join(work, "candidate.json"); const ownedPath = join(work, "owned.json"); const evidencePath = join(work, "evidence.json");
    writeFileSync(candidatePath, `${JSON.stringify(candidate)}\n`); writeFileSync(ownedPath, `${JSON.stringify(owned)}\n`);
    const evidence = { schema: "zlink-v11-ledger-evidence-v1", ledgerId: "SELF-TEST", status: "passed", sourceRevision: base, candidateManifestSha256: shaFile(candidatePath), ownedPathManifestSha256: shaFile(ownedPath), commands: [{ name: "TEST", exitCode: 0, required: true }], completedAt: new Date().toISOString() };
    writeFileSync(evidencePath, `${JSON.stringify(evidence)}\n`);
    runGate({ id: "SELF-TEST", "candidate-manifest": candidatePath, "owned-path-manifest": ownedPath, evidence: evidencePath }, work);
    const badEvidence = { ...evidence, candidateManifestSha256: "0".repeat(64) };
    writeFileSync(evidencePath, `${JSON.stringify(badEvidence)}\n`);
    let rejected = false; try { runGate({ id: "SELF-TEST", "candidate-manifest": candidatePath, "owned-path-manifest": ownedPath, evidence: evidencePath }, work); } catch (error) { rejected = error.message.includes("provenance"); }
    assert(rejected, "self-test expected provenance rejection");
    const schemaMutation = { ...candidate, unexpected: true };
    let schemaRejected = false; try { validateJsonSchema(schemaMutation, readJson(join(here, "schema/ledger-candidate-v1.schema.json")), undefined, "candidate"); } catch (error) { schemaRejected = error.message.includes("unknown key"); }
    assert(schemaRejected, "self-test expected JSON schema mutation rejection");
    const ownedMutation = { ...owned, unexpected: true };
    let ownedSchemaRejected = false; try { validateJsonSchema(ownedMutation, readJson(join(here, "schema/owned-paths-v1.schema.json")), undefined, "owned"); } catch (error) { ownedSchemaRejected = error.message.includes("unknown key"); }
    assert(ownedSchemaRejected, "self-test expected owned-path schema mutation rejection");
    const evidenceMutation = { ...evidence }; delete evidenceMutation.completedAt;
    let evidenceSchemaRejected = false; try { validateJsonSchema(evidenceMutation, readJson(join(here, "schema/ledger-evidence-v1.schema.json")), undefined, "evidence"); } catch (error) { evidenceSchemaRejected = error.message.includes("missing required key"); }
    assert(evidenceSchemaRejected, "self-test expected evidence schema mutation rejection");
    const optionalMutation = { ...evidence, commands: [{ name: "TEST", exitCode: 1, required: false }] };
    let optionalRejected = false; try { validateJsonSchema(optionalMutation, readJson(join(here, "schema/ledger-evidence-v1.schema.json")), undefined, "evidence"); } catch (error) { optionalRejected = error.message.includes("const mismatch"); }
    assert(optionalRejected, "self-test expected optional failing completion command rejection");
    process.stdout.write("ledger gate self-test passed: canonical schema, ownership, hashes, freshness, required exits, and negative mutation\n");
  } finally { rmSync(work, { recursive: true, force: true }); }
}

try { const args = parseArgs(process.argv.slice(2)); if (args.selfTest) selfTest(); else runGate(args); }
catch (error) { process.stderr.write(`${error.message}\n`); process.exitCode = 2; }
