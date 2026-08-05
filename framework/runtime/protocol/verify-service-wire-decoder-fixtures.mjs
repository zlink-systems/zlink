#!/usr/bin/env node

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.dirname(fileURLToPath(import.meta.url));
const schema = JSON.parse(fs.readFileSync(path.join(root, "service-wire-v1.schema.json"), "utf8"));
const fixtures = JSON.parse(fs.readFileSync(path.join(root, "golden/service-decoder-fixtures-v1.json"), "utf8"));
const commands = new Map(schema.commands.map((entry) => [entry.id, entry]));
const frameworkErrorType = schema.types.find((entry) => entry.name === "framework-error-code");
const frameworkErrors = new Map(frameworkErrorType.values.map((entry) => [entry.value, entry]));
const terminalFailureIntegrity = schema.semanticConstraints.find(
  (entry) => entry.kind === "terminal-failure-integrity",
);

function fail(code) {
  const error = new Error(code);
  error.code = code;
  throw error;
}

function decode(bytes) {
  if (bytes.length < schema.protocol.headPrefixBytes) fail("truncated-head");
  if (bytes[0] !== schema.protocol.magic[0] || bytes[1] !== schema.protocol.magic[1]) fail("invalid-magic");
  if (bytes[2] !== schema.protocol.wireMajor) fail("invalid-major");
  const command = commands.get(bytes[3]);
  if (!command) fail("unknown-command");
  const flags = bytes[4];
  const allowed = command.allowedFlags.reduce((value, name) => {
    const flag = schema.flags.find((entry) => entry.name === name);
    return value | flag.bit;
  }, 0);
  if ((flags & ~allowed) !== 0) fail("forbidden-flag");
  if (command.name === "livenessProbe" || command.name === "livenessAck") {
    if (bytes.length < 13) fail("truncated-field");
    if (bytes.length > 13) fail("trailing-byte");
    let probeId = 0n;
    for (const byte of bytes.slice(5)) probeId = (probeId << 8n) | BigInt(byte);
    if (probeId === 0n) fail("invalid-field");
    return { command: command.name, probeId };
  }
  return { command: command.name };
}

function decodeFrameworkErrorCode(wireValue) {
  const entry = frameworkErrors.get(wireValue);
  if (!entry) fail("unknown-framework-error");
  if (entry.name === "none") return { name: entry.name, publicValue: null };
  if (terminalFailureIntegrity.publicMapping !== "wire-value-minus-one") {
    fail("unsupported-framework-error-mapping");
  }
  return { name: entry.name, publicValue: wireValue - 1 };
}

for (const fixture of fixtures.canonical) {
  const decoded = decode(fixture.bytes);
  if (decoded.command !== fixture.name || decoded.probeId.toString() !== fixtures.probeId) {
    throw new Error(`canonical fixture mismatch: ${fixture.name}`);
  }
}
for (const fixture of fixtures.malformed) {
  try {
    decode(fixture.bytes);
    throw new Error(`malformed fixture was accepted: ${fixture.name}`);
  } catch (error) {
    if (error.code !== fixture.error) throw error;
  }
}
const probe = decode(fixtures.canonical.find((entry) => entry.name === "livenessProbe").bytes);
const ack = decode(fixtures.canonical.find((entry) => entry.name === "livenessAck").bytes);
if (probe.probeId !== ack.probeId) throw new Error("livenessAck does not echo livenessProbe id");

if (fixtures.frameworkErrors.publicMapping !== terminalFailureIntegrity.publicMapping
    || JSON.stringify(fixtures.frameworkErrors.reservedWireValues)
      !== JSON.stringify(terminalFailureIntegrity.reservedWireValues)) {
  throw new Error("framework error mapping fixture does not match schema");
}
for (const fixture of fixtures.frameworkErrors.canonical) {
  const decoded = decodeFrameworkErrorCode(fixture.wireValue);
  if (decoded.name !== fixture.name || decoded.publicValue !== fixture.publicValue) {
    throw new Error(`framework error fixture mismatch: ${fixture.name}`);
  }
}
for (const fixture of fixtures.frameworkErrors.malformed) {
  try {
    decodeFrameworkErrorCode(fixture.wireValue);
    throw new Error(`malformed framework error fixture was accepted: ${fixture.name}`);
  } catch (error) {
    if (error.code !== fixture.error) throw error;
  }
}
const relocationDataLost = fixtures.frameworkErrors.canonical.find(
  (entry) => entry.name === "relocationDataLost",
);
if (relocationDataLost?.wireValue !== 35 || relocationDataLost.publicValue !== 34) {
  throw new Error("RelocationDataLost must decode from wire 35 to public 34");
}

console.log(
  `service wire decoder fixtures valid: canonical=${fixtures.canonical.length} `
    + `malformed=${fixtures.malformed.length} frameworkErrors=${fixtures.frameworkErrors.canonical.length} `
    + `frameworkErrorMalformed=${fixtures.frameworkErrors.malformed.length} probeEcho=pass `
    + `RelocationDataLost=wire35/public34`,
);
