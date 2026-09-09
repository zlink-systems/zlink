// SPDX-License-Identifier: MPL-2.0
'use strict';

function argValue(argv, name, fallback) {
  const index = argv.indexOf(name);
  if (index >= 0 && index + 1 < argv.length) return argv[index + 1];
  return fallback;
}

function argInt(argv, name, fallback) {
  const raw = argValue(argv, name, null);
  if (raw === null) return fallback;
  const value = Number.parseInt(raw, 10);
  return Number.isFinite(value) ? value : fallback;
}

module.exports = { argValue, argInt };
