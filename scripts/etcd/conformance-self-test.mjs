import test from "node:test";
import assert from "node:assert/strict";
import path from "node:path";
import fs from "node:fs";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";
import {
  checkCoverage,
  checkExecutors,
  hostClockIds,
} from "./check-conformance-coverage.mjs";
import { startProxy } from "./grpc-proxy.mjs";
import dependencies from "./node-dependencies.cjs";
const require = createRequire(import.meta.url);
const { cases, inventory } = require("./conformance-cases.cjs");
const {
  replaceExactly,
  packageProviderDist,
} = require("./build-node-provider.cjs");
const { AdapterProcess } = require("./provider-process.cjs");
const {
  dotnetMutations,
  javaMutations,
} = require("./build-managed-provider.cjs");
const { cppMutations } = require("./build-cpp-provider.cjs");

test("C++ mutations require exactly the current provider source targets", () => {
  const source = fs
    .readFileSync(
      new URL(
        "../../framework/languages/cpp/extensions/framework-locations-etcd/src/etcd.cpp",
        import.meta.url,
      ),
      "utf8",
    )
    .replaceAll("\r\n", "\n");
  assert.equal(Object.keys(cppMutations).length, 6);
  for (const [id, transform] of Object.entries(cppMutations)) {
    assert.notEqual(transform(source), source, id);
    assert.throws(() => transform(""), /not-built/, id);
    assert.throws(() => transform(source + source), /not-built/, id);
  }
  console.log(
    "C++ mutation targets: 6 accepted; 12 missing/duplicate targets rejected",
  );
});

test("managed mutations require exactly the current provider source targets", () => {
  const root = fileURLToPath(new URL("../../", import.meta.url));
  for (const [language, mutations] of Object.entries({
    dotnet: dotnetMutations,
    jvm: javaMutations,
  })) {
    assert.equal(Object.keys(mutations).length, 6);
    for (const [id, entry] of Object.entries(mutations)) {
      const file =
        language === "dotnet"
          ? path.join(
              root,
              "framework/languages/dotnet/src/Zlink.Framework.Locations.Etcd",
              entry.file,
            )
          : path.join(
              root,
              "framework/languages/java/zlink-framework-locations-etcd/src/main/java/systems/zlink/framework/locations/etcd/ZLinkEtcdLocationStore.java",
            );
      const source = fs.readFileSync(file, "utf8").replaceAll("\r\n", "\n");
      const transform = language === "dotnet" ? entry.transform : entry;
      assert.notEqual(transform(source), source, `${language}:${id}`);
      assert.throws(() => transform(""), /not-built/);
      assert.throws(() => transform(source + source), /not-built/);
    }
  }
  console.log(
    "managed mutation targets: 12 accepted; 24 missing/duplicate targets rejected",
  );
});

test("missing package dist fails before cluster startup without transpile fallback", () => {
  const runner = fileURLToPath(
    new URL("./conformance-runner.mjs", import.meta.url),
  );
  const result = spawnSync(process.execPath, [runner], {
    encoding: "utf8",
    windowsHide: true,
    env: {
      ...process.env,
      ZLINK_ETCD_PROVIDER_DIST: path.join(
        path.dirname(runner),
        ".absent-package-dist",
      ),
    },
  });
  assert.equal(result.status, 1, result.stderr);
  assert.match(result.stderr, /Node baseline package dist is missing/);
  assert.match(result.stderr, /npm run build/);
  console.log(
    "missing baseline: exit=1; package build required; no transpile fallback",
  );
});

test("managed and C++ baseline artifacts must exist before cluster startup", async () => {
  const { runConformance } = await import("./conformance-runner.mjs");
  for (const language of ["dotnet", "jvm", "cpp"]) {
    const absent = fileURLToPath(
      new URL(`./.absent-${language}-adapter`, import.meta.url),
    );
    const config =
      language === "dotnet"
        ? { command: "dotnet", args: [absent + ".dll"] }
        : { command: absent, args: [] };
    await assert.rejects(
      runConformance({ adapters: { [language]: config } }),
      new RegExp(`${language} baseline adapter is missing`),
    );
  }
  console.log(
    "missing baselines: dotnet/jvm/cpp rejected before cluster startup",
  );
});

