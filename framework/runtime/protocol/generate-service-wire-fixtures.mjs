#!/usr/bin/env node

import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";
import {
  encodeGoldenBody,
  validateGoldenFixtures,
  validateSchema,
  validateServiceWireFixtureOracles,
} from "./validate-service-wire-schema.mjs";

const scriptPath = fileURLToPath(import.meta.url);
const protocolDirectory = path.dirname(scriptPath);
const indexPath = path.join(protocolDirectory, "generated", "fixtures", "index.json");

function readJson(filePath) {
  return JSON.parse(fs.readFileSync(filePath, "utf8"));
}

function sha256(filePath) {
  return crypto.createHash("sha256").update(fs.readFileSync(filePath)).digest("hex");
}

function resolveRepositoryPath(relativePath, label) {
  if (typeof relativePath !== "string" || path.isAbsolute(relativePath)) {
    throw new Error(`${label} must be a repository-relative path`);
  }
  const resolved = path.resolve(protocolDirectory, relativePath);
  const relativeToRoot = path.relative(protocolDirectory, resolved);
  if (relativeToRoot.startsWith("..") || path.isAbsolute(relativeToRoot)) {
    throw new Error(`${label} escapes the protocol directory: ${relativePath}`);
  }
  return resolved;
}

function fixturePath(relativePath) {
  return resolveRepositoryPath(relativePath, "goldenFixture");
}

function assertEncodedBody(formatName, fixture) {
  if (!fixture || typeof fixture.decoded !== "object" || fixture.decoded === null) {
    throw new Error(`fixture:${formatName}: decoded case is required`);
  }
  const body = encodeGoldenBody(formatName, fixture.decoded);
  if (!Buffer.isBuffer(body)) {
    throw new Error(`fixture:${formatName}: encoder did not return bytes`);
  }
}

function pointerCase(name, pointers) {
  return { name, pointers };
}

function canonicalCases(fixture) {
  if (typeof fixture.encodedHex === "string") {
    return [pointerCase("canonical", {
      encodedHex: "/encodedHex",
      decoded: "/decoded",
    })];
  }
  if (typeof fixture.logicalHex === "string") {
    return [pointerCase("canonical", {
      logicalHex: "/logicalHex",
      decoded: "/decoded",
    })];
  }
  if (fixture.canonical !== undefined) {
    return [pointerCase(fixture.canonical.name ?? "canonical", {
      hex: "/canonical/hex",
      decoded: "/canonical/decoded",
    })];
  }
  if (Array.isArray(fixture.valid)) {
    return fixture.valid.map((entry, index) => pointerCase(entry.name, {
      input: `/valid/${index}/input`,
      framesHex: `/valid/${index}/framesHex`,
    }));
  }
  throw new Error(`fixture:${fixture.format}: canonical cases are missing`);
}

function malformedCases(fixture) {
  if (Array.isArray(fixture.malformed)) {
    return fixture.malformed.map((entry, index) => ({
      name: entry.name,
      error: entry.error,
      pointers: {
        hex: `/malformed/${index}/hex`,
      },
    }));
  }
  if (Array.isArray(fixture.invalid)) {
    return fixture.invalid.map((entry, index) => ({
      name: entry.name,
      error: entry.error,
      pointers: {
        framesHex: `/invalid/${index}/framesHex`,
      },
    }));
  }
  return [];
}

function surface(format, type, command, commandId) {
  return {
    format,
    type: type ?? null,
    command: command ?? null,
    commandId: commandId ?? null,
  };
}

function makeEntry(kind, goldenFixture, schemaSurface, fixture) {
  if (kind !== "command") {
    assertEncodedBody(schemaSurface.format, fixture);
  }
  const sourcePath = fixturePath(goldenFixture);
  return {
    kind,
    goldenFixture,
    goldenSha256: sha256(sourcePath),
    surface: schemaSurface,
    canonical: canonicalCases(fixture),
    malformed: malformedCases(fixture),
  };
}

