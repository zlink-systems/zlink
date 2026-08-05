#!/usr/bin/env node

import { createHash } from "node:crypto";
import { existsSync, lstatSync, readFileSync, readlinkSync } from "node:fs";
import { isAbsolute, join, resolve } from "node:path";
import { spawnSync } from "node:child_process";

const SHA256 = /^[0-9a-f]{64}$/;
const REVISION = /^[0-9a-f]{40}$/;
const expectedLedgerId = "V11-M3-CORE-VERIFY";
const candidateKeys = new Set([
  "schema", "ledgerId", "baseRevision", "ownedPaths", "directInputs",
  "pathCount", "aggregateSha256", "files",
]);
const directInputKeys = new Set(["path", "contentSha256"]);
const fileKeys = new Set([
  "path", "status", "mode", "contentSha256", "baseContentSha256",
]);

function fail(message) {
  throw new Error(message);
}

function assert(condition, message) {
  if (!condition) fail(message);
}

function shaBytes(value) {
  return createHash("sha256").update(value).digest("hex");
}

function fileBytes(path) {
  const stat = lstatSync(path);
  return stat.isSymbolicLink() ? Buffer.from(readlinkSync(path), "utf8") : readFileSync(path);
}

function git(repoRoot, args, encoding = "utf8") {
  const result = spawnSync("git", args, {
    cwd: repoRoot,
    encoding,
    maxBuffer: 128 * 1024 * 1024,
  });
  assert(result.status === 0,
    `git ${args.join(" ")} exited ${result.status}: ${String(result.stderr).trim()}`);
  return result.stdout;
}

function safeRelative(path) {
  return typeof path === "string" && path.length > 0 && !isAbsolute(path)
    && !path.includes("\\") && !path.split("/").includes("..");
}

function exactKeys(value, allowed, label) {
  assert(value && typeof value === "object" && !Array.isArray(value), `${label} must be an object`);
  for (const key of Object.keys(value))
    assert(allowed.has(key), `${label} contains unknown key: ${key}`);
  for (const key of allowed)
    assert(Object.hasOwn(value, key), `${label} is missing key: ${key}`);
}

function insideOwnedPath(path, ownedPaths) {
  return ownedPaths.some(owner => path === owner || path.startsWith(`${owner}/`));
}

function parseArgs(argv) {
  const result = {};
  for (let i = 0; i < argv.length; i += 2) {
    const key = argv[i];
    const value = argv[i + 1];
    assert(value && ["--candidate-manifest", "--review-evidence", "--repo-root"].includes(key),
      "usage: verify-candidate.mjs --candidate-manifest ABSOLUTE_JSON --review-evidence ABSOLUTE_JSON --repo-root ABSOLUTE_REPOSITORY");
    result[key.slice(2)] = value;
  }
  assert(isAbsolute(result["candidate-manifest"] ?? ""), "--candidate-manifest must be absolute");
  assert(isAbsolute(result["review-evidence"] ?? ""), "--review-evidence must be absolute");
  assert(isAbsolute(result["repo-root"] ?? ""), "--repo-root must be absolute");
  return result;
}

