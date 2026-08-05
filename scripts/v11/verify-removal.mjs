#!/usr/bin/env node

import { createHash } from "node:crypto";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, renameSync, rmSync, statSync, writeFileSync } from "node:fs";
import { dirname, extname, isAbsolute, join, resolve } from "node:path";
import { tmpdir } from "node:os";
import { spawnSync } from "node:child_process";

const SCOPES = new Set(["core", "binding:cpp", "binding:dotnet", "binding:jvm", "binding:node", "framework:cpp", "framework:dotnet", "framework:jvm", "framework:node", "common"]);
function assert(condition, message) { if (!condition) throw new Error(message); }
function shaFile(path) { return createHash("sha256").update(readFileSync(path)).digest("hex"); }
function parseArgs(argv) {
  if (argv.length === 1 && argv[0] === "--self-test") return { selfTest: true };
  const result = {};
  for (let i = 0; i < argv.length; i += 2) {
    const key = argv[i]; const value = argv[i + 1];
    assert(value && ["--scope", "--inventory", "--evidence"].includes(key),
      "usage: verify-removal.sh --scope <scope> --inventory <path> --evidence <absolute-result.json>");
    result[key.slice(2)] = value;
  }
  assert(SCOPES.has(result.scope), "invalid removal scope");
  assert(result.inventory, "missing --inventory");
  assert(result.evidence && isAbsolute(result.evidence), "--evidence must be absolute");
  return result;
}
function records(inventory) {
  const sections = ["corePublicSymbols", "coreExportSymbols", "bindingPublicDeclarations", "bindingProjectionUnits", "bindingCoreSymbolReferences", "files"];
  return sections.flatMap((section) => {
    assert(Array.isArray(inventory[section]), `inventory section is missing: ${section}`);
    return inventory[section].map((record) => ({ ...record, section }));
  });
}
function isImmutableScenarioPath(file = "") {
  return /(?:^|\/)(?:samples?|e2e)(?:\/|$)/i.test(file)
    || /(?:^|\/)SampleSupport\.(?:cs|java|kt|ts|js)$/i.test(file);
}
function inScope(record, scope) {
  if (scope === "core") return record.file?.startsWith("core/") && !record.file.startsWith("core/doc/") && record.action?.includes("remove");
  if (scope.startsWith("binding:")) {
    const language = scope.slice("binding:".length);
    const inventoryLanguage = language === "jvm" ? "java" : language;
    return record.language === inventoryLanguage
      && record.file?.startsWith(`bindings/${inventoryLanguage === "java" ? "java" : language}/`)
      && !isImmutableScenarioPath(record.file)
      && record.action?.includes("remove");
  }
  if (scope.startsWith("framework:")) {
    const language = scope.slice("framework:".length);
    const directory = language === "jvm" ? "java" : language;
    return record.file?.startsWith(`framework/languages/${directory}/`) && record.action?.includes("remove");
  }
  return ["shared-perf", "ci"].includes(record.scope) && record.action?.includes("remove");
}
function tokenPresent(text, token) {
  if (!token || token.length < 3) return false;
  const escaped = token.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
  return new RegExp(`(^|[^A-Za-z0-9_])${escaped}([^A-Za-z0-9_]|$)`, "m").test(text);
}
function cSourceForStructureScan(text) {
  return text
    .replace(/\/\*[\s\S]*?\*\//g, (value) => value.replace(/[^\n]/g, " "))
    .replace(/\/\/[^\n]*/g, (value) => " ".repeat(value.length))
    .replace(/"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'/g, (value) => value.replace(/[^\n]/g, " "));
}
function matchingBrace(text, opening) {
  let depth = 0;
  for (let index = opening; index < text.length; index += 1) {
    if (text[index] === "{") depth += 1;
    else if (text[index] === "}" && --depth === 0) return index;
  }
  return -1;
}
function structBodies(text, parent) {
  const source = cSourceForStructureScan(text);
  const escaped = parent.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
  const bodies = [];
  const openings = new Set();
  const named = new RegExp(`\\b(?:typedef\\s+)?struct\\s+${escaped}\\s*\\{`, "g");
  for (let match = named.exec(source); match; match = named.exec(source)) openings.add(source.indexOf("{", match.index));
  const anonymous = /\btypedef\s+struct\s*\{/g;
  for (let match = anonymous.exec(source); match; match = anonymous.exec(source)) {
    const opening = source.indexOf("{", match.index);
    const closing = matchingBrace(source, opening);
    if (closing < 0) continue;
    const suffix = source.slice(closing + 1, source.indexOf(";", closing + 1) + 1);
    if (tokenPresent(suffix, parent)) openings.add(opening);
  }
  for (const opening of openings) {
    const closing = matchingBrace(source, opening);
    if (closing >= 0) bodies.push(source.slice(opening + 1, closing));
  }
  return { bodies, declarationSeen: openings.size > 0 };
}
function removalSymbolPresent(text, record) {
  if (record.kind !== "struct-field" || !record.parent) return tokenPresent(text, record.symbol ?? record.coreSymbol);
  const declaration = structBodies(text, record.parent);
  if (declaration.bodies.length === 0) {
    // A removed parent makes all of its qualified members absent. If the parent
    // name remains but its declaration cannot be bounded, reject conservatively.
    return declaration.declarationSeen;
  }
  return declaration.bodies.some((body) => tokenPresent(body, record.symbol));
}
function shouldRemoveWholeFile(record) {
  if (record.action === "remove-file") return true;
  if (record.section === "bindingProjectionUnits" && record.action.includes("remove-binding-service-projection")) return true;
  return record.action === "remove-after-oracle-baseline-sealed" && /spot/i.test(record.file ?? "") && /\.(c|cc|cpp|cxx|h|hpp)$/.test(record.file ?? "");
}
function forbiddenPatterns(scope) {
  const coreProjection = /zlink_(?:mesh_node|spot|actor|instance_spot|stream_session)|ZLINK_(?:OPT_HEARTBEAT|MESH_|SPOT_|ACTOR_|INSTANCE_SPOT_|STREAM_SESSION_)/;
  if (scope === "core") return [coreProjection, /zlink\/service\//, /runtime\/services\/mesh/];
  if (scope.startsWith("binding:")) return [coreProjection, /(?:contracts|runtime)[./\\]service/, /createSpotNode|SpotDispatchEvent|bindActor|sendBoundActor/];
  if (scope.startsWith("framework:")) {
    const language = scope.slice("framework:".length);
    const removedCoreServiceApi = /\bzlink_(?:mesh_node|spot|actor|instance_spot|stream_session)_[A-Za-z0-9_]+\b/;
    const languagePatterns = {
      cpp: [/<zlink\/Contracts\/Service\//, /<zlink\/service\//],
      dotnet: [/\b(?:DllImport|LibraryImport)\b[^\n]*\bzlink_(?:mesh_node|spot|actor|instance_spot|stream_session)_/i,
        /\b(?:MethodInfo|FieldInfo)\.(?:Invoke|GetValue)\b/, /BindingFlags\.(?:NonPublic|Private)/],
      jvm: [/\bsystems\.zlink\.runtime\.(?:service|nativeapi\.(?:InternalAccess|NativeServiceSymbols|ServiceInterop))\b/,
        /\bInternalAccess\b/, /\bnativeContext\s*\([^)]*\)\s*\.\s*createMeshNode\s*\(/],
      node: [/from\s+["']@zlink-systems\/zlink\/(?:contracts|runtime)\/service/i,
        /\b(?:binding|zlink)\.createMeshNode\s*\(/, /(?:^|\/)build\/(?:Release|Debug)\/[^\s"']*\.node/]
    };
    return [removedCoreServiceApi, ...(languagePatterns[language] ?? [])];
  }
  return [/perf_(?:multi_)?spot|SPOT_(?:PUBSUB|REQREP|SENDSEND)|comp_src_spot|zlink_(?:spot|mesh_node)_/i];
}
function frameworkSourceRoot(scope, repoRoot) {
  const language = scope.slice("framework:".length);
  const relative = {
    cpp: "framework/languages/cpp/framework",
    dotnet: "framework/languages/dotnet/src/Zlink.Framework",
    jvm: "framework/languages/java/zlink-framework-core/src/main",
    node: "framework/languages/node/packages/framework/src"
  }[language];
  assert(relative, `unsupported Framework source scope: ${scope}`);
  return join(repoRoot, relative);
}
function frameworkSourceFiles(root, output = []) {
  if (!existsSync(root)) return output;
  for (const entry of readdirSync(root, { withFileTypes: true })) {
    if (["build", "dist", "node_modules", ".gradle", "obj", "bin"].includes(entry.name)) continue;
    const path = join(root, entry.name);
    if (entry.isDirectory()) frameworkSourceFiles(path, output);
    else if (/\.(?:c|cc|cpp|cxx|h|hh|hpp|cs|java|kt|kts|js|mjs|cjs|ts|tsx)$/i.test(entry.name)) output.push(path);
  }
  return output;
}
function frameworkProjectionSource(text) {
  return text
    .replace(/\/\*[\s\S]*?\*\//g, (value) => value.replace(/[^\n]/g, " "))
    .replace(/\/\/[^\n]*/g, (value) => " ".repeat(value.length));
}
function artifactRoots(scope, repoRoot) {
  if (scope === "core") return [join(repoRoot, "core/build")];
  if (scope.startsWith("binding:")) {
    const language = scope.slice("binding:".length); const directory = language === "jvm" ? "java" : language;
    return [join(repoRoot, `bindings/${directory}/build`), join(repoRoot, `bindings/${directory}/dist`), join(repoRoot, `bindings/${directory}/prebuilds`)];
  }
  if (scope.startsWith("framework:")) {
    const language = scope.slice("framework:".length); const directory = language === "jvm" ? "java" : language;
    return [join(repoRoot, `framework/languages/${directory}/build`), join(repoRoot, `.artifacts/v11/build/framework-${directory}`)];
  }
  return [join(repoRoot, "bindings/c/perf")];
}
function artifactFiles(root, output = []) {
  if (!existsSync(root)) return output;
  const stat = statSync(root);
  if (stat.isFile()) { output.push(root); return output; }
  for (const entry of readdirSync(root, { withFileTypes: true })) {
    if (["node_modules", ".pytest_cache", "results", "baseline", "archive", "tmp", "package-consumer-runs"].includes(entry.name)) continue;
    artifactFiles(join(root, entry.name), output);
  }
  return output;
}
function shouldScanArtifact(path) {
  const extension = extname(path).toLowerCase();
  return [".so", ".a", ".dll", ".dylib", ".lib", ".node", ".jar", ".nupkg", ".tgz", ".cmake", ".json", ".gradle", ".gyp", ".csproj"].includes(extension)
    || /(?:^|\/)(?:CMakeLists\.txt|pom\.xml|package\.json)$/.test(path);
}
function artifactText(path) {
  const extension = extname(path).toLowerCase();
  if ([".jar", ".nupkg"].includes(extension)) {
    const result = spawnSync("unzip", ["-p", path], { encoding: "latin1", maxBuffer: 256 * 1024 * 1024 });
    assert(result.status === 0, `cannot inspect package archive: ${path}`);
    return result.stdout;
  }
  if (extension === ".tgz") {
    const result = spawnSync("tar", ["-xOzf", path], { encoding: "latin1", maxBuffer: 256 * 1024 * 1024 });
    assert(result.status === 0, `cannot inspect package archive: ${path}`);
    return result.stdout;
  }
  return readFileSync(path).toString("latin1");
}
function verify(scope, inventoryPath, repoRoot) {
  const inventory = JSON.parse(readFileSync(inventoryPath, "utf8"));
  assert(Number.isInteger(inventory.schema) && typeof inventory.version === "string", "unsupported migration inventory");
  const selected = records(inventory).filter((record) => inScope(record, scope));
  assert(selected.length > 0, `inventory has no removal records for scope: ${scope}`);
  const violations = [];
  const checks = { absentFiles: 0, absentSymbols: 0, referenceNoHits: 0, artifactFiles: 0 };
  const patterns = forbiddenPatterns(scope);
  const symbolRecordsByFile = new Map();
  for (const record of selected) {
    const token = record.symbol ?? record.coreSymbol;
    if (token) symbolRecordsByFile.set(record.file, [...(symbolRecordsByFile.get(record.file) ?? []), record]);
  }
  if (scope.startsWith("framework:")) {
    for (const path of frameworkSourceFiles(frameworkSourceRoot(scope, repoRoot))) {
      const text = frameworkProjectionSource(readFileSync(path, "utf8"));
      const hit = patterns.find((pattern) => pattern.test(text));
      if (hit) {
        violations.push({
          kind: "framework-service-projection-reference",
          path: path.slice(repoRoot.length + 1),
          matches: [String(hit)]
        });
      }
    }
  }
  for (const record of selected) {
    assert(typeof record.id === "string" && typeof record.file === "string" && typeof record.action === "string", `invalid inventory removal record: ${record.id ?? "<missing>"}`);
    const absolute = join(repoRoot, record.file);
    if (shouldRemoveWholeFile(record)) {
      checks.absentFiles += 1;
      if (existsSync(absolute)) violations.push({ id: record.id, kind: "file-still-present", path: record.file });
      continue;
    }
    const token = record.symbol ?? record.coreSymbol;
    if (token) {
      checks.absentSymbols += 1;
      if (existsSync(absolute) && removalSymbolPresent(readFileSync(absolute, "utf8"), record)) {
        violations.push({ id: record.id, kind: "symbol-still-present", path: record.file, symbol: token });
      }
    } else {
      checks.referenceNoHits += 1;
      if (!existsSync(absolute)) continue;
      const bytes = readFileSync(absolute);
      const text = bytes.toString("latin1");
      const exactRecords = symbolRecordsByFile.get(record.file) ?? [];
      const hits = exactRecords
        .filter((candidate) => removalSymbolPresent(text, candidate))
        .map((candidate) => candidate.parent ? `${candidate.parent}.${candidate.symbol ?? candidate.coreSymbol}` : candidate.symbol ?? candidate.coreSymbol);
      const patternHit = patterns.find((pattern) => pattern.test(text));
      if (hits.length || patternHit) {
        violations.push({ id: record.id, kind: "service-reference-still-present", path: record.file,
          matches: hits.length ? hits.slice(0, 10) : [String(patternHit)] });
      }
    }
  }
  const scannedArtifacts = [];
  for (const root of artifactRoots(scope, repoRoot)) {
    for (const path of artifactFiles(root).filter(shouldScanArtifact)) {
      checks.artifactFiles += 1;
      const text = artifactText(path);
      const hit = patterns.find((pattern) => pattern.test(text));
      const oracleInput = /framework\/testdata\/v11\/oracle|oracle-(?:child|manifest)|test_mesh_node_basic|unittest_instance_spot_wire/.test(text);
      if (hit || oracleInput) violations.push({ kind: "build-package-reference", path: path.slice(repoRoot.length + 1), matches: [hit ? String(hit) : "oracle-input"] });
      scannedArtifacts.push(path.slice(repoRoot.length + 1));
    }
  }
  return { inventory, selected, violations, checks, scannedArtifacts };
}
function writeEvidence(path, value) {
  mkdirSync(dirname(path), { recursive: true });
  const temp = `${path}.tmp-${process.pid}`;
  writeFileSync(temp, `${JSON.stringify(value, null, 2)}\n`);
  renameSync(temp, path);
}
function run(args, repoRoot = process.cwd()) {
  const inventoryPath = resolve(args.inventory);
  const result = verify(args.scope, inventoryPath, repoRoot);
  const evidence = {
    schema: "zlink-v11-removal-result-v1",
    scope: args.scope,
    status: result.violations.length === 0 ? "passed" : "failed",
    inventoryPath: args.inventory,
    inventorySha256: shaFile(inventoryPath),
    selectedRecords: result.selected.length,
    checks: result.checks,
    violations: result.violations,
    scannedBuildPackageArtifacts: result.scannedArtifacts
  };
  writeEvidence(args.evidence, evidence);
  assert(result.violations.length === 0, `removal verification failed: ${result.violations.length} violation(s)`);
  process.stdout.write(`removal verification passed: scope=${args.scope} records=${result.selected.length} symbols=${result.checks.absentSymbols} files=${result.checks.absentFiles} artifacts=${result.checks.artifactFiles}\n`);
}
function selfTest() {
  const work = mkdtempSync(join(tmpdir(), "zlink-v11-removal-"));
  try {
    assert(!inScope({ language: "node", file: "bindings/node/samples/example.ts", action: "remove-service-reference" }, "binding:node"),
      "binding removal must not consume immutable sample source");
    assert(inScope({ language: "node", file: "bindings/node/src/runtime/service.ts", action: "remove-service-reference" }, "binding:node"),
      "binding removal must retain production source coverage");
    mkdirSync(join(work, "core/build"), { recursive: true });
    const source = join(work, "core/core.cpp"); const buildInput = join(work, "core/CMakeLists.txt");
    const removedFile = join(work, "core/remove_me.cpp"); const artifact = join(work, "core/build/libcandidate.so");
    const inventoryPath = join(work, "inventory.json"); const evidence = join(work, "evidence/result.json");
    writeFileSync(source, `
typedef struct zlink_mesh_monitor_event_t {
  int kind;
} zlink_mesh_monitor_event_t;
typedef struct raw_monitor_event_t {
  int kind;
} raw_monitor_event_t;
int zlink_spot_removed();
int raw_retained();
`);
    writeFileSync(buildInput, "target_link_libraries(candidate zlink_spot_runtime)\n");
    writeFileSync(removedFile, "service source\n");
    writeFileSync(artifact, "binary-zlink_spot_removed\n");
    const inventory = { schema: 1, version: "self-test",
      corePublicSymbols: [
        { id: "remove", file: "core/core.cpp", symbol: "zlink_spot_removed", action: "remove-symbol-from-core-11" },
        { id: "remove-qualified-field", file: "core/core.cpp", kind: "struct-field", symbol: "kind",
          parent: "zlink_mesh_monitor_event_t", action: "remove-symbol-from-core-11" }
      ],
      coreExportSymbols: [], bindingPublicDeclarations: [], bindingProjectionUnits: [], bindingCoreSymbolReferences: [],
      files: [
        { id: "build-input", file: "core/CMakeLists.txt", action: "remove-service-reference" },
        { id: "whole-file", file: "core/remove_me.cpp", action: "remove-file" }
      ] };
    writeFileSync(inventoryPath, JSON.stringify(inventory));
    let rejected = false; try { run({ scope: "core", inventory: inventoryPath, evidence }, work); } catch (error) { rejected = error.message.includes("violation"); }
    assert(rejected, "self-test expected live symbol rejection");
    writeFileSync(source, `
typedef struct raw_monitor_event_t {
  int kind;
} raw_monitor_event_t;
void observe_removed_parent(zlink_mesh_monitor_event_t *event);
int raw_retained();
`);
    writeFileSync(buildInput, "target_link_libraries(candidate raw_runtime)\n");
    rmSync(removedFile);
    let artifactRejected = false; try { run({ scope: "core", inventory: inventoryPath, evidence }, work); } catch (error) { artifactRejected = error.message.includes("violation"); }
    assert(artifactRejected, "self-test expected build artifact rejection");
    writeFileSync(artifact, "binary-raw-runtime\n");
    run({ scope: "core", inventory: inventoryPath, evidence }, work);
    const result = JSON.parse(readFileSync(evidence, "utf8"));
    assert(result.status === "passed" && result.checks.absentSymbols === 2 && result.checks.referenceNoHits === 1
      && result.checks.absentFiles === 1 && result.checks.artifactFiles === 1, "self-test evidence mismatch");
    for (const language of ["cpp", "dotnet", "jvm", "node"]) {
      const directory = language === "jvm" ? "java" : language;
      assert(inScope({ file: `framework/languages/${directory}/audit`,
        action: "remove-framework-binding-service-projection-reference" }, `framework:${language}`),
      `Framework audit record is not selected: ${language}`);
    }
    const frameworkRoot = join(work,
      "framework/languages/java/zlink-framework-core/src/main/systems/zlink/contracts/service/spot");
    mkdirSync(frameworkRoot, { recursive: true });
    const frameworkValueType = join(frameworkRoot, "MeshNodeStatus.java");
    writeFileSync(frameworkValueType,
      "package systems.zlink.contracts.service.spot; record MeshNodeStatus(int peers) {}\n");
    const frameworkAudit = join(work, "framework/languages/java/zlink-framework-core/build.gradle.kts");
    mkdirSync(dirname(frameworkAudit), { recursive: true });
    writeFileSync(frameworkAudit, "plugins { java }\n");
    const frameworkInventory = { schema: 1, version: "self-test",
      corePublicSymbols: [], coreExportSymbols: [], bindingPublicDeclarations: [],
      bindingProjectionUnits: [], bindingCoreSymbolReferences: [], files: [
        { id: "framework-jvm-audit", file: "framework/languages/java/zlink-framework-core/build.gradle.kts",
          action: "remove-framework-binding-service-projection-reference" }
      ] };
    writeFileSync(inventoryPath, JSON.stringify(frameworkInventory));
    run({ scope: "framework:jvm", inventory: inventoryPath, evidence }, work);
    writeFileSync(frameworkValueType,
      "package systems.zlink.contracts.service.spot; class Bad { InternalAccess access; }\n");
    let privateAccessRejected = false;
    try { run({ scope: "framework:jvm", inventory: inventoryPath, evidence }, work); }
    catch (error) { privateAccessRejected = error.message.includes("violation"); }
    assert(privateAccessRejected,
      "Framework self-test did not reject removed binding private access");
    process.stdout.write("removal verifier self-test passed: qualified member scope, raw-name preservation, symbol/file no-hit, runtime isolation, Framework-owned value-type allowance, and private-access negative mutation\n");
  } finally { rmSync(work, { recursive: true, force: true }); }
}

try { const args = parseArgs(process.argv.slice(2)); if (args.selfTest) selfTest(); else run(args); }
catch (error) { process.stderr.write(`${error.message}\n`); process.exitCode = 2; }
