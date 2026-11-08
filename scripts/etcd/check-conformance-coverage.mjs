#!/usr/bin/env node
import { fileURLToPath, pathToFileURL } from 'node:url';
import { args, main, readJson } from './common.mjs';

export const fixturePath = fileURLToPath(new URL('../../framework/runtime/conformance/location-store-provider-v1.json', import.meta.url));
export function checkCoverage(fixture, reports, languages = fixture.languages) {
  const failures = [];
  const ids = new Set(fixture.cases.map((item) => item.id));
  const byLanguage = new Map();
  for (const report of reports) {
    if (!fixture.languages.includes(report.language)) { failures.push(`unknown language: ${report.language}`); continue; }
    if (byLanguage.has(report.language)) { failures.push(`duplicate language report: ${report.language}`); continue; }
    byLanguage.set(report.language, report);
  }
  for (const language of languages) {
    if (!fixture.languages.includes(language)) { failures.push(`unknown required language: ${language}`); continue; }
    const report = byLanguage.get(language);
    if (!report || !Array.isArray(report.cases)) { failures.push(`${language}: missing result cases`); continue; }
    const seen = new Set();
    for (const result of report.cases) {
      if (!ids.has(result.id)) failures.push(`${language}: unknown id ${result.id}`);
      if (seen.has(result.id)) failures.push(`${language}: duplicate id ${result.id}`);
      seen.add(result.id);
      if (result.status !== 'passed' || result.skip === true || result.skipped === true) failures.push(`${language}: ${result.id}: ${result.status} (skip/failure is not coverage)`);
    }
    for (const id of ids) if (!seen.has(id)) failures.push(`${language}: missing ${id}`);
  }
  return failures;
}
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) main(async () => {
  const options = args({ language: 'string' });
  if (!options.positionals.length) throw new Error('usage: check-conformance-coverage.mjs [--language cpp] result.json ...; defaults to all four languages');
  const fixture = await readJson(fixturePath);
  const failures = checkCoverage(fixture, await Promise.all(options.positionals.map(readJson)), options.language ? [options.language] : fixture.languages);
  if (failures.length) throw new Error(failures.join('\n'));
  console.log(`location store conformance coverage: PASS (${fixture.cases.length} IDs per language)`);
});
