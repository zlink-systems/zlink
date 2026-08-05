#!/usr/bin/env node

import { createHash } from "node:crypto";
import {
  existsSync,
  lstatSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  readlinkSync,
  renameSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import { dirname, isAbsolute, join, resolve } from "node:path";
import { tmpdir } from "node:os";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";

const scriptDirectory = dirname(fileURLToPath(import.meta.url));
const repositoryRoot = resolve(scriptDirectory, "..", "..");
const SHA256 = /^[0-9a-f]{64}$/u;
const REVISION = /^[0-9a-f]{40}$/u;
const LANGUAGES = new Set(["cpp", "dotnet", "jvm", "node"]);
const LANGUAGE_SUFFIX = { cpp: "CPP", dotnet: "DN", jvm: "JVM", node: "NODE" };
const CATEGORIES = ["compile", "public-declaration", "internal", "resource", "protocol"];
const CANDIDATE_KEYS = new Set([
  "schema", "ledgerId", "baseRevision", "ownedPaths", "directInputs",
  "pathCount", "aggregateSha256", "files",
]);
const FILE_KEYS = new Set(["path", "status", "mode", "contentSha256", "baseContentSha256"]);
const INPUT_KEYS = new Set(["path", "contentSha256"]);
const OWNED_KEYS = new Set(["schema", "ledgerId", "ownedPaths", "contentSha256"]);

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function sha256(value) {
  return createHash("sha256").update(value).digest("hex");
}

function fileBytes(path) {
  const stat = lstatSync(path);
  return stat.isSymbolicLink()
    ? Buffer.from(readlinkSync(path), "utf8")
    : readFileSync(path);
}

function shaFile(path) {
  return sha256(fileBytes(path));
}

function readJson(path) {
  return JSON.parse(readFileSync(path, "utf8"));
}

function stableJson(value) {
  return `${JSON.stringify(value, null, 2)}\n`;
}

function exactKeys(value, allowed, label) {
  assert(value && typeof value === "object" && !Array.isArray(value), `${label} must be an object`);
  for (const key of Object.keys(value)) {
    assert(allowed.has(key), `${label} contains unknown key: ${key}`);
  }
}

function safeRepositoryPath(path) {
  return typeof path === "string"
    && path.length > 0
    && !isAbsolute(path)
    && !path.includes("\\")
    && !path.split("/").includes("..");
}

function git(cwd, args, binary = false) {
  const result = spawnSync("git", args, {
    cwd,
    encoding: binary ? null : "utf8",
    maxBuffer: 128 * 1024 * 1024,
  });
  const stderr = binary ? result.stderr.toString("utf8") : result.stderr;
  assert(result.status === 0, `git ${args.join(" ")} exited ${result.status}: ${stderr.trim()}`);
  return result.stdout;
}

function parseArguments(argv) {
  if (argv.length === 1 && argv[0] === "--self-test") return { selfTest: true };
  const options = {};
  for (let index = 0; index < argv.length; index += 2) {
    const key = argv[index];
    const value = argv[index + 1];
    assert(
      value !== undefined
        && ["--language", "--candidate-manifest", "--evidence"].includes(key),
      "usage: run-framework-runtime-regression.sh --language <cpp|dotnet|jvm|node> --candidate-manifest <absolute-candidate.json> --evidence <absolute-result.json>",
    );
    const name = key.slice(2);
    assert(!Object.hasOwn(options, name), `duplicate ${key}`);
    options[name] = value;
  }
  for (const key of ["language", "candidate-manifest", "evidence"]) {
    assert(options[key], `missing --${key}`);
  }
  assert(LANGUAGES.has(options.language), `unsupported --language: ${options.language}`);
  for (const key of ["candidate-manifest", "evidence"]) {
    assert(isAbsolute(options[key]), `--${key} must be absolute`);
  }
  assert(resolve(options["candidate-manifest"]) !== resolve(options.evidence), "candidate and evidence paths must differ");
  return options;
}

function inferredOwnedPath(candidatePath) {
  assert(candidatePath.endsWith(".json"), "candidate manifest must use a .json suffix");
  return `${candidatePath.slice(0, -".json".length)}-owned-paths.json`;
}

function validateOwnedManifest(owned, candidate) {
  exactKeys(owned, OWNED_KEYS, "owned-path manifest");
  assert(owned.schema === "zlink-v11-owned-paths-v1", "unsupported owned-path schema");
  assert(owned.ledgerId === candidate.ledgerId, "owned-path ledgerId mismatch");
  assert(JSON.stringify(owned.ownedPaths) === JSON.stringify(candidate.ownedPaths),
    "candidate and owned-path manifests disagree");
  assert(owned.contentSha256 === sha256(JSON.stringify(owned.ownedPaths)),
    "owned-path contentSha256 mismatch");
  for (const owner of owned.ownedPaths) {
    assert(safeRepositoryPath(owner), `owned-path manifest contains an unsafe path: ${owner}`);
  }
}

function validateCandidate(candidate, candidatePath, language, repoRoot = repositoryRoot, selfTest = false) {
  exactKeys(candidate, CANDIDATE_KEYS, "candidate manifest");
  assert(candidate.schema === "zlink-v11-ledger-candidate-v1", "unsupported candidate schema");
  const ledgerPattern = new RegExp(`^V11-M6[A-C]-${LANGUAGE_SUFFIX[language]}$`, "u");
  const dotnetReference = language === "dotnet"
    && candidate.ledgerId === "V11-M6-DN-REFERENCE";
  assert(selfTest ? candidate.ledgerId === "SELF-TEST" : ledgerPattern.test(candidate.ledgerId) || dotnetReference,
    `candidate ledgerId does not match ${language}: ${candidate.ledgerId}`);
  assert(REVISION.test(candidate.baseRevision), "candidate baseRevision must be a full Git revision");
  git(repoRoot, ["cat-file", "-e", `${candidate.baseRevision}^{commit}`]);

  assert(Array.isArray(candidate.ownedPaths) && candidate.ownedPaths.length > 0, "candidate ownedPaths must not be empty");
  assert(candidate.ownedPaths.every(safeRepositoryPath), "candidate ownedPaths contains an unsafe path");
  assert(JSON.stringify([...candidate.ownedPaths].sort()) === JSON.stringify(candidate.ownedPaths), "candidate ownedPaths must be sorted");
  assert(new Set(candidate.ownedPaths).size === candidate.ownedPaths.length, "candidate ownedPaths contains duplicates");

  assert(Array.isArray(candidate.directInputs), "candidate directInputs must be an array");
  assert(
    JSON.stringify(candidate.directInputs.map(({ path }) => path))
      === JSON.stringify(candidate.directInputs.map(({ path }) => path).sort()),
    "candidate directInputs must be sorted",
  );
  assert(new Set(candidate.directInputs.map(({ path }) => path)).size === candidate.directInputs.length,
    "candidate directInputs contains duplicates");
  for (const input of candidate.directInputs) {
    exactKeys(input, INPUT_KEYS, `direct input ${input.path ?? "<missing>"}`);
    assert(safeRepositoryPath(input.path), `unsafe direct input path: ${input.path}`);
    const absolute = join(repoRoot, input.path);
    assert(existsSync(absolute), `candidate direct input is missing: ${input.path}`);
    assert(SHA256.test(input.contentSha256) && input.contentSha256 === shaFile(absolute),
      `candidate direct input drift: ${input.path}`);
  }

  assert(Array.isArray(candidate.files) && candidate.files.length > 0, "candidate files must not be empty");
  assert(candidate.pathCount === candidate.files.length, "candidate pathCount mismatch");
  assert(
    JSON.stringify(candidate.files.map(({ path }) => path))
      === JSON.stringify(candidate.files.map(({ path }) => path).sort()),
    "candidate files must be sorted",
  );
  assert(new Set(candidate.files.map(({ path }) => path)).size === candidate.files.length, "candidate files contains duplicates");
  for (const file of candidate.files) {
    exactKeys(file, FILE_KEYS, `candidate file ${file.path ?? "<missing>"}`);
    assert(safeRepositoryPath(file.path), `unsafe candidate path: ${file.path}`);
    assert(candidate.ownedPaths.some((owner) => file.path === owner || file.path.startsWith(`${owner}/`)),
      `candidate path is outside ownership: ${file.path}`);
    assert(["added", "modified", "deleted", "observed"].includes(file.status), `invalid candidate status: ${file.path}`);
    const absolute = join(repoRoot, file.path);
    if (file.status === "deleted") {
      assert(!existsSync(absolute), `deleted candidate path still exists: ${file.path}`);
      assert(file.mode === null && file.contentSha256 === null, `deleted candidate must have null mode and hash: ${file.path}`);
    } else {
      assert(existsSync(absolute), `candidate path is missing: ${file.path}`);
      assert(/^(100644|100755)$/u.test(file.mode), `candidate mode is invalid: ${file.path}`);
      const mode = `100${(lstatSync(absolute).mode & 0o777).toString(8).padStart(3, "0")}`;
      assert(file.mode === mode, `candidate mode drift: ${file.path}`);
      assert(SHA256.test(file.contentSha256) && file.contentSha256 === shaFile(absolute),
        `candidate content drift: ${file.path}`);
    }
    if (["modified", "deleted"].includes(file.status)) {
      assert(SHA256.test(file.baseContentSha256), `candidate base hash is missing: ${file.path}`);
      assert(file.baseContentSha256 === sha256(git(repoRoot, ["show", `${candidate.baseRevision}:${file.path}`], true)),
        `candidate base hash mismatch: ${file.path}`);
    } else {
      assert(file.baseContentSha256 === null, `added/observed candidate base hash must be null: ${file.path}`);
    }
  }
  assert(candidate.aggregateSha256 === sha256(JSON.stringify(candidate.files)), "candidate aggregateSha256 mismatch");
  const changed = new Set(
    git(repoRoot, ["diff", "--name-only", candidate.baseRevision, "--", ...candidate.ownedPaths])
      .trim().split("\n").filter(Boolean),
  );
  for (const path of git(repoRoot, ["ls-files", "--others", "--exclude-standard", "--", ...candidate.ownedPaths])
    .trim().split("\n").filter(Boolean)) {
    changed.add(path);
  }
  const declared = new Set(
    candidate.files.filter(({ status }) => status !== "observed").map(({ path }) => path),
  );
  for (const path of changed) {
    assert(declared.has(path), `changed owned path is missing from candidate: ${path}`);
  }
  const diffCheck = spawnSync("git", ["diff", "--check", "--", ...candidate.ownedPaths], {
    cwd: repoRoot,
    encoding: "utf8",
  });
  assert(diffCheck.status === 0, `git diff --check failed: ${diffCheck.stdout ?? ""}${diffCheck.stderr ?? ""}`);
  const candidateMtime = lstatSync(candidatePath).mtimeMs;
  for (const path of [
    ...candidate.directInputs.map(({ path }) => path),
    ...candidate.files.filter(({ status }) => status !== "deleted").map(({ path }) => path),
  ]) {
    assert(candidateMtime + 1000 >= lstatSync(join(repoRoot, path)).mtimeMs,
      `candidate manifest is older than input: ${path}`);
  }
}

function command(id, category, cwd, ...argv) {
  return { id, category, cwd, argv };
}

function cppPlan() {
  const cwd = "framework/languages/cpp";
  const build = "../../../.artifacts/v11/build/framework-runtime-regression/cpp";
  const cmake = readFileSync(join(repositoryRoot, cwd, "CMakeLists.txt"), "utf8");
  const vcpkgPackages = join(repositoryRoot, ".tools/vcpkg-src/packages");
  const protobufDirectory = join(vcpkgPackages, "protobuf_x64-linux/share/protobuf");
  const dependencyArguments = existsSync(protobufDirectory)
    ? [
        `-Dprotobuf_DIR=${protobufDirectory}`,
        `-Dabsl_DIR=${join(vcpkgPackages, "abseil_x64-linux/share/absl")}`,
        `-Dutf8_range_DIR=${join(vcpkgPackages, "utf8-range_x64-linux/share/utf8_range")}`,
        `-Dhiredis_DIR=${join(vcpkgPackages, "hiredis_x64-linux/share/hiredis")}`,
        `-Dlibuv_DIR=${join(vcpkgPackages, "libuv_x64-linux/share/libuv")}`,
        `-Dredis++_DIR=${join(vcpkgPackages, "redis-plus-plus_x64-linux/share/redis++")}`,
        `-DCMAKE_PREFIX_PATH=${[
          join(vcpkgPackages, "redis-plus-plus_x64-linux"),
          join(vcpkgPackages, "libuv_x64-linux"),
          join(vcpkgPackages, "hiredis_x64-linux"),
        ].join(";")}`,
      ]
    : [];
  const hasM6cRuntime = /add_executable\s*\(\s*test_cpp_framework_m6c_runtime\b/u.test(cmake);
  const internalTargets = [
    "test_cpp_framework_m6a_runtime",
    "test_cpp_framework_m6b_runtime",
    ...(hasM6cRuntime ? ["test_cpp_framework_m6c_runtime"] : []),
    "test_cpp_framework_channel_messaging",
    "zlink_cpp_framework_mesh_node_vertical_test",
  ];
  const internalPattern = hasM6cRuntime
    ? "^(test_cpp_framework_m6[abc]_runtime|test_cpp_framework_channel_messaging|zlink_cpp_framework_mesh_node_vertical_test)$"
    : "^(test_cpp_framework_m6[ab]_runtime|test_cpp_framework_channel_messaging|zlink_cpp_framework_mesh_node_vertical_test)$";
  const regressionCoverageTargets = [
    "test_cpp_framework_host_lifecycle",
    "test_cpp_framework_in_memory_location_store",
    "test_cpp_framework_opaque_store_providers",
    "test_cpp_framework_location_runtime",
    "test_cpp_framework_store_location_resolvers",
    "test_cpp_framework_gtest_harness",
    "test_cpp_framework_locations_redis",
  ];
  const regressionCoveragePattern =
    "^test_cpp_framework_(host_lifecycle|in_memory_location_store|opaque_store_providers|location_runtime|store_location_resolvers|gtest_harness|locations_redis)$";
  return [
    command("cpp-configure", "compile", cwd, "cmake", "-S", ".", "-B", build,
      "-DZLINK_FRAMEWORK_CPP_BUILD_TESTS=ON",
      "-DZLINK_FRAMEWORK_CPP_BUILD_FOUNDATION_TESTS=ON",
      "-DZLINK_FRAMEWORK_CPP_BUILD_SAMPLES=OFF",
      "-DZLINK_FRAMEWORK_CPP_BUILD_E2E=OFF",
      // CMake currently requires this header-only interface target while
      // generating unrelated contract targets. No target that consumes it is
      // selected by the explicit build or CTest commands below.
      "-DZLINK_STREAM_CONNECTOR_BUILD_E2E_CLIENT=ON",
      "-DZLINK_STREAM_CONNECTOR_BUILD_UNREAL=OFF",
      "-DZLINK_STREAM_CONNECTOR_BUILD_GODOT=OFF",
      "-DZLINK_STREAM_CONNECTOR_BUILD_AXMOL=OFF",
      ...dependencyArguments),
    command("cpp-runtime-compile", "compile", cwd, "cmake", "--build", build, "--target",
      "zlink_framework", "zlink_http_client", "zlink_stream_connector"),
    // contract_headers pins the public header contract: the exact members of every
    // declared type plus a negative probe that fails to COMPILE if a removed marker
    // name reappears. It was outside every gate plan, so nothing enforced it.
    command("cpp-public-build", "public-declaration", cwd, "cmake", "--build", build, "--target",
      "test_cpp_framework_layout_contract", "test_cpp_framework_target_contract",
      "test_cpp_framework_contract_headers"),
    command("cpp-public-test", "public-declaration", cwd, "ctest", "--test-dir", build,
      "--output-on-failure", "-R",
      "^test_cpp_framework_(layout_contract|target_contract|contract_headers)$"),
    command("cpp-internal-build", "internal", cwd, "cmake", "--build", build, "--target",
      ...internalTargets),
    command("cpp-internal-test", "internal", cwd, "ctest", "--test-dir", build,
      "--output-on-failure", "-R", internalPattern),
    command("cpp-regression-coverage-build", "internal", cwd, "cmake", "--build", build,
      "--target", ...regressionCoverageTargets),
    command("cpp-regression-coverage-test", "internal", cwd, "ctest", "--test-dir", build,
      "--output-on-failure", "-R", regressionCoveragePattern),
    command("cpp-resource-build", "resource", cwd, "cmake", "--build", build, "--target",
      "test_cpp_framework_operation_registry", "test_cpp_framework_location_lifecycle",
      "test_cpp_framework_submit_admission"),
    command("cpp-resource-test", "resource", cwd, "ctest", "--test-dir", build,
      "--output-on-failure", "-R",
      "^test_cpp_framework_(operation_registry|location_lifecycle|submit_admission)$"),
    command("cpp-protocol-build", "protocol", cwd, "cmake", "--build", build, "--target",
      "test_cpp_framework_service_wire_codec", "test_cpp_framework_raw_route_port_contract",
      "test_cpp_framework_location_key_codec"),
    command("cpp-protocol-test", "protocol", cwd, "ctest", "--test-dir", build,
      "--output-on-failure", "-R",
      "^test_cpp_framework_(service_wire_codec|raw_route_port_contract|location_key_codec)$"),
  ];
}

function dotnetPlan() {
  const cwd = "framework/languages/dotnet";
  const unit = "tests/Zlink.Framework.UnitTests/Zlink.Framework.UnitTests.csproj";
  const contract = "tests/Zlink.Framework.ContractTests/Zlink.Framework.ContractTests.csproj";
  const redis = "tests/Zlink.Framework.Locations.Redis.Tests/Zlink.Framework.Locations.Redis.Tests.csproj";
  const httpClient = "tests/Zlink.HttpClient.UnitTests/Zlink.HttpClient.UnitTests.csproj";
  const foundation = "tests/Zlink.Framework.M5FoundationTests/Zlink.Framework.M5FoundationTests.csproj";
  const common = ["--nologo", "--verbosity", "minimal"];
  return [
    command("dotnet-runtime-compile", "compile", cwd, "dotnet", "build", unit, ...common),
    command("dotnet-public-declarations", "public-declaration", cwd, "dotnet", "test", contract,
      "--no-restore", ...common),
    command("dotnet-internal-runtime", "internal", cwd, "dotnet", "test", unit,
      "--no-build", "--no-restore", "--filter",
      "FullyQualifiedName!~Documentation.RegressionTests", ...common),
    command("dotnet-resource-regression", "resource", cwd, "dotnet", "test", unit,
      "--no-build", "--no-restore", "--filter",
      "FullyQualifiedName~MaintenanceRuntimeTests|FullyQualifiedName~RuntimeConcurrencyBoundaryTests|FullyQualifiedName~SharedAsyncDisposalTests|FullyQualifiedName~StreamSessionForcedCleanupTests|FullyQualifiedName~DrainCoordinatorTests",
      ...common),
    command("dotnet-protocol-regression", "protocol", cwd, "dotnet", "test", unit,
      "--no-build", "--no-restore", "--filter",
      "FullyQualifiedName~ServiceRuntimeFoundationTests|FullyQualifiedName~StatefulServiceRuntimeTests|FullyQualifiedName~EnvelopeCodecTests|FullyQualifiedName~RouteCodecTests|FullyQualifiedName~StreamWireInteropTests",
      ...common),
    command("dotnet-redis-provider-regression", "internal", cwd, "dotnet", "test", redis,
      ...common),
    command("dotnet-http-client-regression", "internal", cwd, "dotnet", "test", httpClient,
      ...common),
    command("dotnet-m5-foundation-regression", "internal", cwd, "dotnet", "run",
      "--project", foundation),
  ];
}

function jvmPlan() {
  const cwd = "framework/languages/java";
  const isolatedProject = "../../../scripts/v11/gradle/framework-runtime";
  const gradle = [
    "./gradlew",
    "--console=plain",
    "--project-cache-dir",
    "build/v11-framework-runtime-gradle-cache",
    "-p",
    isolatedProject
  ];
  const runtime = ":zlink-framework-java-runtime";
  return [
    command("jvm-isolated-project-graph", "compile", cwd, ...gradle, "projects"),
    command("jvm-runtime-compile", "compile", cwd, ...gradle,
      `${runtime}:zlink-framework-core:classes`,
      `${runtime}:zlink-framework-kotlin:classes`,
      `${runtime}:zlink-http-client:classes`,
      `${runtime}:zlink-stream-connector:classes`),
    command("jvm-public-declarations", "public-declaration", cwd, ...gradle,
      `${runtime}:zlink-framework-core:contractTest`,
      `${runtime}:zlink-framework-kotlin:contractTest`),
    // Runs the whole core suite rather than a --tests glob. The globs matched only
    // 35 of the 98 test classes, and what they missed included LocationContractTest,
    // the only test in the JVM tree that pins the ZLinkFrameworkErrorKind table --
    // so the error-kind contract was guarded by a test no gate executed. The globs
    // also could not match EntrySpotActorDispatchTests, since *Dispatch*Test does
    // not match the plural class name. Measured unfiltered: 528 tests, 0 failures.
    command("jvm-internal-core", "internal", cwd, ...gradle,
      `${runtime}:zlink-framework-core:test`),
    command("jvm-internal-kotlin", "internal", cwd, ...gradle,
      `${runtime}:zlink-framework-kotlin:test`),
    // The gate compiled zlink-http-client but never ran its tests.
    // Measured before wiring: java 41/41, kotlin 9/9, 0 failures.
    command("jvm-internal-http-client", "internal", cwd, ...gradle,
      `${runtime}:zlink-http-client:test`,
      `${runtime}:zlink-http-client-kotlin:test`),
    // Held out of the gate until it was green: its 4 failures were invisible
    // because no gate ran the module, and one of them was a real runtime defect
    // (an admission monitor closed after its own DEALER, aborting the JVM and
    // truncating the suite to 9 of 31). Measured after the fixes: 31/31.
    command("jvm-internal-spring", "internal", cwd, ...gradle,
      `${runtime}:zlink-framework-spring-boot-starter:test`),
    // The official Redis provider is part of the Framework runtime boundary.
    // Its opaque Store and provider retry tests must run in the formal graph;
    // compiling the module does not validate those contracts.
    command("jvm-internal-redis-provider", "internal", cwd, ...gradle,
      `${runtime}:zlink-framework-locations-redis:test`),
    command("jvm-resource-regression", "resource", cwd, ...gradle,
      `${runtime}:zlink-framework-core:test`,
      "--tests", "*CloseGateTest", "--tests", "*TeardownExecutorTest",
      "--tests", "*Drain*Test", "--tests", "*Relocation*Test", "--tests", "*Lease*Test"),
    // The codec modules ship the Protobuf and MessagePack wire contracts but were in
    // no gate. Measured before wiring: protobuf 1/1, msgpack 2/2, 0 failures.
    command("jvm-protocol-codecs", "protocol", cwd, ...gradle,
      `${runtime}:zlink-framework-codec-protobuf:test`,
      `${runtime}:zlink-framework-codec-msgpack:test`),
    command("jvm-protocol-core", "protocol", cwd, ...gradle,
      `${runtime}:zlink-framework-core:test`,
      "--tests", "*CodecTest", "--tests", "*WireCodecTest", "--tests", "*FrameCodecTest"),
    command("jvm-protocol-connector", "protocol", cwd, ...gradle,
      `${runtime}:zlink-stream-connector:test`),
  ];
}

function nodePlan() {
  const cwd = "framework/languages/node";
  return [
    command("node-runtime-compile", "compile", cwd, "node",
      "node_modules/typescript/bin/tsc", "-b", "tsconfig.build.json"),
    command("node-public-declarations", "public-declaration", cwd, "node", "--test",
      "test/contract/contract-surface.test.js",
      "test/contract/backend-public-api-only.test.js",
      "test/contract/node-binding-parity.test.js"),
    command("node-internal-m6a", "internal", cwd, "npm", "run", "verify:m6a-runtime"),
    command("node-internal-m6b", "internal", cwd, "npm", "run", "verify:m6b-runtime"),
    command("node-internal-m6c", "internal", cwd, "npm", "run", "verify:m6c-runtime"),
    command("node-internal-channel-client", "internal", cwd, "node", "--test",
      "test/contract/channel-client.test.js"),
    command("node-internal-http-client", "internal", cwd, "node", "--test",
      "test/contract/http-client.test.js"),
    command("node-resource-regression", "resource", cwd, "node", "--test",
      "test/contract/drain-control.test.js",
      "test/contract/abort.test.js",
      "test/contract/runtime-execution.test.js",
      "test/contract/store-failure-graceful-drain.test.js"),
    command("node-protocol-regression", "protocol", cwd, "node", "--test",
      "test/contract/channel-envelope-error.test.js",
      "test/contract/message-packet-name.test.js",
      "test/contract/stream-connector-json.test.js"),
  ];
}

function planFor(language) {
  return { cpp: cppPlan, dotnet: dotnetPlan, jvm: jvmPlan, node: nodePlan }[language]();
}

function validatePlan(plan, repoRoot = repositoryRoot) {
  assert(Array.isArray(plan) && plan.length >= CATEGORIES.length, "runtime regression plan is incomplete");
  assert(new Set(plan.map(({ id }) => id)).size === plan.length, "runtime regression command IDs must be unique");
  const categories = new Set(plan.map(({ category }) => category));
  for (const category of CATEGORIES) assert(categories.has(category), `runtime regression category is missing: ${category}`);

  for (const step of plan) {
    assert(CATEGORIES.includes(step.category), `unknown command category: ${step.category}`);
    assert(safeRepositoryPath(step.cwd) && existsSync(join(repoRoot, step.cwd)), `command cwd is missing or unsafe: ${step.cwd}`);
    assert(Array.isArray(step.argv) && step.argv.length > 0, `command argv is empty: ${step.id}`);
    for (const token of step.argv) {
      const nonExecutingConnectorInterface =
        token === "-DZLINK_STREAM_CONNECTOR_BUILD_E2E_CLIENT=ON";
      assert(nonExecutingConnectorInterface
          || !/^-D[A-Z0-9_]*(?:SAMPLES?|E2E)[A-Z0-9_]*=(?:ON|TRUE|1)$/u.test(token),
        `sample/E2E CMake feature must stay disabled: ${step.id}: ${token}`);
      const disabledCmakeFeature = /^-D[A-Z0-9_]*(?:SAMPLES?|E2E)[A-Z0-9_]*=OFF$/u.test(token);
      if (!disabledCmakeFeature && !nonExecutingConnectorInterface) {
        assert(!/(^|[/:\\_-])(?:samples?|e2e|e2e-kotlin|cross-language)(?:$|[/:\\_-])/iu.test(token),
          `sample/E2E token is forbidden in runtime execution graph: ${step.id}: ${token}`);
      }
      assert(!/^verify:(?:samples?|cross-language)$/u.test(token) && token !== "test:browser",
        `sample/E2E task is forbidden in runtime execution graph: ${step.id}: ${token}`);
    }
  }

  const jvmSettings = join(repoRoot, "scripts/v11/gradle/framework-runtime/settings.gradle.kts");
  const settingsText = readFileSync(jvmSettings, "utf8");
  assert(!/includeBuild\s*\(\s*["']samples?["']\s*\)/u.test(settingsText),
    "isolated JVM settings includes the sample composite build");
  assert(!/framework\/languages\/java\/(?:samples?|e2e)/u.test(settingsText),
    "isolated JVM settings maps a sample/E2E project");
}

function runCommands(plan, runner = spawnSync, repoRoot = repositoryRoot, emitOutput = true) {
  const records = [];
  for (const step of plan) {
    if (emitOutput) process.stdout.write(`[M6-RUNTIME] start ${step.id}\n`);
    const result = runner(step.argv[0], step.argv.slice(1), {
      cwd: join(repoRoot, step.cwd),
      encoding: "utf8",
      maxBuffer: 256 * 1024 * 1024,
      env: { ...process.env, CI: process.env.CI ?? "1" },
    });
    const stdout = result.stdout ?? "";
    const stderr = result.stderr ?? "";
    if (emitOutput && stdout) process.stdout.write(stdout);
    if (emitOutput && stderr) process.stderr.write(stderr);
    let exitCode = Number.isInteger(result.status) ? result.status : 127;
    if (step.id === "jvm-isolated-project-graph"
        && /\b(?:samples?|e2e(?:-kotlin)?)\b/iu.test(`${stdout}\n${stderr}`)) {
      exitCode = 96;
      if (emitOutput) process.stderr.write("JVM isolated project graph contains a sample/E2E project\n");
    }
    records.push({
      id: step.id,
      category: step.category,
      cwd: step.cwd,
      argv: step.argv,
      required: true,
      exitCode,
      stdoutSha256: sha256(stdout),
      stderrSha256: sha256(stderr),
    });
    if (emitOutput) process.stdout.write(`[M6-RUNTIME] finish ${step.id} exit=${exitCode}\n`);
    if (exitCode !== 0) break;
  }
  return records;
}

function writeEvidence(path, value) {
  mkdirSync(dirname(path), { recursive: true });
  const temporary = `${path}.tmp-${process.pid}`;
  writeFileSync(temporary, stableJson(value));
  renameSync(temporary, path);
}

function evidenceFor(language, candidate, candidatePath, ownedPath, plan, records, sourceRevision) {
  const failed = records.filter(({ exitCode }) => exitCode !== 0);
  assert(failed.length === 0 && records.length === plan.length,
    "refusing to create passed evidence for an incomplete or failed runtime regression");
  return {
    schema: "zlink-v11-ledger-evidence-v1",
    ledgerId: candidate.ledgerId,
    status: "passed",
    sourceRevision,
    candidateManifestSha256: shaFile(candidatePath),
    ownedPathManifestSha256: shaFile(ownedPath),
    commands: records.map((record) => ({
      name: record.id,
      command: record.argv.join(" "),
      exitCode: record.exitCode,
      required: true,
    })),
    completedAt: new Date().toISOString(),
    details: {
      verifier: "M6-RUNTIME",
      language,
      candidate: {
        path: candidatePath,
        baseRevision: candidate.baseRevision,
        aggregateSha256: candidate.aggregateSha256,
        ownedPathsSha256: sha256(JSON.stringify(candidate.ownedPaths)),
      },
      isolation: {
        graphValidated: true,
        sampleProjectsExecuted: 0,
        e2eProjectsExecuted: 0,
        sampleTasksExecuted: 0,
        e2eTasksExecuted: 0,
        sampleProjectsSkipped: 0,
        e2eProjectsSkipped: 0,
        sampleTasksSkipped: 0,
        e2eTasksSkipped: 0,
      },
      commandResults: records,
      summary: {
        required: plan.length,
        passed: records.length,
        failed: 0,
      },
    },
    issues: [],
  };
}

function run(options) {
  const candidatePath = resolve(options["candidate-manifest"]);
  const evidencePath = resolve(options.evidence);
  assert(existsSync(candidatePath), `candidate manifest does not exist: ${candidatePath}`);
  const ownedPath = inferredOwnedPath(candidatePath);
  assert(existsSync(ownedPath), `owned-path manifest does not exist: ${ownedPath}`);
  assert(evidencePath !== ownedPath, "evidence and owned-path manifest paths must differ");
  const candidate = readJson(candidatePath);
  const owned = readJson(ownedPath);
  validateCandidate(candidate, candidatePath, options.language);
  validateOwnedManifest(owned, candidate);
  const plan = planFor(options.language);
  validatePlan(plan);
  rmSync(evidencePath, { force: true });
  const sourceRevision = git(repositoryRoot, ["rev-parse", "HEAD"]).trim();
  const records = runCommands(plan);
  const failed = records.find(({ exitCode }) => exitCode !== 0);
  assert(!failed, `runtime regression failed at ${failed?.id ?? "unknown command"}; no passed evidence was written`);
  validateCandidate(candidate, candidatePath, options.language);
  validateOwnedManifest(owned, candidate);
  const evidence = evidenceFor(options.language, candidate, candidatePath, ownedPath, plan, records, sourceRevision);
  writeEvidence(evidencePath, evidence);
  process.stdout.write(`framework runtime regression passed: ${candidate.ledgerId} commands=${records.length}\n`);
}

function expectFailure(action, expectedText) {
  let error;
  try {
    action();
  } catch (caught) {
    error = caught;
  }
  assert(error && error.message.includes(expectedText), `self-test expected rejection containing: ${expectedText}`);
}

function selfTest() {
  expectFailure(
    () => parseArguments([
      "--language", "node",
      "--candidate-manifest", "relative.json",
      "--evidence", resolve(tmpdir(), "result.json"),
    ]),
    "must be absolute",
  );
  expectFailure(
    () => parseArguments([
      "--language", "python",
      "--candidate-manifest", resolve(tmpdir(), "candidate.json"),
      "--evidence", resolve(tmpdir(), "result.json"),
    ]),
    "unsupported --language",
  );
  for (const language of LANGUAGES) validatePlan(planFor(language));
  const badNode = nodePlan();
  badNode.push(command("bad-sample", "internal", "framework/languages/node", "npm", "run", "verify:samples"));
  expectFailure(() => validatePlan(badNode), "forbidden");
  const badCpp = cppPlan().map((step) => ({
    ...step,
    argv: step.argv.map((token) => token === "-DZLINK_FRAMEWORK_CPP_BUILD_SAMPLES=OFF"
      ? "-DZLINK_FRAMEWORK_CPP_BUILD_SAMPLES=ON"
      : token),
  }));
  expectFailure(() => validatePlan(badCpp), "must stay disabled");

  const work = mkdtempSync(join(tmpdir(), "zlink-v11-framework-runtime-"));
  try {
    git(work, ["init", "-q"]);
    git(work, ["config", "user.email", "runtime-gate@example.invalid"]);
    git(work, ["config", "user.name", "Runtime Gate"]);
    writeFileSync(join(work, "runtime.txt"), "base\n");
    git(work, ["add", "runtime.txt"]);
    git(work, ["commit", "-qm", "base"]);
    const baseRevision = git(work, ["rev-parse", "HEAD"]).trim();
    writeFileSync(join(work, "runtime.txt"), "candidate\n");
    const files = [{
      path: "runtime.txt",
      status: "modified",
      mode: "100644",
      contentSha256: shaFile(join(work, "runtime.txt")),
      baseContentSha256: sha256("base\n"),
    }];
    const candidate = {
      schema: "zlink-v11-ledger-candidate-v1",
      ledgerId: "SELF-TEST",
      baseRevision,
      ownedPaths: ["runtime.txt"],
      directInputs: [],
      pathCount: 1,
      aggregateSha256: sha256(JSON.stringify(files)),
      files,
    };
    const candidatePath = join(work, "candidate.json");
    const ownedPath = join(work, "candidate-owned-paths.json");
    writeFileSync(candidatePath, stableJson(candidate));
    const owned = {
      schema: "zlink-v11-owned-paths-v1",
      ledgerId: "SELF-TEST",
      ownedPaths: candidate.ownedPaths,
      contentSha256: sha256(JSON.stringify(candidate.ownedPaths)),
    };
    writeFileSync(ownedPath, stableJson(owned));
    validateCandidate(candidate, candidatePath, "node", work, true);
    validateOwnedManifest(owned, candidate);
    expectFailure(
      () => validateCandidate({ ...candidate, aggregateSha256: "0".repeat(64) }, candidatePath, "node", work, true),
      "aggregateSha256",
    );

    const fakePlan = nodePlan();
    const records = runCommands(fakePlan, () => ({ status: 0, stdout: "ok\n", stderr: "" }), repositoryRoot, false);
    const evidence = evidenceFor(
      "node",
      candidate,
      candidatePath,
      ownedPath,
      fakePlan,
      records,
      baseRevision,
    );
    assert(evidence.status === "passed", "self-test expected passed evidence");
    assert(evidence.commands.length === fakePlan.length, "self-test command count mismatch");
    assert(Object.values(evidence.details.isolation).every((value) => value === true || value === 0),
      "self-test isolation evidence is invalid");
    const evidencePath = join(work, "result.json");
    writeEvidence(evidencePath, evidence);
    const rowGate = spawnSync(
      join(repositoryRoot, "scripts/v11/run-ledger-gate.sh"),
      [
        "--id", "SELF-TEST",
        "--candidate-manifest", candidatePath,
        "--owned-path-manifest", ownedPath,
        "--evidence", evidencePath,
      ],
      { cwd: work, encoding: "utf8" },
    );
    assert(rowGate.status === 0,
      `self-test evidence was rejected by ROW-GATE: ${(rowGate.stderr ?? "").trim()}`);
    let fakeRuns = 0;
    const failedRecords = runCommands(fakePlan, () => {
      fakeRuns += 1;
      return fakeRuns === 1
        ? { status: 1, stdout: "", stderr: "failure\n" }
        : { status: 0, stdout: "", stderr: "" };
    }, repositoryRoot, false);
    assert(failedRecords.length === 1 && failedRecords[0].exitCode === 1,
      "self-test expected fail-fast command execution");
    const graphViolation = runCommands(
      [command("jvm-isolated-project-graph", "compile", "framework/languages/java", "./gradlew", "projects")],
      () => ({ status: 0, stdout: "Included build ':samples'\n", stderr: "" }),
      repositoryRoot,
      false,
    );
    assert(graphViolation.length === 1 && graphViolation[0].exitCode === 96,
      "self-test expected dynamic JVM project graph isolation rejection");
    expectFailure(
      () => evidenceFor(
        "node",
        candidate,
        candidatePath,
        ownedPath,
        fakePlan,
        failedRecords,
        baseRevision,
      ),
      "refusing to create passed evidence",
    );
  } finally {
    rmSync(work, { recursive: true, force: true });
  }
  process.stdout.write(
    "framework runtime regression self-test passed: argument, candidate, graph isolation, category, evidence, and fail-fast checks\n",
  );
}

try {
  const options = parseArguments(process.argv.slice(2));
  if (options.selfTest) selfTest();
  else run(options);
} catch (error) {
  process.stderr.write(`${error.message}\n`);
  process.exitCode = 2;
}
