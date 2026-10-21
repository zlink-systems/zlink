'use strict';

// Actual API calls and V8 allocation samples, collected in separate passes.
// node perf/request-backpressure-census.cjs heap|counters framework|binding OUTPUT_DIR
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { createHook } = require('node:async_hooks');
const { Session } = require('node:inspector');
const fixture = require('./request-backpressure.cjs');
const COUNT = 1000;

function counters() {
  const calls = Object.create(null);
  let recording = false;
  let nested = false;
  const original = Object.create(null);
  const previousStackLimit = Error.stackTraceLimit;
  Error.stackTraceLimit = 48;
  function record(kind, skip) {
    if (!recording || nested) return;
    nested = true;
    try {
      const holder = {};
      Error.captureStackTrace(holder, skip);
      const stack = holder.stack.split('\n').slice(1).join('\n');
      const key = `${kind}\n${stack}`;
      if (calls[key]) calls[key].count++;
      else calls[key] = { kind, stack, count: 1 };
    } finally {
      nested = false;
    }
  }
  const hook = createHook({
    init: function promiseInit(_id, type) {
      if (type === 'PROMISE') record('PROMISE', promiseInit);
    }
  });
  for (const name of ['get', 'set', 'has', 'delete', 'clear']) {
    original[name] = Map.prototype[name];
    Map.prototype[name] = function observedMapCall(...args) {
      record(`Map.${name}`, observedMapCall);
      return original[name].apply(this, args);
    };
  }
  hook.enable();
  return {
    start() {
      recording = true;
    },
    stop() {
      recording = false;
    },
    rows() {
      return Object.values(calls);
    },
    close() {
      recording = false;
      hook.disable();
      for (const name of Object.keys(original)) Map.prototype[name] = original[name];
      Error.stackTraceLimit = previousStackLimit;
    }
  };
}

function counterPositiveControl() {
  const capture = counters();
  function knownMapOperations() {
    const map = new Map();
    for (let index = 0; index < 7; index++) {
      map.set(index, index);
      map.get(index);
      map.has(index);
      map.delete(index);
      map.clear();
    }
  }
  function knownPromiseCreation() {
    return new Promise((resolve) => resolve());
  }
  let rows;
  try {
    capture.start();
    knownMapOperations();
    knownPromiseCreation();
    capture.stop();
    rows = capture.rows();
  } finally {
    capture.close();
  }
  for (const kind of ['get', 'set', 'has', 'delete', 'clear']) {
    assert.equal(
      rows
        .filter((row) => row.kind === `Map.${kind}` && row.stack.includes('knownMapOperations'))
        .reduce((sum, row) => sum + row.count, 0),
      7
    );
  }
  assert.equal(
    rows
      .filter((row) => row.kind === 'PROMISE' && row.stack.includes('knownPromiseCreation'))
      .reduce((sum, row) => sum + row.count, 0),
    1
  );
  return { passed: true, rows };
}

