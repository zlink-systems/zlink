#!/usr/bin/env node
import fs from "node:fs";
import assert from "node:assert/strict";
import path from "node:path";
import { createRequire } from "node:module";
import { fileURLToPath, pathToFileURL } from "node:url";
import {
  checkCoverage,
  checkExecutors,
  hostClockIds,
} from "./check-conformance-coverage.mjs";
import { args, main } from "./common.mjs";
const require = createRequire(import.meta.url);
const { buildNodeProvider } = require("./build-node-provider.cjs");
const root = fileURLToPath(new URL("../../", import.meta.url));
const mutationCases = {
  "split-record-transaction": "spi.exact-read-observation",
  "remove-apply-clock-comparison": "etcd.expiry-before-apply",
  "immediate-conflict-on-failed-comparison":
    "etcd.condition-change-before-apply",
  "retransmit-unknown-transaction": "etcd.leader-loss-before-result",
  "serializable-read": "etcd.minority-isolation",
  "scan-latest-revision": "etcd.snapshot-stability",
};
export async function runConformance({
  adapters,
  allowUnverified = false,
  mutations = true,
  selected,
} = {}) {
  process.env.NODE_PATH = [
    process.env.ZLINK_ETCD_NODE_MODULES ||
      path.join(root, "framework/languages/node/node_modules"),
    process.env.NODE_PATH,
  ]
    .filter(Boolean)
    .join(path.delimiter);
  const buildDirectory = fs.mkdtempSync(
    path.join(root, "scripts/etcd/.provider-build-"),
  );
  let h;
  const reports = [],
    mutationResults = [];
  let firstFailure;
  try {
    const baseline = buildNodeProvider(
      path.join(buildDirectory, "baseline/dist"),
    );
    const adapterFile = path.join(
      root,
      "framework/languages/node/test/contract/location-etcd-fixtures/provider-adapter.cjs",
    );
    const nodeConfig = {
      command: process.execPath,
      args: [adapterFile],
      env: { ZLINK_ETCD_PROVIDER_DIST: baseline },
    };
    adapters ??= { node: nodeConfig };
    const { Harness, logRoot } = require("./conformance-harness.cjs");
    const { cases, Unverified, inventory } = require("./conformance-cases.cjs");
    require("./conformance-oracle.cjs").verifyGolden();
    const languages = Object.keys(adapters);
    if (
      !languages.length ||
      languages.some((l) => !inventory.languages.includes(l))
    )
      throw new Error("adapter language must be node, dotnet, jvm or cpp");
    const registration = checkExecutors(inventory, cases);
    if (registration.length) throw new Error(registration.join("\n"));
    if (selected?.some((id) => !cases[id]))
      throw new Error("unknown selected case");
    h = new Harness(adapters, path.dirname(baseline));
    fs.mkdirSync(logRoot, { recursive: true });
    const save = () => {
      for (const r of reports)
        fs.writeFileSync(
          path.join(logRoot, `${r.language}-conformance.json`),
          JSON.stringify(r, null, 2),
        );
      fs.writeFileSync(
        path.join(logRoot, "mutation-results.json"),
        JSON.stringify(mutationResults, null, 2),
      );
    };
    reports.push(...languages.map((language) => ({ language, cases: [] })));
    await h.start();
    async function execute(id, language, reader) {
      h.language = language;
      const errorsBefore = h.adapterErrors.length;
      let outcome;
      try {
        if (hostClockIds.has(id))
          throw new Unverified("isolated host clock VM is unavailable");
        const f = await h.namespace(
          `${id.replaceAll(".", "-")}-${language}${reader ? `-${reader}` : ""}`,
        );
        await cases[id](h, f, language, reader);
        outcome = { id, status: "passed" };
      } catch (error) {
        outcome = {
          id,
          status: error instanceof Unverified ? "unverified" : "failed",
          reason: error.message,
        };
        if (!(error instanceof Unverified)) {
          outcome.error = error;
          firstFailure ??= error;
        }
      } finally {
        try {
          await h.endCase();
        } catch (error) {
          outcome = { id, status: "failed", reason: error.message, error };
          firstFailure ??= error;
        }
      }
      if (h.adapterErrors.length !== errorsBefore) {
        outcome = {
          id,
          status: "failed",
          reason: h.adapterErrors.slice(errorsBefore).join("\n"),
          adapterError: true,
        };
        firstFailure ??= new Error(outcome.reason);
      }
      return outcome;
    }
    for (const item of inventory.cases.filter(
      (c) => !selected || selected.includes(c.id),
    )) {
      if (item.level === "cross-language") {
        const pairs = [];
        for (const writer of languages)
          for (const reader of languages) {
            const r = await execute(item.id, writer, reader);
            pairs.push({
              writer,
              reader,
              status: r.status,
              ...(r.reason ? { reason: r.reason } : {}),
            });
            console.log(
              `${item.id} ${writer}×${reader}: ${r.status}${r.reason ? `: ${r.reason}` : ""}`,
            );
          }
        for (const r of reports)
          r.cases.push({
            id: item.id,
            status: pairs.every((p) => p.status === "passed")
              ? "passed"
              : "failed",
            pairs,
          });
      } else {
        for (const language of languages) {
          const { error, ...r } = await execute(item.id, language);
          reports.find((r) => r.language === language).cases.push(r);
          console.log(
            `${item.id} ${language}: ${r.status}${r.reason ? `: ${r.reason}` : ""}`,
          );
          if (error) console.error(error.stack);
        }
      }
      save();
      if (firstFailure) break;
    }
    if (mutations && !firstFailure && adapters.node)
      for (const [id, caseId] of Object.entries(mutationCases)) {
        let r;
        try {
          const dist = buildNodeProvider(
            path.join(buildDirectory, id, "dist"),
            id,
          );
          const original = h.adapters.node;
          h.adapters.node = {
            ...nodeConfig,
            env: { ...nodeConfig.env, ZLINK_ETCD_PROVIDER_DIST: dist },
          };
          let outcome;
          try {
            outcome = await execute(caseId, "node");
          } finally {
            h.adapters.node = original;
          }
          // AssertionError is the only evidence of detection. Runtime/conversion failures stay failures.
          r = {
            id,
            caseId,
            status:
              !outcome.adapterError &&
              outcome.error instanceof assert.AssertionError
                ? "detected"
                : "not-detected",
            reason: outcome.reason,
          };
          if (r.status === "detected") firstFailure = undefined;
        } catch (error) {
          r = { id, caseId, status: "not-built", reason: error.message };
        }
        mutationResults.push(r);
        console.log(
          `mutation ${id}: ${r.status}${r.reason ? `: ${r.reason}` : ""}`,
        );
        save();
      }
    const coverage = checkCoverage(inventory, reports, languages, {
      allowUnverified,
    });
    const unverified = reports.flatMap((r) =>
      r.cases
        .filter((c) => c.status === "unverified")
        .map((c) => `${r.language}:${c.id}`),
    );
    fs.writeFileSync(
      path.join(logRoot, "unverified.json"),
      JSON.stringify(unverified, null, 2),
    );
    const failedMutations = mutationResults.filter(
      (r) => r.status !== "detected",
    );
    console.log(
      `coverage: ${coverage.length ? "FAIL" : "PASS"}; unverified=${unverified.length}`,
    );
    console.log(
      `conformance summary: ${reports.reduce((n, r) => n + r.cases.filter((c) => c.status === "passed").length, 0)}/${inventory.cases.length * languages.length} passed; mutations ${mutationResults.filter((r) => r.status === "detected").length}/${mutations && adapters.node ? 6 : 0} detected`,
    );
    if (coverage.length || failedMutations.length || firstFailure)
      throw new Error(
        [
          firstFailure?.stack,
          ...coverage,
          ...failedMutations.map((r) => `${r.id}: ${r.status}`),
        ]
          .filter(Boolean)
          .join("\n"),
      );
    return { reports, mutationResults, unverified };
  } finally {
    try {
      if (h) await h.close();
    } finally {
      fs.rmSync(buildDirectory, { recursive: true, force: true });
    }
  }
}
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href)
  main(async () => {
    const options = args({
      "adapter-config": "string",
      "allow-unverified": "boolean",
      "no-mutations": "boolean",
      cases: "string",
    });
    await runConformance({
      adapters: options["adapter-config"]
        ? JSON.parse(fs.readFileSync(options["adapter-config"], "utf8"))
        : undefined,
      allowUnverified: options["allow-unverified"] === true,
      mutations: options["no-mutations"] !== true,
      selected: options.cases?.split(","),
    });
  });
