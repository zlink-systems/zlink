const fs = require("node:fs");
const path = require("node:path");
const { execFile } = require("node:child_process");
const { runProviderBuild } = require("./provider-build-command.cjs");
const { promisify } = require("node:util");
const { replaceExactly } = require("./build-node-provider.cjs");

const root = path.resolve(__dirname, "../..");
const cppRoot = path.join(root, "framework/languages/cpp");
const extension = path.join(cppRoot, "extensions/framework-locations-etcd");
const adapter = path.join(cppRoot, "tests/location-store-provider-adapter");
const apply =
  "co_await client.txn (apply_request (*input, *decision, evaluation), operation)";
const helperPosition = "    task_t<store_write_result_t> write (";

// Only isolated source copies receive these deliberately incorrect decisions.
const cppMutations = {
  "split-record-transaction": (s) =>
    replaceExactly(
      replaceExactly(s, apply, apply.replace("client.txn", "split_txn")),
      helperPosition,
      `    task_t<wire::TxnResponse> split_txn (wire::TxnRequest request, operation_t operation)
    {
        wire::TxnResponse response;
        for (const auto &item : request.success ()) {
            wire::TxnRequest part;
            *part.mutable_compare () = request.compare ();
            *part.add_success () = item;
            response = co_await client.txn (std::move (part), operation);
        }
        co_return response;
    }

${helperPosition}`,
    ),
  "remove-apply-clock-comparison": (s) =>
    replaceExactly(
      s,
      "if (evaluation.minimum_expiry) {",
      "if (false && evaluation.minimum_expiry) {",
    ),
  "immediate-conflict-on-failed-comparison": (s) =>
    replaceExactly(
      s,
      "if (response.succeeded ())\n                co_return applied (*input, decision->now, response);",
      "if (response.succeeded ())\n                co_return applied (*input, decision->now, response);\n            co_return store_write_conflict_t{from_ms (decision->now)};",
    ),
  "retransmit-unknown-transaction": (s) =>
    replaceExactly(
      replaceExactly(s, apply, apply.replace("client.txn", "retry_txn")),
      helperPosition,
      `    task_t<wire::TxnResponse> retry_txn (wire::TxnRequest request, operation_t operation)
    {
        std::optional<wire::TxnResponse> response;
        try {
            response = co_await client.txn (request, operation);
        }
        catch (const framework_exception_t &) {
        }
        if (!response)
            response = co_await client.txn (std::move (request), operation);
        co_return std::move (*response);
    }

${helperPosition}`,
    ),
  "serializable-read": (s) =>
    replaceExactly(
      s,
      "request.mutable_request_range ()->set_revision (revision);",
      "request.mutable_request_range ()->set_revision (revision);\n    request.mutable_request_range ()->set_serializable (true);",
    ),
  "scan-latest-revision": (s) => replaceExactly(s, "snapshot.revision", "0", 2),
};

