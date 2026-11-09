const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const { spawn } = require("node:child_process");
const { pathToFileURL } = require("node:url");
const { setTimeout: delay } = require("node:timers/promises");
const encoding = require("./conformance-oracle.cjs");
const { AdapterProcess } = require("./provider-process.cjs");

const root = path.resolve(__dirname, "../..");
const logRoot =
  process.env.ZLINK_ETCD_TEST_LOG_DIR ||
  path.join(__dirname, ".conformance-logs");
const key = (value) => ({ value });
const b64 = (value) => Buffer.from(value).toString("base64");
let sequence = 0;
const activeChildren = new Set();
function child(file, argv, options = {}) {
  const running = spawn(file, argv, {
    cwd: root,
    windowsHide: true,
    ...options,
  });
  let output = "";
  running.stdout?.on("data", (b) => {
    output += b;
  });
  running.stderr?.on("data", (b) => {
    output += b;
  });
  const exited = new Promise((resolve, reject) => {
    running.once("error", reject);
    running.once("exit", (code, signal) => resolve({ code, signal, output }));
  });
  const task = { running, exited };
  activeChildren.add(task);
  running.once("exit", () => activeChildren.delete(task));
  running.once("error", () => activeChildren.delete(task));
  return task;
}
async function command(argv, label, allowFailure = false) {
  const task = child(process.execPath, argv);
  const timer = setTimeout(() => task.running.kill(), 180000);
  try {
    const result = await task.exited;
    fs.writeFileSync(
      path.join(logRoot, `${++sequence}-${label}.log`),
      result.output,
    );
    if (!allowFailure)
      assert.equal(result.code, 0, `${label}: ${result.output}`);
    return result;
  } finally {
    clearTimeout(timer);
  }
}
async function rpc(endpoint, route, body = {}, timeout = 5000) {
  const response = await fetch(new URL(route, endpoint), {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(body),
    signal: AbortSignal.timeout(timeout),
  });
  const result = await response.json();
  if (!response.ok || result.error || result.code)
    throw new Error(JSON.stringify(result));
  return result;
}
async function until(action, label, timeout = 15000) {
  const end = performance.now() + timeout;
  let last;
  do {
    try {
      const value = await action();
      if (value) return value;
    } catch (error) {
      last = error;
    }
    await delay(20);
  } while (performance.now() < end);
  throw new Error(`Timed out waiting for ${label}`, { cause: last });
}
class Harness {
  constructor(adapters, adminPackage) {
    this.pkg = adminPackage;
    this.adapters = adapters;
    this.language = Object.keys(adapters)[0];
    this.clients = [];
    this.adapterErrors = [];
    fs.mkdirSync(logRoot, { recursive: true });
    this.directory = fs.mkdtempSync(path.join(__dirname, ".conformance-run-"));
    this.run = path.join(this.directory, "cluster");
    this.harness = path.join(root, "scripts/etcd/etcd-cluster.mjs");
    this.stores = [];
    this.managers = [];
    this.proxies = [];
    this.serial = 0;
  }
  refresh() {
    this.cluster = JSON.parse(
      fs.readFileSync(path.join(this.run, "cluster.json"), "utf8"),
    );
    return this.cluster;
  }
  async start() {
    await command(
      [
        this.harness,
        "up",
        "--backend",
        "docker",
        "--run-dir",
        this.run,
        "--quota-bytes",
        "134217728",
      ],
      "cluster-up",
    );
    this.refresh();
  }
  async clusterCommand(action, ...args) {
    const result = await command(
      [this.harness, action, "--run-dir", this.run, ...args],
      `cluster-${action}`,
    );
    this.refresh();
    return result;
  }
  options(
    namespace,
    endpoints = this.cluster.endpoints,
    operationTimeoutMs = 5000,
  ) {
    return { namespace, endpoints, operationTimeoutMs };
  }
  store(namespace, endpoints, timeout, language = this.language) {
    const config = this.adapters[language];
    if (!config) throw new Error(`unregistered adapter: ${language}`);
    let client = this.clients.find((c) => c.language === language);
    if (!client) {
      client = new AdapterProcess(config, this.adapterErrors);
      client.language = language;
      this.clients.push(client);
    }
    const store = client.store(this.options(namespace, endpoints, timeout));
    this.stores.push(store);
    return store;
  }
  async namespace(name, options = {}) {
    const namespace = `n${++this.serial}-${name}`;
    const file = path.join(this.directory, `${namespace}.json`);
    fs.writeFileSync(
      file,
      JSON.stringify(this.options(namespace, options.endpoints)),
    );
    await command(
      [path.join(this.pkg, "dist/admin.js"), "init", "--config", file],
      `${namespace}-init`,
    );
    return {
      namespace,
      file,
      keys: encoding.keys(namespace),
      store: this.store(namespace, options.endpoints),
    };
  }
  async raw(route, body = {}, endpoint = this.cluster.endpoints[0]) {
    return rpc(
      endpoint,
      `/v3/kv/${route === "delete_range" ? "deleterange" : route}`,
      body,
    );
  }
  async get(subject, endpoint) {
    return (await this.raw("range", { key: b64(subject) }, endpoint)).kvs?.[0];
  }
  async clock(f) {
    const result = await this.get(f.keys.clock);
    assert.ok(result);
    return {
      value: encoding.decodeClock(Buffer.from(result.value, "base64")),
      revision: result.mod_revision,
    };
  }
  async setClock(f, value) {
    await this.raw("put", {
      key: b64(f.keys.clock),
      value: b64(encoding.encodeClock(value)),
    });
  }
  async put(f, subject, bytes = Buffer.from("x"), retentionMs) {
    return f.store.write({
      conditions: [],
      mutations: [{ kind: "put", key: key(subject), bytes, retentionMs }],
    });
  }
  async pair(f, subject, bytes, expiry = 0n) {
    await this.raw("txn", {
      success: [
        {
          request_put: {
            key: b64(f.keys.metadata(Buffer.from(subject))),
            value: b64(
              encoding.encodeMetadata({
                hasExpiry: expiry !== 0n,
                expiresAtMs: expiry,
                payloadLength: bytes.length,
              }),
            ),
          },
        },
        {
          request_put: {
            key: b64(f.keys.payload(Buffer.from(subject))),
            value: b64(bytes),
          },
        },
      ],
    });
  }
  manager(file, label, env = {}) {
    const task = child(
      process.execPath,
      [path.join(this.pkg, "dist/admin.js"), "run", "--config", file],
      {
        env: { ...process.env, ...env },
        stdio: ["ignore", "pipe", "pipe"],
      },
    );
    task.label = label;
    this.managers.push(task);
    return task;
  }
  async stopManager(task) {
    if (task.running.exitCode === null && task.running.signalCode === null)
      task.running.kill();
    const result = await task.exited;
    fs.writeFileSync(
      path.join(logRoot, `manager-${task.label}.log`),
      result.output,
    );
    this.managers = this.managers.filter((m) => m !== task);
    return result;
  }
  async proxy(target) {
    const { startProxy } = await import(
      pathToFileURL(path.join(root, "scripts/etcd/grpc-proxy.mjs")).href
    );
    const proxy = await startProxy({ target });
    this.proxies.push(proxy);
    return proxy;
  }
  async leader() {
    return until(async () => {
      for (const member of this.cluster.members.filter(
        (m) => !m.killed && !m.paused && !m.partitioned,
      )) {
        const status = await rpc(member.endpoint, "/v3/maintenance/status");
        if (status.header.member_id === status.leader && status.leader !== "0")
          return member;
      }
    }, "maintenance leader");
  }
  async endCase() {
    if (
      !this.managers.length &&
      !this.proxies.length &&
      !this.stores.length &&
      !this.clients.length
    )
      return;
    const failures = [];
    for (const action of [
      ...this.managers.map((m) => () => this.stopManager(m)),
      ...this.proxies.map((p) => () => p.close()),
      ...this.stores.map((store) => () => store.dispose()),
      ...this.clients.map((client) => async () => {
        await client.close();
        fs.writeFileSync(
          path.join(logRoot, `${this.serial}-${client.language}-adapter.log`),
          client.output,
        );
      }),
    ]) {
      try {
        await action();
      } catch (error) {
        failures.push(error);
      }
    }
    fs.writeFileSync(
      path.join(logRoot, `${this.serial}-proxy.json`),
      JSON.stringify(
        this.proxies.map((p) => p.records),
        null,
        2,
      ),
    );
    this.stores = [];
    this.clients = [];
    this.proxies = [];
    if (failures.length) throw new AggregateError(failures, "case cleanup");
  }
  async close() {
    if (!this.cluster && fs.existsSync(path.join(this.run, "cluster.json")))
      this.refresh();
    const failures = [];
    for (const action of [
      () => this.endCase(),
      async () => {
        for (const task of [...activeChildren]) {
          task.running.kill();
          await task.exited;
        }
      },
      async () => {
        if (this.cluster) await this.clusterCommand("down");
      },
    ]) {
      try {
        await action();
      } catch (error) {
        failures.push(error);
      }
    }
    if (this.cluster) {
      for (const args of [
        ["ps", "-aq"],
        ["network", "ls", "-q"],
      ]) {
        const task = child("docker", [
          ...args,
          "--filter",
          `label=dev.zlink.etcd.run=${this.cluster.runId}`,
        ]);
        const result = await task.exited;
        assert.equal(result.code, 0, result.output);
        assert.equal(result.output.trim(), "", "leaked Docker resource");
      }
    }

    for (const member of this.cluster?.members || []) {
      if (fs.existsSync(member.log))
        fs.copyFileSync(
          member.log,
          path.join(logRoot, `member-${member.index}.log`),
        );
    }
    if (this.cluster)
      fs.writeFileSync(
        path.join(logRoot, "cluster-manifest.json"),
        JSON.stringify(this.cluster, null, 2),
      );
    fs.rmSync(this.directory, { recursive: true, force: true });
    assert.equal(activeChildren.size, 0);
    fs.writeFileSync(
      path.join(logRoot, "cleanup.json"),
      JSON.stringify({
        containers: 0,
        networks: 0,
        processes: activeChildren.size,
        temporaryDirectories: fs.existsSync(this.directory) ? 1 : 0,
      }),
    );
    if (failures.length > 0)
      throw new AggregateError(failures, "etcd fixture cleanup");
  }
}
module.exports = {
  Harness,
  command,
  child,
  rpc,
  until,
  delay,
  key,
  b64,
  encoding,
  root,
  logRoot,
};
