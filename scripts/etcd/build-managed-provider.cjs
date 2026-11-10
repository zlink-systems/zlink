const fs = require("node:fs");
const path = require("node:path");
const { runProviderBuild } = require("./provider-build-command.cjs");
const { replaceExactly } = require("./build-node-provider.cjs");

const root = path.resolve(__dirname, "../..");
const dotnetSource = path.join(
  root,
  "framework/languages/dotnet/src/Zlink.Framework.Locations.Etcd",
);
const javaSource = path.join(
  root,
  "framework/languages/java/zlink-framework-locations-etcd/src/main/java",
);
const dotnetAssembly = "Zlink.Framework.Locations.Etcd";
const apply =
  "var applied = await client.Txn(apply, operation).ConfigureAwait(false);";
const javaApply = `return client.txn(
                                            applyRequest(
                                                    request, decision, evaluation.minimumExpiry()),
                                            operation)`;
const javaHelperPosition = "    private static Evaluation evaluate(";

// Each transformation changes the provider's decision, before compilation.
const dotnetMutations = {
  "split-record-transaction": {
    file: "ZLinkEtcdLocationStore.Write.cs",
    transform: (s) =>
      replaceExactly(
        s,
        apply,
        `TxnResponse applied = null!;
            foreach (var item in apply.Success)
            {
                var part = new TxnRequest();
                part.Compare.Add(apply.Compare);
                part.Success.Add(item);
                applied = await client.Txn(part, operation).ConfigureAwait(false);
            }`,
      ),
  },
  "remove-apply-clock-comparison": {
    file: "ZLinkEtcdLocationStore.Write.cs",
    transform: (s) =>
      replaceExactly(
        s,
        "if (minimumExpiry is { } expiry)",
        "if (minimumExpiry is { } expiry && false)",
      ),
  },
  "immediate-conflict-on-failed-comparison": {
    file: "ZLinkEtcdLocationStore.Write.cs",
    transform: (s) =>
      replaceExactly(
        s,
        "if (!applied.Succeeded)\n                continue;",
        "if (!applied.Succeeded)\n                return new ZLinkStoreWriteResult.Conflict(DateTimeOffset.FromUnixTimeMilliseconds(now));",
      ),
  },
  "retransmit-unknown-transaction": {
    file: "ZLinkEtcdLocationStore.Write.cs",
    transform: (s) =>
      replaceExactly(
        s,
        apply,
        `TxnResponse applied;
            try { applied = await client.Txn(apply, operation).ConfigureAwait(false); }
            catch { applied = await client.Txn(apply, operation).ConfigureAwait(false); }`,
      ),
  },
  "serializable-read": {
    file: "ZLinkEtcdLocationStore.cs",
    transform: (s) =>
      replaceExactly(
        s,
        "RequestRange = new RangeRequest { Key = key, Revision = revision },",
        "RequestRange = new RangeRequest { Key = key, Revision = revision, Serializable = true },",
      ),
  },
  "scan-latest-revision": {
    file: "ZLinkEtcdLocationStore.Scan.cs",
    transform: (s) => replaceExactly(s, "snapshot.Revision", "0", 2),
  },
};
const javaMutations = {
  "split-record-transaction": (s) =>
    replaceExactly(
      replaceExactly(s, javaApply, javaApply.replace("client.txn", "splitTxn")),
      javaHelperPosition,
      `    private CompletableFuture<TxnResponse> splitTxn(TxnRequest request, EtcdClient.Operation operation) {
        CompletableFuture<TxnResponse> result = CompletableFuture.completedFuture(null);
        for (var item : request.getSuccessList()) {
            var part = TxnRequest.newBuilder().addAllCompare(request.getCompareList()).addSuccess(item).build();
            result = result.thenCompose(ignored -> client.txn(part, operation));
        }
        return result;
    }

${javaHelperPosition}`,
    ),
  "remove-apply-clock-comparison": (s) =>
    replaceExactly(
      s,
      "if (minimumExpiry != Long.MAX_VALUE)",
      "if (minimumExpiry != Long.MAX_VALUE && false)",
    ),
  "immediate-conflict-on-failed-comparison": (s) =>
    replaceExactly(
      s,
      ": writeAttempt(request, operation));",
      ": CompletableFuture.completedFuture(new ZLinkStoreWriteConflict(Instant.ofEpochMilli(decision.now()))));",
    ),
  "retransmit-unknown-transaction": (s) =>
    replaceExactly(
      replaceExactly(s, javaApply, javaApply.replace("client.txn", "retryTxn")),
      javaHelperPosition,
      `    private CompletableFuture<TxnResponse> retryTxn(TxnRequest request, EtcdClient.Operation operation) {
        return client.txn(request, operation).exceptionallyCompose(error -> client.txn(request, operation));
    }

${javaHelperPosition}`,
    ),
  "serializable-read": (s) =>
    replaceExactly(
      s,
      ".setRequestRange(RangeRequest.newBuilder().setKey(key).setRevision(revision))",
      ".setRequestRange(RangeRequest.newBuilder().setKey(key).setRevision(revision).setSerializable(true))",
    ),
  "scan-latest-revision": (s) =>
    replaceExactly(s, "snapshot.revision()", "0", 2),
};

