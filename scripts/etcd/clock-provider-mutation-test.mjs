import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { createRequire } from "node:module";
const require = createRequire(import.meta.url);
const { Harness, logRoot } = require("./conformance-harness.cjs");
const { cases } = require("./conformance-cases.cjs");
const {
  packageProviderDist,
  replaceExactly,
} = require("./build-node-provider.cjs");

test("real guest provider clock assertion rejects Date.now as StoreNow", async () => {
  const baseline = packageProviderDist();
  const output = path.resolve(logRoot);
  fs.mkdirSync(output, { recursive: true });
  const mutant = fs.mkdtempSync(path.join(output, "host-clock-mutant-"));
  fs.cpSync(baseline, mutant, { recursive: true });
  fs.cpSync(
    path.resolve("framework/runtime/protocol/etcd/v3.6.16"),
    path.join(mutant, "proto/v3.6.16"),
    { recursive: true },
  );
  const file = path.join(mutant, "location-store.js");
  fs.writeFileSync(
    file,
    replaceExactly(
      fs.readFileSync(file, "utf8"),
      "return (0, encoding_1.decodeClock)(clock.value);",
      "return BigInt(Date.now());",
    ),
  );
  const h = new Harness(
    {
      node: {
        command: process.execPath,
        args: [
          path.resolve(
            "framework/languages/node/test/contract/location-etcd-fixtures/provider-adapter.cjs",
          ),
        ],
        env: {
          ZLINK_ETCD_PROVIDER_DIST: mutant,
          NODE_PATH: path.resolve("framework/languages/node/node_modules"),
        },
      },
    },
    path.dirname(baseline),
  );
  try {
    await h.start();
    const f = await h.namespace("host-clock-mutant");
    await assert.rejects(
      cases["etcd.provider-clock-independent"](h, f),
      assert.AssertionError,
    );
    assert.deepEqual(h.adapterErrors, []);
    console.log(
      "HOST CLOCK MUTATION: actual guest provider using Date.now rejected by assertion",
    );
  } finally {
    await h.close();
  }
});