function main() {
  const args = parseArgs(process.argv.slice(2));
  const repoRoot = resolve(args["repo-root"]);
  const candidatePath = resolve(args["candidate-manifest"]);
  const reviewEvidencePath = resolve(args["review-evidence"]);
  assert(existsSync(candidatePath), `candidate manifest is missing: ${candidatePath}`);
  assert(existsSync(reviewEvidencePath), `review evidence is missing: ${reviewEvidencePath}`);

  const bytes = readFileSync(candidatePath);
  const candidate = JSON.parse(bytes.toString("utf8"));
  exactKeys(candidate, candidateKeys, "candidate manifest");
  assert(candidate.schema === "zlink-v11-ledger-candidate-v1", "unsupported candidate schema");
  assert(candidate.ledgerId === expectedLedgerId,
    `candidate ledgerId must be ${expectedLedgerId}, found: ${candidate.ledgerId ?? "<missing>"}`);
  assert(REVISION.test(candidate.baseRevision ?? ""), "candidate baseRevision is invalid");
  git(repoRoot, ["cat-file", "-e", `${candidate.baseRevision}^{commit}`]);
  const head = git(repoRoot, ["rev-parse", "HEAD"]).trim();

  assert(Array.isArray(candidate.ownedPaths) && candidate.ownedPaths.includes("core"),
    "candidate must own the core tree");
  assert(candidate.ownedPaths.every(safeRelative), "candidate contains an unsafe owned path");
  assert(new Set(candidate.ownedPaths).size === candidate.ownedPaths.length,
    "candidate ownedPaths contains duplicates");
  assert(JSON.stringify(candidate.ownedPaths) === JSON.stringify([...candidate.ownedPaths].sort()),
    "candidate ownedPaths must be sorted");
  assert(Array.isArray(candidate.directInputs), "candidate directInputs must be an array");
  assert(JSON.stringify(candidate.directInputs.map(input => input.path))
    === JSON.stringify(candidate.directInputs.map(input => input.path).sort()),
  "candidate directInputs must be sorted");
  for (const input of candidate.directInputs) {
    exactKeys(input, directInputKeys, `candidate direct input ${input?.path}`);
    assert(safeRelative(input?.path), `unsafe candidate direct input: ${input?.path}`);
    assert(SHA256.test(input.contentSha256 ?? ""),
      `candidate direct input hash is invalid: ${input.path}`);
  }

  assert(Array.isArray(candidate.files) && candidate.files.length > 0,
    "candidate files must not be empty");
  assert(candidate.pathCount === candidate.files.length, "candidate pathCount mismatch");
  assert(JSON.stringify(candidate.files.map(file => file.path))
    === JSON.stringify(candidate.files.map(file => file.path).sort()),
  "candidate files must be sorted");
  assert(new Set(candidate.files.map(file => file.path)).size === candidate.files.length,
    "candidate files contains duplicates");
  assert(SHA256.test(candidate.aggregateSha256 ?? "")
    && shaBytes(JSON.stringify(candidate.files)) === candidate.aggregateSha256,
  "candidate aggregateSha256 mismatch");

  const declaredCore = new Set();
  for (const file of candidate.files) {
    exactKeys(file, fileKeys, `candidate file ${file?.path}`);
    assert(safeRelative(file?.path), `unsafe candidate file path: ${file?.path}`);
    assert(insideOwnedPath(file.path, candidate.ownedPaths),
      `candidate file is outside ownedPaths: ${file.path}`);
    assert(["added", "modified", "deleted", "observed"].includes(file.status),
      `invalid candidate file status: ${file.path}`);
    const absolute = join(repoRoot, file.path);
    if (file.status === "deleted") {
      assert(!existsSync(absolute), `deleted candidate path exists: ${file.path}`);
      assert(file.contentSha256 === null && file.mode === null,
        `deleted candidate path must have null current metadata: ${file.path}`);
    } else {
      assert(existsSync(absolute), `candidate path is missing: ${file.path}`);
      const mode = `100${(lstatSync(absolute).mode & 0o777).toString(8).padStart(3, "0")}`;
      assert(file.mode === mode, `candidate mode drift: ${file.path}`);
      assert(SHA256.test(file.contentSha256 ?? "")
        && shaBytes(fileBytes(absolute)) === file.contentSha256,
      `candidate content drift: ${file.path}`);
    }
    if (["modified", "deleted"].includes(file.status)) {
      assert(SHA256.test(file.baseContentSha256 ?? ""),
        `candidate base hash is missing: ${file.path}`);
      const baseBytes = git(repoRoot, ["show", `${candidate.baseRevision}:${file.path}`], null);
      assert(shaBytes(baseBytes) === file.baseContentSha256,
        `candidate base hash mismatch: ${file.path}`);
    } else {
      assert(file.baseContentSha256 === null,
        `added or observed candidate path must have null base hash: ${file.path}`);
    }
    if (file.path === "core" || file.path.startsWith("core/"))
      declaredCore.add(file.path);
  }

  const changedCore = new Set(git(repoRoot,
    ["diff", "--no-renames", "--name-only", candidate.baseRevision, "--", "core"])
    .trim().split("\n").filter(Boolean));
  for (const path of git(repoRoot,
    ["ls-files", "--others", "--exclude-standard", "--", "core"]).trim().split("\n").filter(Boolean))
    changedCore.add(path);
  for (const path of changedCore)
    assert(declaredCore.has(path), `changed core path is missing from candidate: ${path}`);
  for (const path of declaredCore) {
    const record = candidate.files.find(file => file.path === path);
    if (record.status !== "observed")
      assert(changedCore.has(path), `candidate declares unchanged core path as changed: ${path}`);
  }

  const reviewBytes = readFileSync(reviewEvidencePath);
  const review = JSON.parse(reviewBytes.toString("utf8"));
  assert(review.schema === "zlink-v11-ledger-evidence-v1",
    "review evidence schema must be zlink-v11-ledger-evidence-v1");
  assert(review.ledgerId === "V11-R2", "review evidence ledgerId must be V11-R2");
  assert(review.status === "passed", "review evidence must record passed status");
  const candidateManifestSha256 = shaBytes(bytes);
  assert(SHA256.test(review.candidateManifestSha256 ?? ""),
    "review evidence row candidate provenance is invalid");
  assert(review.details && typeof review.details === "object"
    && !Array.isArray(review.details), "review evidence details are missing");
  assert(review.details.approvedCandidateManifestSha256 === candidateManifestSha256,
    "review evidence does not approve the supplied candidate manifest SHA-256");

  const result = {
    schema: 1,
    ledgerId: candidate.ledgerId,
    baseRevision: candidate.baseRevision,
    worktreeHead: head,
    manifestPath: candidatePath,
    manifestSha256: candidateManifestSha256,
    aggregateSha256: candidate.aggregateSha256,
    coreChangedPathCount: changedCore.size,
    approval: {
      ledgerId: review.ledgerId,
      evidencePath: reviewEvidencePath,
      evidenceSha256: shaBytes(reviewBytes),
      candidateManifestSha256: review.details.approvedCandidateManifestSha256,
    },
  };
  process.stdout.write(`${JSON.stringify(result)}\n`);
}

try {
  main();
} catch (error) {
  process.stderr.write(`${error.message}\n`);
  process.exit(1);
}