function classify(frame) {
  if (/\/packages\/(framework|nestjs|stream-wire|stream-connector)\//.test(frame))
    return 'framework';
  if (/zlink.*(?:binding|node_modules\/.*zlink)|\/bindings\/node\//.test(frame)) return 'binding';
  if (/request-backpressure(?:-census)?\.cjs/.test(frame)) return 'fixture';
  return 'other';
}

function heapSummary(profile) {
  const direct = Object.create(null);
  const owners = Object.create(null);
  const chains = Object.create(null);
  const nodes = Object.create(null);
  const frameName = (frame) =>
    `${frame.functionName || '(anonymous)'} ${frame.url}:${frame.lineNumber + 1}:${frame.columnNumber + 1}`;
  function visit(node, ancestors) {
    const frame = frameName(node.callFrame);
    const chain = [...ancestors, frame];
    nodes[node.id] = { frame, chain, selfSize: node.selfSize };
    for (const child of node.children) visit(child, chain);
  }
  visit(profile.head, []);
  function add(table, key, bytes, samples) {
    const row = table[key] ?? (table[key] = { frame: key, bytes: 0, samples: 0 });
    row.bytes += bytes;
    row.samples += samples;
  }
  const selfBytes = Object.create(null);
  for (const node of Object.values(nodes))
    selfBytes[node.frame] = (selfBytes[node.frame] ?? 0) + node.selfSize;
  // V8 supplies both sample sizes and node selfSize; preserve both measures.
  for (const sample of profile.samples) {
    const node = nodes[sample.nodeId];
    add(direct, node.frame, sample.size, 1);
    const owner = [...node.chain].reverse().find((frame) => classify(frame) === 'framework');
    add(owners, owner ?? `[${classify(node.frame)}] ${node.frame}`, sample.size, 1);
    const entry =
      chains[sample.nodeId] ??
      (chains[sample.nodeId] = {
        bytes: 0,
        samples: 0,
        nodeId: sample.nodeId,
        selfSize: node.selfSize,
        chain: node.chain
      });
    entry.bytes += sample.size;
    entry.samples++;
  }
  function rows(table) {
    return Object.values(table)
      .sort((left, right) => right.bytes - left.bytes)
      .map((row) => ({
        ...row,
        bytesPerRequest: row.bytes / COUNT,
        samplesPerRequest: row.samples / COUNT
      }));
  }
  const fixedStatusChains = Object.values(chains).filter((row) =>
    row.chain.some((frame) => /^(status|snapshot) .*request-backpressure\.cjs:/.test(frame))
  );
  return {
    fixedStatusObservation: {
      bytes: fixedStatusChains.reduce((sum, row) => sum + row.bytes, 0),
      samples: fixedStatusChains.reduce((sum, row) => sum + row.samples, 0),
      chains: fixedStatusChains
    },
    meaning:
      'V8 allocation samples, not an exact object census. Direct and nearest Framework owner partitions each count a sample once. Chains overlap and are not additive.',
    direct: rows(direct).map((row) => ({
      ...row,
      sourceGroup: sourceGroup(row.frame),
      selfSize: selfBytes[row.frame],
      selfSizePerRequest: selfBytes[row.frame] / COUNT
    })),
    nearestFrameworkOwner: rows(owners).map((row) => ({
      ...row,
      sourceGroup: sourceGroup(row.frame)
    })),
    chains: Object.values(chains)
  };
}

function sourceGroup(frame) {
  if (/\/stream-wire\/.*(?:json|contract)/.test(frame)) return 'json-excluded';
  return classify(frame);
}

function counterSummary(rows) {
  const direct = Object.create(null);
  const nearest = Object.create(null);
  function add(table, row, owner) {
    const key = `${row.observation}\n${row.kind}\n${owner}`;
    const entry =
      table[key] ??
      (table[key] = {
        observation: row.observation,
        kind: row.kind,
        owner,
        sourceGroup: sourceGroup(owner),
        count: 0
      });
    entry.count += row.count;
  }
  const attributedCalls = rows.map((row) => {
    const frames = row.stack.split('\n');
    const directScriptCaller =
      frames.find((frame) => /\/.+:\d+:\d+/.test(frame) && !/node:/.test(frame)) ??
      '[no script caller]';
    const nearestFrameworkOwner =
      frames.find((frame) => classify(frame) === 'framework') ?? '[no Framework owner]';
    const observation = /pressureMetrics|snapshot|status \(/.test(row.stack)
      ? 'status-metrics-fixed-observation'
      : 'request-path';
    const attributed = { ...row, directScriptCaller, nearestFrameworkOwner, observation };
    add(direct, attributed, directScriptCaller);
    add(nearest, attributed, nearestFrameworkOwner);
    return attributed;
  });
  const summarize = (table) =>
    Object.values(table)
      .sort((left, right) => right.count - left.count)
      .map((row) => ({ ...row, perRequest: row.count / COUNT }));
  return {
    meaning:
      'Exact calls in this capture. directScriptCaller and nearestFrameworkOwner are alternative partitions; do not add them. WeakMap is outside the Map-only capture. Pump/timer/yield counts depend on scheduling.',
    directScriptCaller: summarize(direct),
    nearestFrameworkOwner: summarize(nearest),
    attributedCalls
  };
}

async function main() {
  const [mode, name, output] = process.argv.slice(2);
  assert.ok(['heap', 'counters'].includes(mode));
  assert.ok(['framework', 'binding'].includes(name));
  assert.ok(output);
  fs.mkdirSync(output, { recursive: true });
  const write = (suffix, value) =>
    fs.writeFileSync(
      path.join(output, `${name}-${mode}-${suffix}.json`),
      JSON.stringify(value, (_key, item) => (typeof item === 'bigint' ? String(item) : item), 2)
    );
  if (mode === 'counters') write('control', counterPositiveControl());
  const pair = await fixture[`${name}Pair`]();
  try {
    await fixture.run(pair, 256, false);
    let result;
    if (mode === 'counters') {
      const capture = counters();
      let rows;
      try {
        capture.start();
        result = await fixture.run(pair, COUNT, false);
        capture.stop();
        rows = capture.rows();
      } finally {
        capture.close();
      }
      write('raw', rows);
      write('summary', counterSummary(rows));
    } else {
      const session = new Session();
      session.connect();
      const post = (method, params) =>
        new Promise((resolve, reject) =>
          session.post(method, params, (error, value) => (error ? reject(error) : resolve(value)))
        );
      try {
        await post('HeapProfiler.startSampling', {
          samplingInterval: 1,
          includeObjectsCollectedByMajorGC: true,
          includeObjectsCollectedByMinorGC: true
        });
        result = await fixture.run(pair, COUNT, false);
        const { profile } = await post('HeapProfiler.stopSampling');
        write('raw', profile);
        write('summary', heapSummary(profile));
      } finally {
        session.disconnect();
      }
    }
    assert.equal(result.completed, COUNT);
    // Existing status snapshots include fixed-cost metrics observations; these stay
    // visible in raw stacks and must not be interpreted as per-request work.
    write('workload', {
      node: process.version,
      mode,
      name,
      note: 'Timing under instrumentation is not performance evidence. run includes two status snapshots and final validation.',
      result
    });
    console.log(JSON.stringify({ mode, name, completed: result.completed, output }));
  } finally {
    await pair.close();
  }
}

module.exports = { heapSummary, counterSummary };

if (require.main === module)
  main().catch((error) => {
    console.error(error);
    process.exitCode = 1;
  });