test("unexpected Store failure is not converted into a mutation assertion", async () => {
  const failure = Object.assign(new Error("unexpected read failure"), {
    category: "store-failure",
    commit: "none",
  });
  const h = {
    clock: async () => ({ value: 0n }),
    put: async () => ({ kind: "applied" }),
    get: async () => ({ mod_revision: "1" }),
  };
  const f = {
    keys: { metadata: () => Buffer.from("m"), payload: () => Buffer.from("p") },
    store: {
      read: async () => {
        throw failure;
      },
    },
  };
  await assert.rejects(
    cases["spi.exact-read-observation"](h, f),
    (error) => error === failure,
  );
});

test("Node JSONL adapter distinguishes invalid public retention from conversion failure", async () => {
  let client, store;
  try {
    const dist = packageProviderDist();
    const errors = [];
    client = new AdapterProcess(
      {
        command: process.execPath,
        args: [
          fileURLToPath(
            new URL(
              "../../framework/languages/node/test/contract/location-etcd-fixtures/provider-adapter.cjs",
              import.meta.url,
            ),
          ),
        ],
        env: {
          ZLINK_ETCD_PROVIDER_DIST: dist,
          NODE_PATH:
            process.env.ZLINK_ETCD_NODE_MODULES ||
            fileURLToPath(
              new URL(
                "../../framework/languages/node/node_modules",
                import.meta.url,
              ),
            ),
        },
      },
      errors,
    );
    store = client.store({
      namespace: "protocol",
      endpoints: ["http://127.0.0.1:1"],
      operationTimeoutMs: 100,
    });
    await assert.rejects(
      store.write({
        conditions: [],
        mutations: [
          {
            kind: "put",
            key: { value: "a" },
            bytes: Buffer.from("x"),
            retentionMs: -1,
          },
        ],
      }),
      (e) => e.category === "argument-error",
    );
    assert.deepEqual(errors, []);
    await assert.rejects(
      client.request({
        op: "write",
        store: "s1",
        request: {
          conditions: [],
          mutations: [{ kind: "put", key: "a", bytesBase64: "!" }],
        },
      }).promise,
      (e) => e.category === "adapter-error",
    );
    assert.ok(errors.length);
    assert.deepEqual(
      await client.request({ op: "cancel", target: "completed" }).promise,
      { kind: "cancelled", active: false },
    );
    console.log(
      "Node adapter: negative retention -> argument-error; invalid base64 -> adapter-error",
    );
  } finally {
    try {
      if (store) await store.dispose();
    } finally {
      if (client) await client.close();
    }
  }
});

test("adapter conversion failures and crashes remain adapter errors", async () => {
  for (const result of [
    { kind: "invented" },
    {
      kind: "found",
      value: {
        bytesBase64: "!",
        version: "v",
        expiresAtMs: null,
        storeNowMs: "0",
      },
    },
  ]) {
    const errors = [];
    const script = `require('node:readline').createInterface({input:process.stdin}).on('line',line=>{const m=JSON.parse(line); const result=m.op==='create'?{kind:'created'}:m.op==='dispose'?{kind:'disposed'}:${JSON.stringify(result)}; process.stdout.write(JSON.stringify({v:1,id:m.id,ok:true,result})+'\\n');});`;
    const client = new AdapterProcess(
      { command: process.execPath, args: ["-e", script] },
      errors,
    );
    const store = client.store({});
    try {
      await assert.rejects(
        store.read({ value: "a" }),
        (e) => e.category === "adapter-error",
      );
      assert.ok(errors.length);
    } finally {
      await store.dispose();
      await client.close();
    }
  }
  const errors = [];
  const client = new AdapterProcess(
    { command: process.execPath, args: ["-e", "process.exit(2)"] },
    errors,
  );
  const store = client.store({});
  try {
    await assert.rejects(
      store.read({ value: "a" }),
      (e) => e.category === "adapter-error",
    );
    assert.ok(errors.some((e) => e.includes("adapter crash")));
  } finally {
    await store.dispose().catch(() => {});
    await client.close();
  }
  console.log("adapter-error: unknown kind, invalid base64 and crash rejected");
});

