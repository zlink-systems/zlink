const assert = require('node:assert/strict');
const test = require('node:test');

const framework = require('../../packages/framework/dist/internal');

const profiles = [
  [framework.ZLinkApplicationHwmProfile.Compact, 20n],
  [framework.ZLinkApplicationHwmProfile.LowLatency, 50n],
  [framework.ZLinkApplicationHwmProfile.Balanced, 100n],
  [framework.ZLinkApplicationHwmProfile.Throughput, 200n]
];

test('Application HWM uses every profile ratio with an explicit process budget', () => {
  for (const [profile, expected] of profiles) {
    assert.equal(resolve({
      applicationHwmProfile: profile,
      processMemoryLimitBytes: 1000n
    }), expected);
  }
});

test('Application HWM chooses the managed heap limit when it is the only finite limit', () => {
  assert.equal(resolve({
    applicationHwmProfile: framework.ZLinkApplicationHwmProfile.Balanced
  }, {
    managedHeapLimitBytes: 700n,
    physicalMemoryBytes: 64_000n
  }), 70n);
});

test('Application HWM chooses the OS limit when it is the only finite limit', () => {
  assert.equal(resolve({
    applicationHwmProfile: framework.ZLinkApplicationHwmProfile.Balanced
  }, {
    osLimitBytes: 900n,
    physicalMemoryBytes: 64_000n
  }), 90n);
});

test('Application HWM uses the smaller OS and managed heap limit', () => {
  assert.equal(framework.resolveEffectiveMemoryBudget({
    osLimitBytes: 2000n,
    managedHeapLimitBytes: 700n,
    physicalMemoryBytes: 64_000n
  }), 700n);
  assert.equal(resolve({
    applicationHwmProfile: framework.ZLinkApplicationHwmProfile.Balanced
  }, {
    osLimitBytes: 2000n,
    managedHeapLimitBytes: 700n,
    physicalMemoryBytes: 64_000n
  }), 70n);
});

test('Application HWM falls back to physical memory when no finite runtime limit exists', () => {
  assert.equal(framework.resolveEffectiveMemoryBudget({
    physicalMemoryBytes: 1234n
  }), 1234n);
  assert.equal(resolve({
    applicationHwmProfile: framework.ZLinkApplicationHwmProfile.Balanced
  }, {
    physicalMemoryBytes: 1234n
  }), 123n);
});

test('explicit ProcessMemoryLimitBytes is authoritative over detected limits', () => {
  assert.equal(resolve({
    applicationHwmProfile: framework.ZLinkApplicationHwmProfile.Balanced,
    processMemoryLimitBytes: 1000n
  }, {
    osLimitBytes: 10n,
    managedHeapLimitBytes: 20n,
    physicalMemoryBytes: 30n
  }), 100n);
});

test('Application HWM Auto starts with a positive host fallback in the current runtime', () => {
  const value = resolve({
    applicationHwmProfile: framework.ZLinkApplicationHwmProfile.Balanced
  });
  assert.equal(value > 0n, true);
});

test('ApplicationHwmBytes zero is unlimited and a positive value is a fixed host budget', () => {
  assert.equal(resolve({
    applicationHwmBytes: 0n,
    applicationHwmProfile: framework.ZLinkApplicationHwmProfile.Balanced,
    processMemoryLimitBytes: 1n
  }), 0n);
  assert.equal(resolve({
    applicationHwmBytes: 37n,
    applicationHwmProfile: framework.ZLinkApplicationHwmProfile.Throughput,
    processMemoryLimitBytes: 1n
  }), 37n);

  const unlimited = new framework.ZLinkInboundDispatchBudget(0n);
  unlimited.enqueue(1_000_000n);
  assert.equal(unlimited.receivePaused, false);

  const fixed = new framework.ZLinkInboundDispatchBudget(37n);
  fixed.enqueue(37n);
  assert.equal(fixed.receivePaused, true);
});

test('negative ApplicationHwmBytes and non-positive Auto results are startup errors', () => {
  assert.throws(
    () => resolve({
      applicationHwmBytes: -1n,
      applicationHwmProfile: framework.ZLinkApplicationHwmProfile.Balanced
    }),
    /zero or positive/
  );
  assert.throws(
    () => resolve({
      applicationHwmProfile: framework.ZLinkApplicationHwmProfile.Compact,
      processMemoryLimitBytes: 1n
    }),
    /non-positive byte limit/
  );
  assert.throws(
    () => resolve({
      applicationHwmProfile: framework.ZLinkApplicationHwmProfile.Balanced,
      processMemoryLimitBytes: 0n
    }, { physicalMemoryBytes: 64_000n }),
    /positive effective memory budget/
  );
});

function resolve(options, sources) {
  return framework.resolveApplicationHwm({
    applicationHwmBytes: undefined,
    applicationHwmProfile: framework.ZLinkApplicationHwmProfile.Balanced,
    processMemoryLimitBytes: undefined,
    ...options
  }, sources);
}
