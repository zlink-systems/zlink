// SPDX-License-Identifier: MPL-2.0

'use strict';

const fs = require('node:fs');
const {
  positiveIntegerEnv,
  defaultClientsForPattern
} = require('./perf_multi_policy');

function explicitClientCount() {
  for (const name of ['PERF_MULTI_CLIENTS', 'PERF_CLIENTS']) {
    const configured = Number(process.env[name] || NaN);
    if (Number.isFinite(configured) && configured > 0) {
      return Math.trunc(configured);
    }
  }
  return null;
}

function currentSoftNofileLimit() {
  if (process.platform !== 'linux') {
    return null;
  }
  try {
    const limits = fs.readFileSync('/proc/self/limits', 'utf8');
    const line = limits.split('\n').find((entry) => entry.startsWith('Max open files'));
    if (!line) {
      return null;
    }
    const fields = line.trim().split(/\s+/);
    if (fields.length < 5 || fields[3] === 'unlimited') {
      return null;
    }
    const soft = Number(fields[3]);
    return Number.isFinite(soft) && soft > 0 ? soft : null;
  } catch {
    return null;
  }
}

function memoryAvailableKb() {
  if (process.platform !== 'linux') {
    return null;
  }
  try {
    const meminfo = fs.readFileSync('/proc/meminfo', 'utf8');
    const line = meminfo.split('\n').find((entry) => entry.startsWith('MemAvailable:'));
    if (!line) {
      return null;
    }
    const match = line.match(/^MemAvailable:\s+(\d+)\s+kB$/);
    if (!match) {
      return null;
    }
    return Number(match[1]);
  } catch {
    return null;
  }
}

function resolveMemoryMaxClients() {
  const availableKb = memoryAvailableKb();
  if (!Number.isFinite(availableKb) || availableKb <= 0) {
    return null;
  }
  const budgetPct = Number(process.env.PERF_MULTI_MEMORY_BUDGET_PCT || 70);
  const baseMb = Number(process.env.PERF_MULTI_MEMORY_BASE_MB || 512);
  const perClientKb = Number(process.env.PERF_MULTI_MEMORY_PER_CLIENT_KB || 1024);
  if (!Number.isFinite(budgetPct) || budgetPct < 1 || budgetPct > 95) {
    return null;
  }
  if (!Number.isFinite(baseMb) || baseMb < 0) {
    return null;
  }
  if (!Number.isFinite(perClientKb) || perClientKb < 1) {
    return null;
  }
  const usableKb = Math.trunc(availableKb * budgetPct / 100);
  const baseKb = Math.trunc(baseMb * 1024);
  if (usableKb <= baseKb) {
    return 1;
  }
  return Math.max(1, Math.trunc((usableKb - baseKb) / perClientKb));
}

function resolvePatternClients(patternName, options, clientSource) {
  const requested = clientSource === 'policy'
    ? defaultClientsForPattern(patternName)
    : options.clients;
  let effectiveClients = requested;

  if (!Number.isFinite(requested) || requested <= 0) {
    throw new Error(`invalid multi clients for ${patternName}: ${requested}`);
  }

  if (process.env.PERF_SKIP_MEMORY_CHECK !== '1') {
    const maxClients = resolveMemoryMaxClients();
    if (Number.isFinite(maxClients) && maxClients > 0) {
      if (clientSource === 'policy') {
        effectiveClients = Math.min(requested, maxClients);
      } else if (requested > maxClients) {
        return {
          clients: requested,
          skipReason: `memory_guard_clients=${requested},max_clients=${maxClients},mem_available_kb=${memoryAvailableKb()},budget_pct=${process.env.PERF_MULTI_MEMORY_BUDGET_PCT || 70},base_mb=${process.env.PERF_MULTI_MEMORY_BASE_MB || 512},per_client_kb=${process.env.PERF_MULTI_MEMORY_PER_CLIENT_KB || 1024}`
        };
      }
    }
  }

  if (process.env.PERF_SKIP_NOFILE_CHECK !== '1') {
    const required = effectiveClients * 3 + 4096;
    const softLimit = currentSoftNofileLimit();
    if (Number.isFinite(softLimit) && softLimit < required) {
      return {
        clients: effectiveClients,
        skipReason: `nofile_guard_clients=${effectiveClients},required=${required},soft=${softLimit}`
      };
    }
  }

  return {
    clients: effectiveClients,
    skipReason: ''
  };
}

function resolveTransportClients(patternName, transport, clients) {
  if (patternName !== 'MULTI_STREAM' || transport === 'tcp') {
    return clients;
  }
  const nonTcpMax = positiveIntegerEnv('PERF_STREAM_NON_TCP_CLIENTS_MAX', 'PERF_MULTI_STREAM_NON_TCP_CLIENTS_MAX') ?? 10000;
  return Math.min(clients, nonTcpMax);
}

module.exports = {
  explicitClientCount,
  resolvePatternClients,
  resolveTransportClients
};