test("all 35 functions are registered; removing a function fails coverage and restoration passes", () => {
  assert.equal(inventory.cases.length, 35);
  assert.deepEqual(checkExecutors(inventory, cases), []);
  const id = inventory.cases[0].id,
    original = cases[id];
  try {
    delete cases[id];
    assert.deepEqual(checkExecutors(inventory, cases), [
      `missing case function: ${id}`,
    ]);
    console.log(`coverage negative: missing case function: ${id}`);
  } finally {
    cases[id] = original;
  }
  assert.deepEqual(checkExecutors(inventory, cases), []);
  console.log("coverage restored: PASS (35 functions)");
  const runner = new URL("./conformance-runner.mjs", import.meta.url).href;
  const module = new URL("./conformance-cases.cjs", import.meta.url).href;
  const code = `import {createRequire} from 'node:module'; import {runConformance} from ${JSON.stringify(runner)}; const {cases}=createRequire(${JSON.stringify(module)})('./conformance-cases.cjs'); delete cases[${JSON.stringify(id)}]; await runConformance();`;
  const negative = spawnSync(
    process.execPath,
    ["--input-type=module", "-e", code],
    { encoding: "utf8", windowsHide: true },
  );
  assert.equal(negative.status, 1, negative.stderr);
  assert.match(negative.stderr, /missing case function/);
  console.log(
    "missing function runner exit=1; source registration remains restored",
  );
});

function reports(languages) {
  return languages.map((language) => ({
    language,
    cases: inventory.cases.map(({ id, level }) => ({
      id,
      status: "passed",
      ...(level === "cross-language"
        ? {
            pairs: languages.flatMap((writer) =>
              languages.map((reader) => ({ writer, reader, status: "passed" })),
            ),
          }
        : {}),
    })),
  }));
}
test("pair coverage requires complete matrices and permits only explicit host-clock exceptions", () => {
  for (const languages of [
    ["node"],
    ["node", "dotnet", "jvm"],
    inventory.languages,
  ]) {
    const good = reports(languages);
    assert.deepEqual(checkCoverage(inventory, good, languages), []);
    for (const change of [
      (r) => r[0].cases[0].pairs.pop(),
      (r) => r[0].cases[0].pairs.push(r[0].cases[0].pairs[0]),
      (r) => {
        r[0].cases[0].pairs[0].status = "failed";
      },
      (r) => {
        r[0].cases[0].pairs[0].status = "skipped";
      },
      (r) => {
        r[0].cases[0].pairs[0].skip = true;
      },
      (r) => {
        r[0].cases[0].pairs[0].writer = "other";
      },
    ]) {
      const bad = structuredClone(good);
      change(bad);
      assert.ok(checkCoverage(inventory, bad, languages).length);
    }
    console.log(
      `pair coverage ${languages.length}×${languages.length}: positive PASS; missing/duplicate/failed/skipped/unknown rejected`,
    );
  }
  const host = reports(["node"]);
  const threeLanguages = ["node", "dotnet", "jvm"];
  const missingDotnetPair = reports(threeLanguages);
  missingDotnetPair
    .find((r) => r.language === "dotnet")
    .cases.find((c) => c.pairs)
    .pairs.pop();
  assert.ok(
    checkCoverage(inventory, missingDotnetPair, threeLanguages).some(
      (failure) => failure.includes("missing pair"),
    ),
  );
  for (const c of host[0].cases)
    if (hostClockIds.has(c.id)) {
      c.status = "unverified";
      c.reason = "VM unavailable";
    }
  assert.equal(checkCoverage(inventory, host, ["node"]).length, 3);
  assert.deepEqual(
    checkCoverage(inventory, host, ["node"], { allowUnverified: true }),
    [],
  );
  host[0].cases[0].status = "unverified";
  host[0].cases[0].reason = "not a host case";
  assert.ok(
    checkCoverage(inventory, host, ["node"], { allowUnverified: true }).length,
  );
  assert.throws(
    () => replaceExactly("changed source", "target", "replacement"),
    /not-built/,
  );
  assert.throws(
    () => replaceExactly("target target", "target", "replacement"),
    /not-built/,
  );
});

