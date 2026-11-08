const fs = require("node:fs");
const path = require("node:path");
const dependencies = require("./node-dependencies.cjs");
const root = path.resolve(__dirname, "../..");
const source = path.join(
  root,
  "framework/languages/node/packages/framework-locations-etcd/src",
);
function packageProviderDist() {
  const dist = path.resolve(
    process.env.ZLINK_ETCD_PROVIDER_DIST ||
      path.join(
        root,
        "framework/languages/node/packages/framework-locations-etcd/dist",
      ),
  );
  for (const file of ["index.js", "admin.js"]) {
    if (!fs.existsSync(path.join(dist, file)))
      throw new Error(
        `Node baseline package dist is missing: ${path.join(dist, file)}. Run npm run build in framework/languages/node/packages/framework-locations-etcd (or set ZLINK_ETCD_PROVIDER_DIST to its isolated package build output).`,
      );
  }
  return dist;
}
const apply = `const applied = await this.client.rpc<TxnResponse>(
          'KV',
          'txn',
          { compare, success },
          operation
        );`;
function replaceExactly(text, from, to, count = 1) {
  const found = text.split(from).length - 1;
  if (found !== count)
    throw new Error(
      `not-built: mutation expected ${count} target(s), found ${found}: ${from}`,
    );
  return text.split(from).join(to);
}
const mutations = {
  "split-record-transaction": (s) =>
    replaceExactly(
      s,
      apply,
      `let applied;
        for (const item of success) applied = await this.client.rpc<TxnResponse>('KV', 'txn', {compare, success: [item]}, operation);`,
    ),
  "remove-apply-clock-comparison": (s) =>
    replaceExactly(s, "if (expiries.length > 0)", "if (false)"),
  "immediate-conflict-on-failed-comparison": (s) =>
    replaceExactly(
      s,
      "if (!applied.succeeded) continue;",
      "if (!applied.succeeded) return {kind: 'conflict', storeNow: date(clock)};",
    ),
  "retransmit-unknown-transaction": (s) =>
    replaceExactly(
      s,
      apply,
      `const applied = await (async () => {
          try { return await this.client.rpc<TxnResponse>('KV', 'txn', {compare, success}, operation); }
          catch { return await this.client.rpc<TxnResponse>('KV', 'txn', {compare, success}, operation); }
        })();`,
    ),
  "serializable-read": (s) =>
    replaceExactly(s, "serializable: false", "serializable: true", 2),
  "scan-latest-revision": (s) =>
    replaceExactly(
      replaceExactly(s, "revision: snapshot.revision,", "revision: '0',"),
      "        snapshot.revision\n",
      "        '0'\n",
    ),
};
// The unmodified control and mutants use exactly the same transformation pipeline.
function buildMutationProvider(directory, mutation) {
  const ts = dependencies("typescript");
  if (mutation && !mutations[mutation])
    throw new Error(`not-built: unknown mutation ${mutation}`);
  function emit(current, relative = "") {
    for (const file of fs.readdirSync(current, { withFileTypes: true })) {
      const name = path.join(relative, file.name),
        input = path.join(current, file.name);
      if (file.isDirectory()) {
        emit(input, name);
        continue;
      }
      if (!name.endsWith(".ts")) continue;
      let text = fs.readFileSync(input, "utf8").replaceAll("\r\n", "\n");
      if (name === "location-store.ts" && mutation)
        text = mutations[mutation](text);
      const output = ts.transpileModule(text, {
        fileName: name,
        reportDiagnostics: true,
        compilerOptions: {
          target: ts.ScriptTarget.ES2022,
          module: ts.ModuleKind.CommonJS,
          esModuleInterop: true,
        },
      });
      if (
        output.diagnostics?.some(
          (d) => d.category === ts.DiagnosticCategory.Error,
        )
      )
        throw new Error(
          `not-built: ${ts.formatDiagnosticsWithColorAndContext(output.diagnostics, { getCanonicalFileName: (f) => f, getCurrentDirectory: () => root, getNewLine: () => "\n" })}`,
        );
      const dest = path.join(directory, name.replace(/\.ts$/, ".js"));
      fs.mkdirSync(path.dirname(dest), { recursive: true });
      fs.writeFileSync(dest, output.outputText);
    }
  }
  emit(source);
  fs.cpSync(
    path.join(root, "framework/runtime/protocol/etcd/v3.6.16"),
    path.join(directory, "proto/v3.6.16"),
    { recursive: true },
  );
  return directory;
}
module.exports = {
  packageProviderDist,
  buildMutationProvider,
  mutations,
  replaceExactly,
};
