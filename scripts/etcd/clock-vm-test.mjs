#!/usr/bin/env node
// Real guest integration: no executor or provider substitutes are accepted.
import fs from "node:fs";
import assert from "node:assert/strict";
import { createRequire } from "node:module";
const require = createRequire(import.meta.url);
const { Harness } = require("./conformance-harness.cjs");
const { cases } = require("./conformance-cases.cjs");
const { packageProviderDist } = require("./build-node-provider.cjs");
const path = require("node:path");
const ids = [
  "etcd.provider-clock-independent",
  "etcd.manager-wall-clock-jumps",
  "spi.snapshot-clock",
];
const baseline = packageProviderDist();
const adapters = process.env.ZLINK_ETCD_ADAPTER_CONFIG
  ? JSON.parse(fs.readFileSync(process.env.ZLINK_ETCD_ADAPTER_CONFIG, "utf8"))
  : {
      node: {
        command: process.execPath,
        args: [
          path.resolve(
            "framework/languages/node/test/contract/location-etcd-fixtures/provider-adapter.cjs",
          ),
        ],
        env: { ZLINK_ETCD_PROVIDER_DIST: baseline },
      },
    };
const h = new Harness(adapters, path.dirname(baseline));
let failed = 0,
  passed = 0;
try {
  await h.start();
  for (
    let iteration = 1;
    iteration <= Number(process.env.ZLINK_CLOCK_ITERATIONS || 40);
    iteration++
  )
    for (const language of Object.keys(adapters))
      for (const id of ids) {
        h.language = language;
        const errorsBefore = h.adapterErrors.length;
        try {
          try {
            const f = await h.namespace(
              `${id.replaceAll(".", "-")}-${language}`,
            );
            await cases[id](h, f, language);
          } finally {
            await h.endCase();
          }
          assert.equal(
            h.adapterErrors.length,
            errorsBefore,
            h.adapterErrors.slice(errorsBefore).join("\n"),
          );
          passed++;
          console.log(`${iteration} ${language} ${id}: passed`);
        } catch (error) {
          failed++;
          console.log(`${iteration} ${language} ${id}: failed: ${error.stack}`);
        }
      }
} finally {
  await h.close();
}
console.log(`HOST CLOCK SUMMARY: passed=${passed}; failed=${failed}`);
if (failed) process.exitCode = 1;
