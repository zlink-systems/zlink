// SPDX-License-Identifier: MPL-2.0

'use strict';

const fs = require('node:fs');
const path = require('node:path');

function buildEffectiveOptions(options, extraLines = []) {
  const patterns = options.patterns || options.pattern || '-';
  const lines = [
    `- lang: ${options.lang}`,
    `- suite: ${options.suite}`,
    `- runs: ${options.runs}`,
    `- patterns: ${patterns}`,
    `- transports: ${(options.transports || []).join(',') || '-'}`,
    `- msg_sizes: ${(options.msgSizes || []).join(',') || '-'}`,
    `- pin_cpu: ${options.pinCpu ? 'on' : 'off'}`
  ];
  if (typeof options.duration !== 'undefined') {
    lines.push(`- duration_seconds: ${options.duration}`);
  }
  if (typeof options.clientsLabel !== 'undefined') {
    lines.push(`- clients: ${options.clientsLabel}`);
  } else if (typeof options.clients !== 'undefined') {
    lines.push(`- clients: ${options.clients}`);
  }
  if (options.resultsTag) {
    lines.push(`- results_tag: ${options.resultsTag}`);
  }
  if (Array.isArray(extraLines)) {
    lines.push(...extraLines);
  }
  return lines;
}

function throughputUnit(pattern) {
  return pattern === 'DEALER_ROUTER_REQREP'
    || pattern === 'ROUTER_ROUTER_REQREP'
    || pattern === 'MULTI_DEALER_ROUTER'
    || pattern === 'MULTI_DEALER_ROUTER_SENDSEND'
    || pattern === 'MULTI_DEALER_ROUTER_REQREP'
    || pattern === 'MULTI_ROUTER_ROUTER'
    || pattern === 'MULTI_ROUTER_ROUTER_SENDSEND'
    || pattern === 'MULTI_ROUTER_ROUTER_REQREP'
    || pattern === 'MULTI_STREAM'
    ? 'Kops/s'
    : 'Kmsg/s';
}

function formatTableHeader() {
  return [
    '| Size     |         Throughput |      Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) |',
    '|----------|--------------------|----------------|---------------|---------------|---------------|'
  ];
}

function formatTableRow(row) {
  const throughput = `${(row.metrics.throughput / 1000).toFixed(3).padStart(8)} ${throughputUnit(row.pattern)}`;
  const bandwidth = `${row.metrics.bandwidth.toFixed(3).padStart(10)} MB/s`;
  return `| ${`${row.msgSize}B`.padEnd(8)} | ${throughput.padStart(16)} | ${bandwidth.padStart(12)} | ${`${row.metrics.latency.toFixed(3).padStart(9)} ms`.padStart(12)} | ${`${row.metrics.latency_p95.toFixed(3).padStart(9)} ms`.padStart(12)} | ${`${row.metrics.latency_p99.toFixed(3).padStart(9)} ms`.padStart(12)} |`;
}

function formatFailureRow(msgSize, label = 'FAIL') {
  const cell = String(label).padStart(16);
  return `| ${String(msgSize).padEnd(8)}B | ${cell} | ${'FAIL'.padStart(10)} | ${'FAIL'.padStart(13)} | ${'FAIL'.padStart(13)} | ${'FAIL'.padStart(13)} |`;
}

function formatTableRows(rows) {
  return [
    ...formatTableHeader(),
    ...rows.map((row) => formatTableRow(row))
  ];
}

function patternDirection(pattern) {
  return throughputUnit(pattern) === 'Kops/s' ? 'echo' : 'one-way';
}

function completionLines(status, expected, actual) {
  return [
    `- status: ${status}`,
    `- expected_result_lines: ${expected}`,
    `- actual_result_lines: ${actual}`
  ];
}

function formatTimestamp(date) {
  const pad = (value) => String(value).padStart(2, '0');
  return `${date.getFullYear()}${pad(date.getMonth() + 1)}${pad(date.getDate())}_${pad(date.getHours())}${pad(date.getMinutes())}${pad(date.getSeconds())}`;
}

function retainLatestFiles(dir, maxFiles) {
  if (!fs.existsSync(dir)) {
    return;
  }
  const files = fs.readdirSync(dir).sort();
  while (files.length > maxFiles) {
    const oldest = files.shift();
    fs.rmSync(path.join(dir, oldest), { force: true });
  }
}

function platformName() {
  if (process.platform === 'win32') {
    return 'windows';
  }
  if (process.platform === 'darwin') {
    return 'macos';
  }
  return 'linux';
}

function writeReport(reportDir, lang, suite, options, renderedSections, completionLines) {
  fs.mkdirSync(reportDir, { recursive: true });
  const maxFiles = suite === 'multi'
    ? Number(process.env.PERF_RESULTS_MAX_FILES || 100)
    : 100;
  const keepCount = Number.isFinite(maxFiles) && maxFiles > 0 ? maxFiles : 100;
  const stamp = formatTimestamp(new Date());
  const tag = options.resultsTag ? `_${options.resultsTag}` : '';
  const file = path.join(
    reportDir,
    `perf_${lang}_${suite}_${platformName()}_${stamp}${tag}.txt`
  );
  const sections = Array.isArray(renderedSections) ? renderedSections : [];
  const completion = Array.isArray(completionLines) ? completionLines : [];
  const content = [
    '## Effective Options (start)',
    ...buildEffectiveOptions({ ...options, lang, suite }),
    '',
    ...sections,
    ...(sections.length > 0 ? [''] : []),
    '## Effective Options (result)',
    ...buildEffectiveOptions({ ...options, lang, suite }),
    '',
    ...completion
  ].join('\n');
  fs.writeFileSync(file, `${content}\n`, 'utf8');
  if (options.output) {
    fs.writeFileSync(options.output, `${content}\n`, 'utf8');
  }
  retainLatestFiles(reportDir, keepCount);
  return file;
}

module.exports = {
  buildEffectiveOptions,
  completionLines,
  formatFailureRow,
  formatTableRows,
  formatTableHeader,
  formatTableRow,
  patternDirection,
  writeReport
};