function cacheValues(file) {
  const result = {};
  for (const line of fs.readFileSync(file, "utf8").split(/\r?\n/)) {
    const match = line.match(/^([^#/:][^:]*):[^=]+=(.*)$/);
    if (match) result[match[1]] = match[2];
  }
  return result;
}
function cmake(value) {
  if (value.includes("]==]")) throw new Error("not-built: invalid CMake path");
  return `[==[${value}]==]`;
}
function requireFile(file) {
  if (!fs.existsSync(file)) throw new Error(`not-built: missing ${file}`);
  return file;
}

async function buildCppProvider(directory, mutation, baseline) {
  if (process.platform !== "linux")
    throw new Error("not-built: C++ mutation builds require Linux");
  directory = path.resolve(directory);
  fs.mkdirSync(directory, { recursive: true });
  const baselineBuild = baseline.env?.ZLINK_ETCD_CPP_BUILD_DIR;
  if (!baselineBuild || !path.isAbsolute(baselineBuild))
    throw new Error(
      "not-built: cpp adapter env.ZLINK_ETCD_CPP_BUILD_DIR must be an absolute baseline build directory",
    );
  const cache = cacheValues(
    requireFile(path.join(baselineBuild, "CMakeCache.txt")),
  );
  if (cache.ZLINK_FRAMEWORK_CPP_SHARED === "ON")
    throw new Error("not-built: C++ mutations require a static baseline build");
  const logDirectory = process.env.ZLINK_ETCD_TEST_LOG_DIR
    ? path.resolve(
        process.env.ZLINK_ETCD_TEST_LOG_DIR,
        "mutation-build/cpp",
        mutation || "control",
      )
    : directory;
  fs.mkdirSync(logDirectory, { recursive: true });
  const source = path.join(directory, "source");
  fs.cpSync(path.join(extension, "src"), path.join(source, "src"), {
    recursive: true,
  });
  fs.cpSync(path.join(extension, "include"), path.join(source, "include"), {
    recursive: true,
  });
  fs.cpSync(adapter, path.join(source, "adapter"), { recursive: true });
  const provider = path.join(source, "src/etcd.cpp");
  const text = fs.readFileSync(provider, "utf8").replaceAll("\r\n", "\n");
  if (mutation && !cppMutations[mutation])
    throw new Error(`not-built: unknown mutation ${mutation}`);
  fs.writeFileSync(provider, mutation ? cppMutations[mutation](text) : text);

  const baselineExtension = path.join(
    baselineBuild,
    "extensions/framework-locations-etcd",
  );
  const archive = requireFile(
    path.join(baselineExtension, "libzlink_framework_locations_etcd.a"),
  );
  const abstractions = requireFile(
    path.join(baselineBuild, "libzlink_framework_provider_abstractions.a"),
  );
  const ar = cache.CMAKE_AR || "ar";
  const { stdout } = await promisify(execFile)(ar, ["t", archive]);
  const members = stdout.trim().split(/\r?\n/);
  const protos = members.filter((name) => /^[^/]+\.pb\.cc\.o$/.test(name));
  // The extension has nine protobuf members and one gRPC member. Provider
  // implementation members must never enter the mutant's link inputs.
  if (protos.length !== 10 || new Set(protos).size !== 10)
    throw new Error(
      `not-built: expected ten distinct generated proto objects, found ${protos.length}`,
    );
  const objects = path.join(directory, "proto-objects");
  fs.mkdirSync(objects);
  await runProviderBuild(
    ar,
    ["x", archive, ...protos],
    objects,
    path.join(logDirectory, "extract.log"),
  );
  const generated = requireFile(path.join(baselineExtension, "generated"));
  const build = path.join(directory, "build");
  const executable = path.join(build, "location-store-provider-adapter");
  const boost = cache.ZLINK_FRAMEWORK_CPP_BOOST_INCLUDE_DIR;
  fs.writeFileSync(
    path.join(source, "CMakeLists.txt"),
    `cmake_minimum_required(VERSION 3.20)
project(cpp_provider_mutation LANGUAGES CXX)
find_package(Threads REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
add_executable(location-store-provider-adapter
  adapter/main.cpp src/etcd.cpp src/encoding.cpp src/cursor.cpp src/client.cpp
  ${protos.map((name) => cmake(path.join(objects, name))).join("\n  ")})
target_compile_features(location-store-provider-adapter PRIVATE cxx_std_20)
target_compile_options(location-store-provider-adapter PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
target_include_directories(location-store-provider-adapter PRIVATE
  ${boost ? cmake(boost) : ""}
  ${cmake(path.join(cppRoot, "common/include"))}
  ${cmake(path.join(cppRoot, "framework/include"))}
  include ${cmake(generated)})
target_link_libraries(location-store-provider-adapter PRIVATE
  ${cmake(abstractions)} gRPC::grpc++ protobuf::libprotobuf OpenSSL::Crypto
  nlohmann_json::nlohmann_json Threads::Threads)
target_link_options(location-store-provider-adapter PRIVATE -Wl,--exclude-libs,ALL)
`,
  );
  const configure = [
    "-S",
    source,
    "-B",
    build,
    "-G",
    "Ninja",
    `-DCMAKE_CXX_COMPILER=${cache.CMAKE_CXX_COMPILER}`,
    `-DCMAKE_BUILD_TYPE=${cache.CMAKE_BUILD_TYPE || "Release"}`,
  ];
  // Reuse the baseline dependency selection and ABI. Never install a different
  // triplet as an implicit side effect of a mutation build.
  for (const key of [
    "CMAKE_TOOLCHAIN_FILE",
    "CMAKE_PREFIX_PATH",
    "CMAKE_CXX_FLAGS",
    "CMAKE_CXX_FLAGS_RELEASE",
    "VCPKG_TARGET_TRIPLET",
    "VCPKG_HOST_TRIPLET",
    "VCPKG_INSTALLED_DIR",
    "VCPKG_OVERLAY_TRIPLETS",
    "gRPC_DIR",
    "Protobuf_DIR",
    "nlohmann_json_DIR",
  ])
    if (cache[key]) configure.push(`-D${key}=${cache[key]}`);
  configure.push("-DVCPKG_MANIFEST_INSTALL=OFF");
  await runProviderBuild(
    "cmake",
    configure,
    directory,
    path.join(logDirectory, "configure.log"),
  );
  await runProviderBuild(
    "cmake",
    [
      "--build",
      build,
      "--parallel",
      "1",
      "--target",
      "location-store-provider-adapter",
    ],
    directory,
    path.join(logDirectory, "build.log"),
  );
  return { ...baseline, command: requireFile(executable), args: [] };
}

module.exports = { buildCppProvider, cppMutations };