function files(directory, extension) {
  return fs.readdirSync(directory, { withFileTypes: true }).flatMap((entry) => {
    const file = path.join(directory, entry.name);
    return entry.isDirectory()
      ? files(file, extension)
      : file.endsWith(extension)
        ? [file]
        : [];
  });
}
function copySources(source, target, extension) {
  for (const file of files(source, extension)) {
    // Build products are never source inputs.
    if (
      path
        .relative(source, file)
        .split(path.sep)
        .some((p) => p === "obj" || p === "bin")
    )
      continue;
    const dest = path.join(target, path.relative(source, file));
    fs.mkdirSync(path.dirname(dest), { recursive: true });
    fs.writeFileSync(
      dest,
      fs.readFileSync(file, "utf8").replaceAll("\r\n", "\n"),
    );
  }
}
function mutate(file, transform) {
  fs.writeFileSync(file, transform(fs.readFileSync(file, "utf8")));
}
function xml(value) {
  return value
    .replaceAll("&", "&amp;")
    .replaceAll('"', "&quot;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;");
}

async function buildDotnet(directory, mutation, baseline, logDirectory) {
  const dll = baseline.args?.find((arg) => arg.endsWith(".dll"));
  if (!dll || !path.isAbsolute(dll) || !fs.existsSync(dll))
    throw new Error(
      "not-built: dotnet adapter requires an existing absolute DLL path in args",
    );
  const baselineDirectory = path.dirname(dll);
  const deploy = path.join(directory, "deploy");
  fs.cpSync(baselineDirectory, deploy, { recursive: true });
  const source = path.join(directory, "source");
  copySources(dotnetSource, source, ".cs");
  if (mutation) {
    const change = dotnetMutations[mutation];
    if (!change) throw new Error(`not-built: unknown mutation ${mutation}`);
    mutate(path.join(source, change.file), change.transform);
  }
  const generatedRoot = path.join(dotnetSource, "obj/Release/net8.0");
  if (!fs.existsSync(generatedRoot))
    throw new Error(
      `not-built: generated etcd protobuf source missing: ${generatedRoot}; build the baseline adapter in Release/net8.0 first`,
    );
  const assemblyInfo = fs.readFileSync(
    path.join(generatedRoot, `${dotnetAssembly}.AssemblyInfo.cs`),
    "utf8",
  );
  const assemblyVersion = assemblyInfo.match(
    /AssemblyVersionAttribute\("([^"]+)"\)/,
  )?.[1];
  if (!assemblyVersion)
    throw new Error(
      "not-built: baseline provider AssemblyVersion was not found",
    );
  const generated = files(generatedRoot, ".cs").filter(
    (file) =>
      !path.basename(file).includes("Assembly") &&
      !path.basename(file).includes("GlobalUsings"),
  );
  if (!generated.length)
    throw new Error(
      `not-built: no generated etcd protobuf source in ${generatedRoot}`,
    );
  for (const file of generated) {
    const target = path.join(
      directory,
      "generated",
      path.relative(generatedRoot, file),
    );
    fs.mkdirSync(path.dirname(target), { recursive: true });
    fs.copyFileSync(file, target);
  }
  const references = fs
    .readdirSync(baselineDirectory)
    .filter(
      (file) =>
        file.endsWith(".dll") &&
        file !== `${dotnetAssembly}.dll` &&
        file !== path.basename(dll),
    );
  const project = path.join(directory, "Provider.csproj");
  fs.writeFileSync(
    project,
    `<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net8.0</TargetFramework>
    <AssemblyName>${dotnetAssembly}</AssemblyName>
    <AssemblyVersion>${xml(assemblyVersion)}</AssemblyVersion>
    <Nullable>enable</Nullable><ImplicitUsings>enable</ImplicitUsings><LangVersion>12.0</LangVersion>
    <EnableDefaultCompileItems>false</EnableDefaultCompileItems>
    <ManagePackageVersionsCentrally>false</ManagePackageVersionsCentrally>
    <ImportDirectoryBuildProps>false</ImportDirectoryBuildProps><ImportDirectoryBuildTargets>false</ImportDirectoryBuildTargets>
  </PropertyGroup>
  <ItemGroup><Compile Include="source/**/*.cs" /><Compile Include="generated/**/*.cs" /></ItemGroup>
  <ItemGroup>${references.map((file) => `<Reference Include="${xml(path.basename(file, ".dll"))}"><HintPath>${xml(path.join(baselineDirectory, file))}</HintPath></Reference>`).join("\n")}</ItemGroup>
</Project>\n`,
  );
  // Empty local imports isolate this project from the surrounding repository's build props.
  fs.writeFileSync(
    path.join(directory, "Directory.Build.props"),
    "<Project />\n",
  );
  fs.writeFileSync(
    path.join(directory, "Directory.Build.targets"),
    "<Project />\n",
  );
  await runProviderBuild(
    "dotnet",
    [
      "build",
      project,
      "-c",
      "Release",
      "-m:1",
      "--nologo",
      "--disable-build-servers",
      "-p:UseSharedCompilation=false",
      "-o",
      path.join(directory, "output"),
    ],
    directory,
    path.join(logDirectory, "build.log"),
  );
  fs.copyFileSync(
    path.join(directory, "output", `${dotnetAssembly}.dll`),
    path.join(deploy, `${dotnetAssembly}.dll`),
  );
  return {
    ...baseline,
    args: baseline.args.map((arg) =>
      arg === dll ? path.join(deploy, path.basename(dll)) : arg,
    ),
  };
}