function buildIndex(schema, schemaPath) {
  validateSchema(schema);
  validateGoldenFixtures(schema, schemaPath);
  const oracles = validateServiceWireFixtureOracles(schema, schemaPath);
  const fixtures = [];

  for (const format of schema.durableFormats) {
    const fixture = readJson(fixturePath(format.goldenFixture));
    fixtures.push(makeEntry(
      "durable",
      format.goldenFixture,
      surface(format.name, format.body?.$ref),
      fixture,
    ));
  }

  const logical = oracles.logical;
  const logicalFixture = readJson(fixturePath(logical.goldenFixture));
  fixtures.push(makeEntry(
    "logical",
    logical.goldenFixture,
    surface(logical.format, logical.type),
    logicalFixture,
  ));

  for (const commandFixture of [...oracles.commands].sort((left, right) => (
    left.commandId - right.commandId
  ))) {
    const fixture = readJson(fixturePath(commandFixture.goldenFixture));
    fixtures.push(makeEntry(
      "command",
      commandFixture.goldenFixture,
      surface(
        commandFixture.format,
        null,
        commandFixture.command,
        commandFixture.commandId,
      ),
      fixture,
    ));
  }

  return {
    schema: "service-wire-v1",
    version: 1,
    fixtures,
  };
}

function renderIndex(index) {
  return `${JSON.stringify(index, null, 2)}\n`;
}

function validateIndexReferences(index) {
  if (index === null || typeof index !== "object" || !Array.isArray(index.fixtures)) {
    throw new Error("fixture index must contain a fixtures array");
  }
  for (const [indexNumber, entry] of index.fixtures.entries()) {
    const location = `fixture index fixtures[${indexNumber}]`;
    const sourcePath = fixturePath(entry.goldenFixture);
    if (!fs.existsSync(sourcePath)) {
      throw new Error(`${location}: golden file does not exist: ${entry.goldenFixture}`);
    }
    if (typeof entry.goldenSha256 !== "string") {
      throw new Error(`${location}: goldenSha256 is required`);
    }
    const actualHash = sha256(sourcePath);
    if (actualHash !== entry.goldenSha256) {
      throw new Error(
        `${location}: golden hash mismatch for ${entry.goldenFixture}`
        + ` (index ${entry.goldenSha256}, actual ${actualHash})`,
      );
    }
  }
}

function run(mode, schemaArgument) {
  const schemaPath = path.resolve(
    schemaArgument ?? path.join(protocolDirectory, "service-wire-v1.schema.json"),
  );
  const schema = readJson(schemaPath);
  if (mode === "write") {
    const index = buildIndex(schema, schemaPath);
    fs.writeFileSync(indexPath, renderIndex(index), "utf8");
    console.log(`service wire fixture catalog written: ${index.fixtures.length} fixtures`);
    return;
  }

  if (!fs.existsSync(indexPath)) {
    throw new Error(`fixture index does not exist: ${path.relative(protocolDirectory, indexPath)}`);
  }
  const actualText = fs.readFileSync(indexPath, "utf8");
  const actualIndex = JSON.parse(actualText);
  validateIndexReferences(actualIndex);
  const expectedText = renderIndex(buildIndex(schema, schemaPath));
  if (actualText !== expectedText) {
    throw new Error("service wire fixture catalog drift detected: generated/fixtures/index.json");
  }
  console.log(`service wire fixture catalog valid: ${actualIndex.fixtures.length} fixtures`);
}

if (process.argv[1] && scriptPath === path.resolve(process.argv[1])) {
  const [modeArgument, schemaArgument, ...extraArguments] = process.argv.slice(2);
  if (!["--write", "--check"].includes(modeArgument) || extraArguments.length > 0) {
    console.error("usage: generate-service-wire-fixtures.mjs --write|--check [schema-path]");
    process.exit(2);
  }
  try {
    run(modeArgument === "--write" ? "write" : "check", schemaArgument);
  } catch (error) {
    console.error(error.stack ?? String(error));
    process.exit(1);
  }
}

export {
  buildIndex,
  renderIndex,
  validateIndexReferences,
};
