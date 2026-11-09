#!/usr/bin/env node
import test from "node:test";
import assert from "node:assert/strict";
import http from "node:http";
import { mkdir, mkdtemp, rm, readFile, writeFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import { join } from "node:path";
import { execFile } from "node:child_process";
import { promisify } from "node:util";
import { startProxy } from "./hold-proxy.mjs";
import { checkCoverage, fixturePath } from "./check-conformance-coverage.mjs";
import {
  delay,
  portLocks,
  readJson,
  releasePorts,
  reservePorts,
  rpc,
  writeJson,
} from "./common.mjs";
import { LIMITS } from "./constants.mjs";
import { validateEtcdFixtures } from "./validate-fixtures.mjs";
import { rawKV } from "./raw-kv.mjs";

const execute = promisify(execFile);
test(
  "golden bytes and complete spec requirement inventory",
  validateEtcdFixtures,
);
test("spec inventory survives inserted paragraphs and rejects changed bullets; time vectors enforce cap", async () => {
  const runs = fileURLToPath(new URL("./.runs/", import.meta.url));
  await mkdir(runs, { recursive: true });
  const directory = await mkdtemp(join(runs, "fixtures-"));
  try {
    const spec = await readFile(
      new URL(
        "../../framework/doc/framework/common/spec/server/05-location-relocation/07-location-store-etcd.ko.md",
        import.meta.url,
      ),
      "utf8",
    );
    const shifted = spec.replace(
      "## 12.",
      "\nInserted paragraph for line independence.\n\n## 12.",
    );
    assert.notEqual(shifted, spec);
    const path = join(directory, "07-copy.md");
    await writeFile(path, shifted);
    await validateEtcdFixtures({ "etcd-spec": path });
    console.log("spec paragraph insertion: PASS");
    const offset = shifted.indexOf("## 12.");
    const changed =
      shifted.slice(0, offset) +
      shifted.slice(offset).replace("- ", "- changed ");
    assert.notEqual(changed, shifted);
    await writeFile(path, changed);
    await assert.rejects(
      validateEtcdFixtures({ "etcd-spec": path }),
      /unknown observation/,
    );
    console.log("spec bullet mutation: rejected as expected");
    const golden = await readJson(
      new URL(
        "../../framework/runtime/protocol/golden/location-store-etcd-v1.json",
        import.meta.url,
      ),
    );
    for (const field of ["metadataVectors", "clockVectors"]) {
      const invalid = structuredClone(golden);
      if (field === "metadataVectors") {
        const vector = invalid[field].find((v) => v.hasExpiry);
        vector.expiresAtMs = "9223372036855";
        const bytes = Buffer.from(vector.metadataHex, "hex");
        bytes.writeBigUInt64BE(9223372036855n, 2);
        vector.metadataHex = bytes.toString("hex");
      } else {
        invalid.clockVectors.at(-1).value = "9223372036855";
        const bytes = Buffer.alloc(8);
        bytes.writeBigUInt64BE(9223372036855n);
        invalid.clockVectors.at(-1).hex = bytes.toString("hex");
      }
      const goldenPath = join(directory, "bad-golden.json");
      await writeJson(goldenPath, invalid);
      await assert.rejects(validateEtcdFixtures({ "golden-file": goldenPath }));
      console.log(`${field} above cap: rejected as expected`);
    }
  } finally {
    await rm(directory, { recursive: true });
  }
});

test("native port locks remain owned until the owning run releases them", async () => {
  const owner = `self-test-${process.pid}`;
  const other = `${owner}-other`;
  try {
    const ports = await reservePorts(10, owner);
    assert.equal(new Set(ports).size, 10);
    await releasePorts(other);
    for (const port of ports)
      assert.equal(
        await readFile(new URL(String(port), portLocks), "utf8"),
        owner,
      );
    await releasePorts(owner);
    for (const port of ports)
      await assert.rejects(readFile(new URL(String(port), portLocks)), {
        code: "ENOENT",
      });
    console.log("native port lock ownership and release: PASS");
  } finally {
    await releasePorts(owner);
  }
});

test("coverage accepts all languages and rejects missing, skipped, failed, duplicate and unknown results", async () => {
  const fixture = await readJson(fixturePath);
  const good = fixture.languages.map((language) => ({
    language,
    cases: fixture.cases.map(({ id, level }) => ({
      id,
      status: "passed",
      ...(level === "cross-language"
        ? {
            pairs: fixture.languages.flatMap((writer) =>
              fixture.languages.map((reader) => ({
                writer,
                reader,
                status: "passed",
              })),
            ),
          }
        : {}),
    })),
  }));
  assert.deepEqual(checkCoverage(fixture, good), []);
  for (const modify of [
    (reports) => reports[0].cases.pop(),
    (reports) => {
      reports[0].cases[0].status = "skipped";
    },
    (reports) => {
      reports[0].cases[0].status = "failed";
    },
    (reports) => reports[0].cases.push(reports[0].cases[0]),
    (reports) => reports[0].cases.push({ id: "unknown", status: "passed" }),
    (reports) => reports.pop(),
    (reports) => {
      reports[0].cases[0].skip = true;
    },
  ]) {
    const bad = structuredClone(good);
    modify(bad);
    assert.ok(checkCoverage(fixture, bad).length);
  }
  const runs = fileURLToPath(new URL("./.runs/", import.meta.url));
  await mkdir(runs, { recursive: true });
  const directory = await mkdtemp(join(runs, "coverage-"));
  try {
    const files = await Promise.all(
      good.map(async (report) => {
        const path = join(directory, `${report.language}.json`);
        await writeJson(path, report);
        return path;
      }),
    );
    const cli = fileURLToPath(
      new URL("./check-conformance-coverage.mjs", import.meta.url),
    );
    const passed = await execute(process.execPath, [cli, ...files]);
    assert.match(passed.stdout, /PASS/);
    console.log(`coverage positive exit=0: ${passed.stdout.trim()}`);
    const report = structuredClone(good[0]);
    report.cases.pop();
    report.cases[0].status = "skipped";
    await writeJson(files[0], report);
    await assert.rejects(
      execute(process.execPath, [cli, ...files]),
      (error) => {
        assert.equal(error.code, 1);
        assert.match(error.stderr, /missing/);
        assert.match(error.stderr, /skipped/);
        console.log(
          `coverage negative expected exit=1: ${error.stderr
            .split("\n")
            .filter((line) => /missing|skipped/.test(line))
            .join(" | ")}`,
        );
        return true;
      },
    );
  } finally {
    await rm(directory, { recursive: true });
  }
});

async function waitFor(fn) {
  const deadline = Date.now() + LIMITS.requestMs;
  while (Date.now() < deadline) {
    const result = await fn();
    if (result) return result;
    await delay(LIMITS.pollMs);
  }
  throw new Error("observable proxy state did not arrive");
}
test("hold proxy retains responses and requests until release, then disconnects active TCP connections", async () => {
  let requests = 0;
  const target = http.createServer((request, response) => {
    requests++;
    request.resume();
    response.end("response-bytes");
  });
  await new Promise((resolve) => target.listen(0, "127.0.0.1", resolve));
  const proxy = await startProxy({
    target: `http://127.0.0.1:${target.address().port}`,
  });
  const request = () => {
    let settled = false;
    const result = fetch(proxy.endpoint, {
      signal: AbortSignal.timeout(LIMITS.requestMs),
    }).then((response) => response.text());
    result.then(
      () => {
        settled = true;
      },
      () => {
        settled = true;
      },
    );
    return { result, settled: () => settled };
  };
  try {
    await rpc(proxy.control, "/hold-downstream");
    const heldResponse = request();
    await waitFor(
      async () => (await rpc(proxy.control, "/status")).bufferedDownstream > 0,
    );
    assert.equal(requests, 1);
    assert.equal(heldResponse.settled(), false);
    await rpc(proxy.control, "/release");
    assert.equal(await heldResponse.result, "response-bytes");
    await rpc(proxy.control, "/hold-upstream");
    const heldRequest = request();
    await waitFor(
      async () => (await rpc(proxy.control, "/status")).bufferedUpstream > 0,
    );
    assert.equal(requests, 1);
    assert.equal(heldRequest.settled(), false);
    await rpc(proxy.control, "/release");
    assert.equal(await heldRequest.result, "response-bytes");
    await rpc(proxy.control, "/hold-downstream");
    const disconnected = request();
    // Attach the rejection handler before resetting the socket.
    const rejection = assert.rejects(disconnected.result);
    await waitFor(
      async () => (await rpc(proxy.control, "/status")).bufferedDownstream > 0,
    );
    await rpc(proxy.control, "/disconnect");
    await rejection;
  } finally {
    await proxy.close();
    target.closeAllConnections();
    await new Promise((resolve) => target.close(resolve));
  }
});
test("raw observer preserves gateway bytes and historical revision and rejects gateway errors", async () => {
  const requests = [];
  const server = http.createServer(async (request, response) => {
    let body = "";
    for await (const chunk of request) body += chunk;
    requests.push({ path: request.url, body: JSON.parse(body) });
    response.setHeader("content-type", "application/json");
    if (requests.length === 3) {
      response.writeHead(400);
      response.end('{"error":"compacted"}');
    } else
      response.end(
        JSON.stringify({
          kvs: [{ key: "AP8=", value: "gAA=" }],
          header: { revision: "9007199254740993" },
        }),
      );
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  try {
    const endpoint = `http://127.0.0.1:${server.address().port}`;
    const result = await rawKV({
      command: "range",
      endpoint,
      "key-hex": "00ff",
      revision: "9007199254740993",
    });
    assert.equal(result.bytes[0].keyHex, "00ff");
    assert.equal(result.bytes[0].valueHex, "8000");
    assert.equal(requests[0].body.revision, "9007199254740993");
    await rawKV({
      command: "put",
      endpoint,
      "key-hex": "00ff",
      "value-hex": "",
    });
    assert.equal(requests[1].body.value, "");
    await assert.rejects(
      rawKV({ command: "range", endpoint, "key-hex": "00ff" }),
      /compacted/,
    );
    await assert.rejects(
      rawKV({ command: "put", endpoint, "key-hex": "00f", "value-hex": "" }),
      /even-length/,
    );
  } finally {
    server.closeAllConnections();
    await new Promise((resolve) => server.close(resolve));
  }
});