async function buildJava(directory, mutation, baseline, logDirectory) {
  if (!path.isAbsolute(baseline.command) || !fs.existsSync(baseline.command))
    throw new Error(
      "not-built: jvm adapter requires an existing absolute install bin script path",
    );
  const baselineDirectory = path.dirname(path.dirname(baseline.command));
  const deploy = path.join(directory, "deploy");
  fs.cpSync(baselineDirectory, deploy, { recursive: true });
  const lib = path.join(deploy, "lib");
  const jars = fs.readdirSync(lib).filter((file) => file.endsWith(".jar"));
  const providerJars = jars.filter(
    (file) =>
      file.startsWith("zlink-framework-locations-etcd-") &&
      !file.endsWith("-provider-adapter.jar"),
  );
  if (providerJars.length !== 1)
    throw new Error(
      `not-built: expected one provider jar, found ${providerJars.join(", ")}`,
    );
  const source = path.join(directory, "source");
  copySources(javaSource, source, ".java");
  if (mutation) {
    if (!javaMutations[mutation])
      throw new Error(`not-built: unknown mutation ${mutation}`);
    mutate(
      path.join(
        source,
        "systems/zlink/framework/locations/etcd/ZLinkEtcdLocationStore.java",
      ),
      javaMutations[mutation],
    );
  }
  const classes = path.join(directory, "classes");
  fs.mkdirSync(classes, { recursive: true });
  const classpath = jars
    .map((file) => path.join(lib, file))
    .join(path.delimiter);
  await runProviderBuild(
    "javac",
    ["-J-Xmx256m", "-cp", classpath, "-d", classes, ...files(source, ".java")],
    directory,
    path.join(logDirectory, "build.log"),
  );
  const jarTree = path.join(directory, "jar-tree");
  fs.mkdirSync(jarTree);
  await runProviderBuild(
    "jar",
    ["--extract", "--file", path.join(lib, providerJars[0]), "--dir", jarTree],
    directory,
    path.join(logDirectory, "jar-extract.log"),
  );
  // Remove the original production classes, including nested classes, while retaining generated proto classes.
  const providerPackage = path.join(
    jarTree,
    "systems/zlink/framework/locations/etcd",
  );
  for (const file of fs.readdirSync(providerPackage))
    if (file.endsWith(".class"))
      fs.unlinkSync(path.join(providerPackage, file));
  fs.cpSync(classes, jarTree, { recursive: true });
  await runProviderBuild(
    "jar",
    [
      "--create",
      "--file",
      path.join(lib, providerJars[0]),
      "--no-manifest",
      "-C",
      jarTree,
      ".",
    ],
    directory,
    path.join(logDirectory, "jar.log"),
  );
  return {
    ...baseline,
    command: path.join(deploy, "bin", path.basename(baseline.command)),
  };
}

async function buildManagedProvider(language, directory, mutation, baseline) {
  directory = path.resolve(directory);
  fs.mkdirSync(directory, { recursive: true });
  const logDirectory = process.env.ZLINK_ETCD_TEST_LOG_DIR
    ? path.resolve(
        process.env.ZLINK_ETCD_TEST_LOG_DIR,
        "mutation-build",
        language,
        mutation || "control",
      )
    : directory;
  fs.mkdirSync(logDirectory, { recursive: true });
  if (language === "dotnet")
    return buildDotnet(directory, mutation, baseline, logDirectory);
  if (language === "jvm")
    return buildJava(directory, mutation, baseline, logDirectory);
  throw new Error(`not-built: unsupported managed language ${language}`);
}
module.exports = { buildManagedProvider, dotnetMutations, javaMutations };
