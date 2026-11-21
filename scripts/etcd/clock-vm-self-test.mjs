import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { createRequire } from "node:module";
const require = createRequire(import.meta.url);
const { ClockVM } = require("./clock-vm.cjs");

test("real guest rejects absent executor, unapplied clock changes and clock restoration", async () => {
  const directory = fs.mkdtempSync(
    path.join(
      process.env.ZLINK_ETCD_TEST_LOG_DIR || os.tmpdir(),
      "clock-negative-",
    ),
  );
  const vm = new ClockVM(directory, "negative");
  const absent = new ClockVM(directory, "absent");
  absent.config.qemu = path.join(directory, "absent-qemu");
  try {
    await assert.rejects(absent.start(), /ENOENT/);
    await absent.close();
    await vm.start();
    const observe = vm.observe.bind(vm);
    vm.observe = () => observe(); // Deliberately omit the settimeofday request in a real guest.
    await assert.rejects(vm.move(-86400000), /did not take effect/);
    vm.observe = observe;
    await vm.move(-86400000);
    await vm.verifyClock();
    await vm.observe(Date.now()); // Deliberately restore guest realtime like a time daemon.
    await assert.rejects(vm.verifyClock(), /restored by time synchronization/);
    console.log(
      "NEGATIVE VM: missing executor rejected; unapplied change rejected; restored clock rejected; actual clock change accepted",
    );
  } finally {
    await vm.close();
  }
});

test("runner rejects missing VM even when unverified coverage is allowed", () => {
  const directory = fs.mkdtempSync(
    path.join(
      process.env.ZLINK_ETCD_TEST_LOG_DIR || os.tmpdir(),
      "clock-unavailable-",
    ),
  );
  const env = { ...process.env, ZLINK_ETCD_TEST_LOG_DIR: directory };
  delete env.ZLINK_CLOCK_VM_CONFIG;
  const result = require("node:child_process").spawnSync(
    process.execPath,
    [
      path.join(import.meta.dirname, "conformance-runner.mjs"),
      "--no-mutations",
      "--allow-unverified",
      "--cases",
      "etcd.provider-clock-independent",
    ],
    { env, encoding: "utf8", timeout: 180000 },
  );
  fs.writeFileSync(
    path.join(directory, "runner.log"),
    result.stdout + result.stderr,
  );
  assert.equal(result.status, 1, result.stderr);
  const report = JSON.parse(
    fs.readFileSync(path.join(directory, "node-conformance.json"), "utf8"),
  );
  assert.equal(report.cases[0].status, "failed");
  assert.match(report.cases[0].reason, /requires ZLINK_CLOCK_VM_CONFIG/);
  assert.deepEqual(
    JSON.parse(
      fs.readFileSync(path.join(directory, "unverified.json"), "utf8"),
    ),
    [],
  );
  assert.deepEqual(
    JSON.parse(fs.readFileSync(path.join(directory, "cleanup.json"), "utf8")),
    { containers: 0, networks: 0, processes: 0, temporaryDirectories: 0 },
  );
});

test("guest artifact preparation rejects a missing kernel", () => {
  const directory = fs.mkdtempSync(
    path.join(
      process.env.ZLINK_ETCD_TEST_LOG_DIR || os.tmpdir(),
      "clock-bad-kernel-",
    ),
  );
  const result = require("node:child_process").spawnSync(
    "bash",
    [
      path.join(import.meta.dirname, "prepare-clock-vm.sh"),
      directory,
      path.join(directory, "missing-kernel"),
      process.cwd(),
      "unused",
      process.execPath,
      process.cwd(),
    ],
    { encoding: "utf8" },
  );
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /missing-kernel/);
  assert.equal(fs.existsSync(path.join(directory, "config.json")), false);
});