test("gRPC proxy preserves exact wire bytes, selects semantic boundaries, forwards Auth and cuts connections", async () => {
  const grpc = dependencies("@grpc/grpc-js"),
    loader = dependencies("@grpc/proto-loader");
  const protoRoot = fileURLToPath(
    new URL("../../framework/runtime/protocol/etcd/v3.6.16/", import.meta.url),
  );
  const services = grpc.loadPackageDefinition(
    loader.loadSync(path.join(protoRoot, "etcd/api/etcdserverpb/rpc.proto"), {
      keepCase: true,
      longs: String,
      bytes: Buffer,
      defaults: true,
      includeDirs: [protoRoot],
    }),
  ).etcdserverpb;
  const server = new grpc.Server(),
    received = [];
  const descriptor = (name, method) =>
    Object.values(services[name].service).find((d) =>
      d.path.endsWith(`/${method}`),
    );
  const raw = (d) => ({
    ...d,
    requestDeserialize: (b) => b,
    responseSerialize: (b) => b,
  });
  for (const name of ["KV", "Auth"]) {
    const service = {},
      handlers = {};
    for (const [method, d] of Object.entries(services[name].service)) {
      service[method] = raw(d);
      handlers[method] = (call, cb) => {
        received.push({ path: d.path, bytes: call.request });
        cb(
          null,
          d.responseSerialize(
            d.path.endsWith("/Authenticate")
              ? { token: "test-token" }
              : d.path.endsWith("/Txn")
                ? { succeeded: true }
                : {},
          ),
        );
      };
    }
    server.addService(service, handlers);
  }
  const port = await new Promise((resolve, reject) =>
    server.bindAsync(
      "127.0.0.1:0",
      grpc.ServerCredentials.createInsecure(),
      (e, p) => (e ? reject(e) : resolve(p)),
    ),
  );
  const proxy = await startProxy({ target: `http://127.0.0.1:${port}` });
  const client = new grpc.Client(
    new URL(proxy.endpoint).host,
    grpc.credentials.createInsecure(),
  );
  const call = (d, bytes) =>
    new Promise((resolve, reject) =>
      client.makeUnaryRequest(
        d.path,
        (b) => b,
        (b) => b,
        bytes,
        new grpc.Metadata(),
        { deadline: Date.now() + 3000 },
        (e, r) => (e ? reject(e) : resolve(r)),
      ),
    );
  const txn = descriptor("KV", "Txn"),
    range = descriptor("KV", "Range");
  const key = Buffer.from("n\0m\0a");
  try {
    const request = txn.requestSerialize({
      success: [{ request_range: { key } }],
    });
    await call(txn, request);
    assert.deepEqual(
      received.at(-1).bytes,
      request,
      "absent range_end must stay absent on wire",
    );
    for (const boundary of [
      "apply-before",
      "txn-response",
      "metadata-response",
      "cleanup-before",
      "clock-next",
    ]) {
      const clock = Buffer.from("n\0c");
      const gate = proxy.arm({
        boundary,
        key: boundary === "clock-next" ? clock : key,
      });
      // A nonmatching key must continue without consuming the gate.
      await call(
        txn,
        txn.requestSerialize({
          success: [
            {
              request_put: {
                key: Buffer.from("other"),
                value: Buffer.from("x"),
              },
            },
          ],
        }),
      );
      if (boundary === "clock-next")
        await call(range, range.requestSerialize({ key: clock }));
      const d = ["metadata-response", "clock-next"].includes(boundary)
        ? range
        : txn;
      const body =
        boundary === "metadata-response"
          ? { key, range_end: Buffer.from("z") }
          : boundary === "clock-next"
            ? { key: clock }
            : {
                success: [
                  boundary === "cleanup-before"
                    ? { request_delete_range: { key } }
                    : { request_put: { key, value: Buffer.from([0, 255]) } },
                ],
              };
      const bytes = d.requestSerialize(body),
        count = received.length;
      const pending = call(d, bytes);
      pending.catch(() => {});
      await Promise.race([
        gate.entered,
        pending.then(() => {
          throw new Error("boundary was not held");
        }),
      ]);
      assert.equal(
        received.length,
        count +
          (["txn-response", "metadata-response"].includes(boundary) ? 1 : 0),
      );
      gate.release();
      await pending;
      assert.deepEqual(received.at(-1).bytes, bytes);
    }
    const auth = descriptor("Auth", "Authenticate"),
      authBytes = auth.requestSerialize({ name: "test", password: "test" });
    const result = await call(auth, authBytes);
    assert.equal(auth.responseDeserialize(result).token, "test-token");
    assert.deepEqual(received.at(-1).bytes, authBytes);
    const lost = proxy.arm({ boundary: "txn-response", key });
    const pending = call(
      txn,
      txn.requestSerialize({ success: [{ request_put: { key } }] }),
    );
    const rejected = assert.rejects(
      pending,
      (e) => e.code === grpc.status.UNAVAILABLE,
    );
    await lost.entered;
    lost.loseResponse();
    await rejected;
    const cut = proxy.arm({ boundary: "apply-before", key });
    const disconnected = call(
      txn,
      txn.requestSerialize({ success: [{ request_put: { key } }] }),
    );
    const cutRejected = assert.rejects(disconnected);
    await cut.entered;
    proxy.disconnect();
    await cutRejected;
    console.log(
      "proxy: five boundaries PASS; exact wire bytes PASS; Auth PASS; response loss/connection cut rejected",
    );
  } finally {
    client.close();
    await proxy.close();
    server.forceShutdown();
  }
});
