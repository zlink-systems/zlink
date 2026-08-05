#!/usr/bin/env node
import { createHash } from "node:crypto";
import {
  chmodSync,
  copyFileSync,
  existsSync,
  lstatSync,
  mkdirSync,
  readFileSync,
  readlinkSync,
  rmSync,
  symlinkSync,
} from "node:fs";
import path from "node:path";

const [candidatePath, repoRoot, snapshotRoot] = process.argv.slice(2);
if (!candidatePath || !repoRoot || !snapshotRoot) {
  throw new Error("usage: materialize-build-source.mjs CANDIDATE REPO_ROOT SNAPSHOT_ROOT");
}

const candidate = JSON.parse(readFileSync(candidatePath, "utf8"));
if (candidate.schema !== "zlink-v11-ledger-candidate-v1"
    || candidate.ledgerId !== "V11-M3-CORE-VERIFY"
    || !Array.isArray(candidate.files)) {
  throw new Error("invalid Core candidate manifest");
}

const digest = value => createHash("sha256").update(value).digest("hex");
const bytes = file => {
  const stat = lstatSync(file);
  return stat.isSymbolicLink()
    ? Buffer.from(readlinkSync(file), "utf8")
    : readFileSync(file);
};

for (const record of candidate.files) {
  if (record.path !== "VERSION" && !record.path.startsWith("core/")) continue;
  const source = path.join(repoRoot, ...record.path.split("/"));
  const target = path.join(snapshotRoot, ...record.path.split("/"));
  if (record.status === "deleted") {
    rmSync(target, { force: true, recursive: true });
    continue;
  }
  if (!existsSync(source) || digest(bytes(source)) !== record.contentSha256) {
    throw new Error(`candidate source changed while materializing: ${record.path}`);
  }
  mkdirSync(path.dirname(target), { recursive: true });
  rmSync(target, { force: true, recursive: true });
  const sourceStat = lstatSync(source);
  if (sourceStat.isSymbolicLink()) {
    symlinkSync(readlinkSync(source), target);
  } else {
    copyFileSync(source, target);
    chmodSync(target, Number.parseInt(record.mode.slice(-3), 8));
  }
  if (digest(bytes(target)) !== record.contentSha256) {
    throw new Error(`materialized candidate hash mismatch: ${record.path}`);
  }
}

console.log(`materialized Core candidate ${candidate.baseRevision}`);
