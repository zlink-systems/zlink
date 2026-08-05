#!/usr/bin/env node

import childProcess from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';

function fail(message) {
  throw new Error(message);
}

function sha256(file) {
  return crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
}

function requireAbsolute(value, name) {
  if (!value || !path.isAbsolute(value)) fail(`${name} must be an absolute path`);
  if (value.split(path.sep).includes('..') || value.split(path.sep).includes('.')) {
    fail(`${name} must not contain dot path segments`);
  }
  return fs.realpathSync(value);
}

function parseArguments(argv) {
  const values = {};
  for (let index = 0; index < argv.length; index += 2) {
    const name = argv[index];
    const value = argv[index + 1];
    if (!['--prefix', '--core-package-evidence'].includes(name) || value === undefined) {
      fail('Usage: verify-core-input.mjs --prefix ABSOLUTE_DIR --core-package-evidence ABSOLUTE_JSON');
    }
    values[name.slice(2)] = value;
  }
  return values;
}

function sameCandidate(actual, expected) {
  return actual?.ledgerId === expected?.ledgerId
    && actual?.baseRevision === expected?.baseRevision
    && actual?.manifestSha256 === expected?.manifestSha256
    && actual?.aggregateSha256 === expected?.aggregateSha256;
}

const args = parseArguments(process.argv.slice(2));
const prefix = requireAbsolute(args.prefix, '--prefix');
const evidencePath = requireAbsolute(args['core-package-evidence'], '--core-package-evidence');

//  The expected Core version comes from the repository VERSION file so that a
//  Core bump does not require editing this validator.
const repoRoot = path.resolve(path.dirname(new URL(import.meta.url).pathname), '../../..');
const versionFile = fs.readFileSync(path.join(repoRoot, 'VERSION'), 'utf8');
const expectedCoreVersion = versionFile.match(/^LIBZLINK_VERSION=(.+)$/m)?.[1]?.trim();
if (!/^11\.[0-9]+\.[0-9]+$/.test(expectedCoreVersion ?? '')) {
  fail('VERSION must declare LIBZLINK_VERSION as 11.x.y');
}
if (!fs.statSync(prefix).isDirectory()) fail('--prefix must name a directory');
if (!fs.statSync(evidencePath).isFile()) fail('--core-package-evidence must name a file');

const evidence = JSON.parse(fs.readFileSync(evidencePath, 'utf8'));
if (evidence.schema !== 1 || evidence.ledgerId !== 'V11-M3-CORE-PKG'
    || evidence.command !== 'CORE-PKG' || evidence.status !== 'pass') {
  fail('Core package evidence is not a passed V11-M3-CORE-PKG result');
}
if (evidence.version !== expectedCoreVersion) fail(`Core package evidence must report version ${expectedCoreVersion}`);
if (fs.realpathSync(evidence.output?.prefix ?? '') !== prefix) {
  fail('Core package prefix does not match the approved evidence');
}

const provenancePath = path.join(prefix, 'share/zlink/core-package-provenance.json');
if (!fs.statSync(provenancePath).isFile()) fail('Core package provenance is missing');
const provenanceSha256 = sha256(provenancePath);
if (provenanceSha256 !== evidence.output?.provenanceSha256
    || provenanceSha256 !== evidence.consumer?.provenance?.sha256) {
  fail('Core package provenance SHA-256 does not match the approved evidence');
}
const provenance = JSON.parse(fs.readFileSync(provenancePath, 'utf8'));
if (provenance.schema !== 1 || provenance.package !== 'zlink-core'
    || provenance.version !== expectedCoreVersion) {
  fail(`Core package provenance must identify zlink-core ${expectedCoreVersion}`);
}
if (!sameCandidate(provenance.candidate, evidence.candidate)
    || !sameCandidate(provenance.candidate, evidence.consumer?.candidate)
    || provenance.candidate?.approvalEvidenceSha256 !== evidence.approval?.evidenceSha256
    || provenance.candidate?.approvalEvidenceSha256 !== evidence.consumer?.approval?.evidenceSha256
    || evidence.approval?.candidateManifestSha256 !== evidence.candidate?.manifestSha256) {
  fail('Core package candidate identity does not match the approved evidence');
}

const runtimePath = fs.realpathSync(path.join(prefix, 'lib/libzlink.so'));
const relativeRuntime = path.relative(prefix, runtimePath);
if (relativeRuntime.startsWith('..') || path.isAbsolute(relativeRuntime)) {
  fail('Core runtime resolves outside the approved package prefix');
}
const runtimeSha256 = sha256(runtimePath);
const runtimeRecord = provenance.files?.find(record => record.path === `lib/libzlink.so.${expectedCoreVersion}`);
if (!runtimeRecord || runtimeRecord.sha256 !== runtimeSha256
    || evidence.consumer?.runtime?.sha256 !== runtimeSha256
    || evidence.consumer?.runtime?.version !== expectedCoreVersion) {
  fail('Core runtime SHA-256 or version does not match the approved package');
}
const sonameOutput = childProcess.execFileSync('readelf', ['-d', runtimePath], {encoding: 'utf8'});
const soname = sonameOutput.match(/\(SONAME\).*\[([^\]]+)\]/)?.[1];
if (soname !== 'libzlink.so.11' || evidence.consumer?.runtime?.soname !== soname) {
  fail('Core runtime SONAME does not match libzlink.so.11');
}
if (fs.existsSync(path.join(prefix, 'include/zlink/service'))) {
  fail('Core package contains the removed service headers');
}

process.stdout.write(`${JSON.stringify({
  prefix,
  version: provenance.version,
  provenancePath,
  provenanceSha256,
  candidate: {
    ledgerId: provenance.candidate.ledgerId,
    baseRevision: provenance.candidate.baseRevision,
    manifestSha256: provenance.candidate.manifestSha256,
    aggregateSha256: provenance.candidate.aggregateSha256,
    approvalEvidenceSha256: provenance.candidate.approvalEvidenceSha256,
  },
  runtime: {path: runtimePath, sha256: runtimeSha256, soname},
}, null, 2)}\n`);
