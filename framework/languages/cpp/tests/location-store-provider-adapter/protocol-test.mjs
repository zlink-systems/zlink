import test from "node:test";
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { once } from "node:events";
import net from "node:net";
import readline from "node:readline";

const executable = process.argv[2] || process.env.ZLINK_ETCD_CPP_ADAPTER;
assert.ok(
  executable,
  "usage: node protocol-test.mjs /absolute/path/to/adapter",
);

function adapter() {
  const child = spawn(executable, [], { stdio: ["pipe", "pipe", "pipe"] });
  const pending = new Map();
  const responses = [];
  let serial = 0;
  let stderr = "";
  child.stderr.on("data", (chunk) => (stderr += chunk));
  const exited = once(child, "exit");
  readline.createInterface({ input: child.stdout }).on("line", (line) => {
    const response = JSON.parse(line);
    assert.equal(response.v, 1);
    assert.equal(typeof response.ok, "boolean");
    const resolve = pending.get(response.id);
    assert.ok(resolve, `unexpected response: ${line}`);
    pending.delete(response.id);
    responses.push(response);
    resolve(response);
  });
  function raw(line, id) {
    const promise = new Promise((resolve) => pending.set(id, resolve));
    child.stdin.write(`${line}\n`);
    return promise;
  }
  function request(body, id = String(++serial)) {
    return { id, promise: raw(JSON.stringify({ v: 1, id, ...body }), id) };
  }
  return {
    request,
    raw,
    responses,
    async close() {
      child.stdin.end();
      const [code, signal] = await exited;
      assert.equal(code, 0, `${signal}: ${stderr}`);
      assert.equal(pending.size, 0, "all requests must receive one response");
    },
    kill() {
      child.kill();
    },
  };
}

function error(response, category, commit) {
  assert.equal(response.ok, false);
  assert.equal(response.error.category, category);
  assert.equal(response.error.commit, commit);
  assert.equal(typeof response.error.message, "string");
}

test(
  "C++ adapter requires JSON integer options and scan limits",
  { timeout: 10000 },
  async (t) => {
    const client = adapter();
    try {
      const options = {
        endpoints: ["http://127.0.0.1:1"],
        namespace: "adapter-integers",
        operationTimeoutMs: 100,
      };
      assert.equal(
        (await client.request({ op: "create", store: "s", options }).promise)
          .ok,
        true,
      );
      for (const limit of [1.5, 4294967297])
        await t.test(`scan limit ${limit}`, async () => {
          error(
            await client.request({
              op: "scan",
              store: "s",
              request: { prefix: "", limit, cursor: null },
            }).promise,
            "adapter-error",
          );
        });
      await t.test("fractional operation timeout", async () => {
        error(
          await client.request({
            op: "create",
            store: "fractional-timeout",
            options: { ...options, operationTimeoutMs: 100.5 },
          }).promise,
          "adapter-error",
        );
      });
      assert.deepEqual(
        (await client.request({ op: "dispose", store: "s" }).promise).result,
        { kind: "disposed" },
      );
      await client.close();
      console.log(
        "C++ integer conversion: fractional/overflow scan limits and fractional timeout rejected",
      );
    } finally {
      client.kill();
    }
  },
);

test(
  "C++ JSONL public validation, conversion and protocol errors",
  { timeout: 10000 },
  async () => {
    const client = adapter();
    try {
      const created = await client.request({
        op: "create",
        store: "s",
        options: {
          endpoints: ["http://127.0.0.1:1"],
          namespace: "adapter-protocol",
          operationTimeoutMs: 100,
        },
      }).promise;
      assert.deepEqual(created.result, { kind: "created" });
      const write = (put) =>
        client.request({
          op: "write",
          store: "s",
          request: {
            conditions: [],
            mutations: [{ kind: "put", key: "a", ...put }],
          },
        }).promise;
      error(
        await write({ bytesBase64: "AA==", retentionMs: "-1" }),
        "argument-error",
      );
      error(
        await client.request({ op: "read", store: "s", key: "" }).promise,
        "argument-error",
      );
      error(await write({ bytesBase64: "!" }), "adapter-error");
      error(
        await write({ bytesBase64: "AA==", retentionMs: "1.5" }),
        "adapter-error",
      );
      error(
        await client.request({ op: "invented", store: "s" }).promise,
        "adapter-error",
      );
      error(await client.raw("{", null), "adapter-error");
      error(
        await client.raw('{"v":2,"id":"wrong-version"}', null),
        "adapter-error",
      );
      const first = client.request(
        { op: "cancel", target: "absent" },
        "duplicate",
      );
      assert.deepEqual((await first.promise).result, {
        kind: "cancelled",
        active: false,
      });
      error(
        await client.request({ op: "cancel", target: "absent" }, "duplicate")
          .promise,
        "adapter-error",
      );
      assert.deepEqual(
        (await client.request({ op: "dispose", store: "s" }).promise).result,
        { kind: "disposed" },
      );
      await client.close();
      console.log(
        "C++ adapter validation: public argument errors PASS; conversion/protocol errors PASS",
      );
    } finally {
      client.kill();
    }
  },
);

test(
  "C++ adapter receives other requests during I/O; started cancel preserves failure",
  { timeout: 10000 },
  async () => {
    const sockets = new Set();
    let entered;
    const traffic = new Promise((resolve) => (entered = resolve));
    // Accept the gRPC handshake and hold it. This proves the public read started
    // without needing an etcd server or relying on a scheduling delay.
    const server = net.createServer((socket) => {
      sockets.add(socket);
      socket.once("data", entered);
      socket.on("close", () => sockets.delete(socket));
    });
    server.listen(0, "127.0.0.1");
    await once(server, "listening");
    const client = adapter();
    try {
      const endpoint = `http://127.0.0.1:${server.address().port}`;
      assert.equal(
        (
          await client.request({
            op: "create",
            store: "held",
            options: {
              endpoints: [endpoint],
              namespace: "adapter-held",
              operationTimeoutMs: 1500,
            },
          }).promise
        ).ok,
        true,
      );
      const read = client.request({ op: "read", store: "held", key: "a" });
      await traffic;
      const cancelled = await client.request({ op: "cancel", target: read.id })
        .promise;
      assert.deepEqual(cancelled.result, { kind: "cancelled", active: true });
      assert.ok(
        !client.responses.some((response) => response.id === read.id),
        "cancel must not settle a started operation",
      );
      error(
        await client.request({ op: "read", store: "held", key: "" }).promise,
        "argument-error",
      );
      assert.ok(
        !client.responses.some((response) => response.id === read.id),
        "validation must proceed while another read is pending",
      );
      const response = await read.promise;
      error(response, "store-failure", "none");
      assert.equal(response.error.nativeType, "framework_exception_t");
      assert.deepEqual(
        (await client.request({ op: "cancel", target: read.id }).promise)
          .result,
        { kind: "cancelled", active: false },
      );
      assert.deepEqual(
        (await client.request({ op: "dispose", store: "held" }).promise).result,
        { kind: "disposed" },
      );
      await client.close();
      console.log(
        "C++ adapter concurrency: pending read + argument request PASS; active/finished cancel PASS; original timeout failure retained",
      );
    } finally {
      client.kill();
      for (const socket of sockets) socket.destroy();
      await new Promise((resolve, reject) =>
        server.close((error) => (error ? reject(error) : resolve())),
      );
    }
  },
);
