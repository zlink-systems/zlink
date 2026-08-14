// SPDX-License-Identifier: MPL-2.0

'use strict';

function parseAutoHwmDetailLine(line) {
  const stripped = String(line || '').trim();
  if (!stripped.startsWith('AUTO_HWM_DETAIL,')) {
    return null;
  }
  const fields = {};
  for (const item of stripped.split(',').slice(1)) {
    const pos = item.indexOf('=');
    if (pos < 0) {
      continue;
    }
    fields[item.slice(0, pos).trim()] = item.slice(pos + 1).trim();
  }
  return Object.keys(fields).length > 0 ? fields : null;
}

function bytesToKbDisplay(value) {
  const parsed = Number(value);
  if (!Number.isFinite(parsed) || parsed < 0) {
    return '?';
  }
  if (parsed <= 0) {
    return '0';
  }
  if ((parsed % 1024) === 0) {
    return String(parsed / 1024);
  }
  return (parsed / 1024).toFixed(1);
}

function cellWidths(rows, columns) {
  return columns.map(([header, key]) => rows.reduce(
    (width, row) => Math.max(width, String(row[key] ?? '?').length),
    header.length
  ));
}

function autoHwmDetailTableLines(rows, pattern) {
  const patternRows = rows
    .filter((row) => String(row.pattern || '').toUpperCase() === pattern.toUpperCase());
  if (patternRows.length === 0) {
    return [];
  }

  const seen = new Set();
  const displayRows = [];
  for (const row of patternRows) {
    const display = {
      ...row,
      sndbuf_kb: bytesToKbDisplay(row.effective_sndbuf),
      rcvbuf_kb: bytesToKbDisplay(row.effective_rcvbuf),
    };
    const key = [
      'msg_size',
      'component',
      'owner',
      'socket',
      'socket_type',
      'role',
      'sndhwm',
      'rcvhwm',
      'sndbuf_kb',
      'rcvbuf_kb',
      'snd_pending_bytes',
      'rcv_pending_bytes',
    ].map((name) => display[name] ?? '').join('\0');
    if (seen.has(key)) {
      continue;
    }
    seen.add(key);
    displayRows.push(display);
  }
  if (displayRows.length === 0) {
    return [];
  }
  displayRows.sort((lhs, rhs) => {
    const sizeDiff = Number(lhs.msg_size || 0) - Number(rhs.msg_size || 0);
    if (sizeDiff !== 0) return sizeDiff;
    return String(lhs.component || '').localeCompare(String(rhs.component || ''))
      || String(lhs.owner || '').localeCompare(String(rhs.owner || ''))
      || String(lhs.socket || '').localeCompare(String(rhs.socket || ''));
  });

  const columns = [
    ['Size(B)', 'msg_size'],
    ['Component', 'component'],
    ['Owner', 'owner'],
    ['Socket', 'socket'],
    ['Type', 'socket_type'],
    ['Role', 'role'],
    ['SNDHWM', 'sndhwm'],
    ['RCVHWM', 'rcvhwm'],
    ['SNDBUF(KB)', 'sndbuf_kb'],
    ['RCVBUF(KB)', 'rcvbuf_kb'],
    ['SndPending(B)', 'snd_pending_bytes'],
    ['RcvPending(B)', 'rcv_pending_bytes'],
  ];
  const widths = cellWidths(displayRows, columns);
  const header = '| ' + columns
    .map(([name], index) => name.padEnd(widths[index]))
    .join(' | ') + ' |';
  const separator = '|-' + widths.map((width) => '-'.repeat(width)).join('-|-') + '-|';
  return [
    '',
    '## Auto-HWM Detail',
    `- pattern: ${pattern}`,
    header,
    separator,
    ...displayRows.map((row) => '| ' + columns
      .map(([, key], index) => String(row[key] ?? '?').padEnd(widths[index]))
      .join(' | ') + ' |')
  ];
}

module.exports = {
  autoHwmDetailTableLines,
  parseAutoHwmDetailLine,
};
