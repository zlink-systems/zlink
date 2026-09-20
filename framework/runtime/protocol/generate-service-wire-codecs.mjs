#!/usr/bin/env node

import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { assertLoweringCoverage, lowerSchema } from "./service-wire-lowering.mjs";
import { serviceWireOutputManifest as manifest } from "./service-wire-output-manifest.mjs";

const protocolDirectory = path.dirname(fileURLToPath(import.meta.url));
const generatedDirectory = path.join(protocolDirectory, "generated");

function resolveOutput(relativePath) {
  return path.resolve(protocolDirectory, relativePath);
}

function executeTool(entry, mode, schemaPath) {
  const outputs = entry.output === undefined ? entry.outputs : [entry.output];
  const argumentsList = [
    resolveOutput(entry.tool),
    mode,
    schemaPath,
    ...outputs.map(resolveOutput),
  ];
  const result = spawnSync(process.execPath, argumentsList, {
    cwd: protocolDirectory,
    encoding: "utf8",
    stdio: "pipe",
  });
  if (result.stdout) process.stdout.write(result.stdout);
  if (result.stderr) process.stderr.write(result.stderr);
  if (result.error) throw result.error;
  if (result.status !== 0) {
    throw new Error(`${entry.tool} ${mode} failed with exit code ${result.status}`);
  }
}

function synchronizeRuntimeOutputs(mode) {
  for (const language of manifest.languages) {
    const canonicalPath = resolveOutput(language.output);
    for (const runtimeOutput of language.runtimeOutputs) {
      const runtimePath = resolveOutput(runtimeOutput);
      if (mode === "--write") {
        fs.mkdirSync(path.dirname(runtimePath), { recursive: true });
        fs.copyFileSync(canonicalPath, runtimePath);
      } else if (!fs.existsSync(runtimePath)
          || !fs.readFileSync(runtimePath).equals(fs.readFileSync(canonicalPath))) {
        throw new Error(`generated runtime copy drift: ${runtimeOutput}`);
      }
    }
  }
}

function declaredGeneratedOutputs() {
  const relativePaths = [
    ...manifest.languages.map((language) => language.output),
    ...manifest.generators.flatMap((generator) => generator.outputs),
  ];
  return new Set(relativePaths
    .map(resolveOutput)
    .filter((outputPath) => path.relative(generatedDirectory, outputPath)
      .split(path.sep)[0] !== ".."));
}

function assertManifestPathsUnique() {
  const entries = [
    ...manifest.languages.flatMap((language) => [language.output, ...language.runtimeOutputs]),
    ...manifest.generators.flatMap((generator) => generator.outputs),
  ];
  const owners = new Map();
  for (const entry of entries) {
    const resolved = resolveOutput(entry);
    if (owners.has(resolved)) {
      throw new Error(`duplicate output manifest path: ${entry} (${owners.get(resolved)})`);
    }
    owners.set(resolved, entry);
  }
}

function generatedFiles(directory = generatedDirectory) {
  if (!fs.existsSync(directory)) return [];
  const files = [];
  const visit = (current) => {
    for (const entry of fs.readdirSync(current, { withFileTypes: true })) {
      const entryPath = path.join(current, entry.name);
      if (entry.isDirectory()) visit(entryPath);
      else if (entry.isFile()) files.push(entryPath);
    }
  };
  visit(directory);
  return files.sort();
}

function assertNoOrphanOutputs() {
  const declared = declaredGeneratedOutputs();
  const orphaned = generatedFiles().filter((filePath) => !declared.has(filePath));
  if (orphaned.length !== 0) {
    throw new Error(
      `orphan generated service-wire outputs:\n${orphaned
        .map((filePath) => `  ${path.relative(protocolDirectory, filePath)}`)
        .join("\n")}`,
    );
  }
}

function run(mode, schemaArgument) {
  assertManifestPathsUnique();
  const schemaPath = path.resolve(schemaArgument ?? resolveOutput(manifest.schema));
  const schema = JSON.parse(fs.readFileSync(schemaPath, "utf8"));
  const ir = lowerSchema(schema);
  assertLoweringCoverage(schema, ir);

  for (const language of manifest.languages) {
    executeTool(language, mode, schemaPath);
  }
  synchronizeRuntimeOutputs(mode);
  for (const generator of manifest.generators) executeTool(generator, mode, schemaPath);
  assertNoOrphanOutputs();

  const runtimeCopies = manifest.languages
    .reduce((count, language) => count + language.runtimeOutputs.length, 0);
  const legacyGenerators = manifest.generators.filter((entry) => entry.legacy).length;
  console.log(
    `service-wire output manifest ${mode === "--write" ? "written" : "verified"}: `
      + `${declaredGeneratedOutputs().size} generated files, ${runtimeCopies} runtime copy, `
      + `${manifest.languages.length} renderers, ${legacyGenerators} legacy generator`,
  );
}

const [mode, schemaArgument, ...extraArguments] = process.argv.slice(2);
if (!["--write", "--check"].includes(mode) || extraArguments.length > 0) {
  console.error("usage: generate-service-wire-codecs.mjs --write|--check [schema-path]");
  process.exit(2);
}
try {
  run(mode, schemaArgument);
} catch (error) {
  console.error(error.stack ?? String(error));
  process.exit(1);
}
