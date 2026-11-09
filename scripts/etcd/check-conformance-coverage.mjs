#!/usr/bin/env node
import { fileURLToPath, pathToFileURL } from "node:url";
import { args, main, readJson } from "./common.mjs";

export const fixturePath = fileURLToPath(
  new URL(
    "../../framework/runtime/conformance/location-store-provider-v1.json",
    import.meta.url,
  ),
);
export const hostClockIds = new Set([
  "etcd.provider-clock-independent",
  "etcd.manager-wall-clock-jumps",
  "spi.snapshot-clock",
]);
export function checkExecutors(fixture, executors) {
  const failures = [];
  for (const { id } of fixture.cases)
    if (typeof executors[id] !== "function")
      failures.push(`missing case function: ${id}`);
  for (const id of Object.keys(executors))
    if (!fixture.cases.some((c) => c.id === id))
      failures.push(`unknown case function: ${id}`);
  return failures;
}
export function checkCoverage(
  fixture,
  reports,
  languages = fixture.languages,
  { allowUnverified = false } = {},
) {
  const failures = [];
  const ids = new Set(fixture.cases.map((item) => item.id));
  const byLanguage = new Map();
  for (const report of reports) {
    if (!fixture.languages.includes(report.language)) {
      failures.push(`unknown language: ${report.language}`);
      continue;
    }
    if (byLanguage.has(report.language)) {
      failures.push(`duplicate language report: ${report.language}`);
      continue;
    }
    byLanguage.set(report.language, report);
  }
  for (const language of languages) {
    if (!fixture.languages.includes(language)) {
      failures.push(`unknown required language: ${language}`);
      continue;
    }
    const report = byLanguage.get(language);
    if (!report || !Array.isArray(report.cases)) {
      failures.push(`${language}: missing result cases`);
      continue;
    }
    const seen = new Set();
    for (const result of report.cases) {
      if (!ids.has(result.id))
        failures.push(`${language}: unknown id ${result.id}`);
      if (seen.has(result.id))
        failures.push(`${language}: duplicate id ${result.id}`);
      seen.add(result.id);
      const permittedUnverified =
        allowUnverified &&
        hostClockIds.has(result.id) &&
        result.status === "unverified" &&
        typeof result.reason === "string" &&
        result.reason.length > 0;
      if (
        (!permittedUnverified && result.status !== "passed") ||
        result.skip === true ||
        result.skipped === true
      )
        failures.push(
          `${language}: ${result.id}: ${result.status} (skip/failure is not coverage)`,
        );
      if (
        fixture.cases.find((c) => c.id === result.id)?.level ===
        "cross-language"
      ) {
        const seenPairs = new Set();
        for (const pair of result.pairs || []) {
          const key = `${pair.writer}×${pair.reader}`;
          if (
            !languages.includes(pair.writer) ||
            !languages.includes(pair.reader)
          )
            failures.push(`${result.id}: unknown pair ${key}`);
          if (seenPairs.has(key))
            failures.push(`${result.id}: duplicate pair ${key}`);
          seenPairs.add(key);
          if (
            pair.status !== "passed" ||
            pair.skip === true ||
            pair.skipped === true
          )
            failures.push(`${result.id}: failed/skipped pair ${key}`);
        }
        for (const writer of languages)
          for (const reader of languages)
            if (!seenPairs.has(`${writer}×${reader}`))
              failures.push(`${result.id}: missing pair ${writer}×${reader}`);
      }
    }
    for (const id of ids)
      if (!seen.has(id)) failures.push(`${language}: missing ${id}`);
  }
  return failures;
}
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href)
  main(async () => {
    const options = args({ language: "string", "allow-unverified": "boolean" });
    if (!options.positionals.length)
      throw new Error(
        "usage: check-conformance-coverage.mjs [--language cpp] result.json ...; defaults to all four languages",
      );
    const fixture = await readJson(fixturePath);
    const reports = await Promise.all(options.positionals.map(readJson));
    const failures = checkCoverage(
      fixture,
      reports,
      options.language ? options.language.split(",") : fixture.languages,
      { allowUnverified: options["allow-unverified"] === true },
    );
    if (failures.length) throw new Error(failures.join("\n"));
    const unverified = reports.reduce(
      (n, r) => n + r.cases.filter((c) => c.status === "unverified").length,
      0,
    );
    console.log(
      `location store conformance coverage: PASS (${fixture.cases.length} IDs per language; ${unverified} unverified)`,
    );
  });
