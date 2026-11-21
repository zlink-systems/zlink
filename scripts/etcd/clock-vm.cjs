const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const net = require("node:net");
const { spawn } = require("node:child_process");
const { once } = require("node:events");
const quote = (s) => "'" + String(s).replaceAll("'", "'\\''") + "'";

// QEMU owns isolation and availability; callers never convert preparation failures to coverage.
class ClockVM {
  constructor(directory, label) {
    const file = process.env.ZLINK_CLOCK_VM_CONFIG;
    if (!file)
      throw new Error(
        "host clock VM requires ZLINK_CLOCK_VM_CONFIG (QEMU kernel, initrd and SSH key)",
      );
    this.config = JSON.parse(fs.readFileSync(file, "utf8"));
    for (const key of ["kernel", "initrd", "privateKey"])
      assert.ok(
        fs.existsSync(this.config[key]),
        `missing VM ${key}: ${this.config[key]}`,
      );
    this.directory = directory;
    this.label = label;
    this.evidenceFile = path.join(directory, `${label}-clock.jsonl`);
    fs.writeFileSync(this.evidenceFile, "");
  }
  record(value) {
    fs.appendFileSync(this.evidenceFile, JSON.stringify(value) + "\n");
  }
  async start() {
    const listener = net.createServer();
    listener.listen(0, "127.0.0.1");
    await once(listener, "listening");
    this.port = listener.address().port;
    await new Promise((resolve) => listener.close(resolve));
    const c = this.config;
    this.vm = spawn(
      c.qemu || "qemu-system-x86_64",
      [
        "-nodefaults",
        "-no-reboot",
        "-display",
        "none",
        "-m",
        "1024",
        "-smp",
        "2",
        "-accel",
        c.accel || "kvm",
        "-kernel",
        c.kernel,
        "-initrd",
        c.initrd,
        "-append",
        "console=ttyS0 panic=-1 rdinit=/init",
        "-serial",
        "stdio",
        "-fsdev",
        `local,id=host,path=${c.hostRoot || "/"},security_model=none,multidevs=remap,readonly=on`,
        "-device",
        "virtio-9p-pci,fsdev=host,mount_tag=host",
        "-netdev",
        `user,id=net,hostfwd=tcp:127.0.0.1:${this.port}-:22`,
        "-device",
        "virtio-net-pci,netdev=net,romfile=",
        ...(c.args || []),
      ],
      { env: { ...process.env, ...c.env }, stdio: ["ignore", "pipe", "pipe"] },
    );
    this.output = "";
    this.vm.stdout.on("data", (b) => {
      this.output = (this.output + b).slice(-32768);
      fs.appendFileSync(path.join(this.directory, `${this.label}-boot.log`), b);
    });
    this.vm.stderr.on("data", (b) => {
      this.output = (this.output + b).slice(-32768);
      fs.appendFileSync(path.join(this.directory, `${this.label}-boot.log`), b);
    });
    this.exited = new Promise((resolve) => {
      this.vm.once("error", (e) => {
        this.failure = e;
        resolve();
      });
      this.vm.once("exit", (code, signal) => {
        this.failure = new Error(`QEMU exited ${code}/${signal}`);
        resolve();
      });
    });
    const deadline = performance.now() + 60000;
    let last;
    while (!this.initial) {
      if (this.failure)
        throw new Error(`${this.failure.message}: ${this.output}`, {
          cause: this.failure,
        });
      if (performance.now() >= deadline)
        throw new Error(`guest SSH startup failed: ${this.output}`, {
          cause: last,
        });
      try {
        this.initial = await this.observe();
      } catch (error) {
        last = error;
        await new Promise((resolve) => setTimeout(resolve, 100));
      }
    }
    const host = fs
      .readFileSync("/proc/sys/kernel/random/boot_id", "utf8")
      .trim();
    assert.notEqual(
      this.initial.boot,
      host,
      "clock target must be a different kernel",
    );
    return this;
  }
  endpoints(endpoints) {
    return endpoints.map((endpoint) => {
      const url = new URL(endpoint);
      if (["127.0.0.1", "localhost"].includes(url.hostname))
        url.hostname = "10.0.2.2";
      return url.href.replace(/\/$/, "");
    });
  }
  command(config) {
    // Preserve SDK discovery used by the existing adapter launchers, with guest-private writes.
    const environment = Object.fromEntries(
      [
        "PATH",
        "JAVA_HOME",
        "DOTNET_ROOT",
        "LD_LIBRARY_PATH",
        "NODE_PATH",
        "ZLINK_LIBRARY_PATH",
      ]
        .filter((key) => process.env[key] !== undefined)
        .map((key) => [key, process.env[key]]),
    );
    Object.assign(environment, config.env, { TMPDIR: "/tmp", HOME: "/root" });
    const script = [
      `cd ${quote(config.cwd || process.cwd())}`,
      `exec env ${Object.entries(environment)
        .map(([k, v]) => quote(`${k}=${v}`))
        .join(
          " ",
        )} ${[config.command, ...(config.args || [])].map(quote).join(" ")}`,
    ].join(" && ");
    return {
      command: "ssh",
      args: [
        "-T",
        "-i",
        this.config.privateKey,
        "-p",
        String(this.port),
        "-o",
        "BatchMode=yes",
        "-o",
        "ConnectTimeout=2",
        "-o",
        "StrictHostKeyChecking=no",
        "-o",
        "UserKnownHostsFile=/dev/null",
        "-o",
        "LogLevel=ERROR",
        "root@127.0.0.1",
        script,
      ],
    };
  }
  async exec(config) {
    const c = this.command(config);
    const task = spawn(c.command, c.args, {
      stdio: ["ignore", "pipe", "pipe"],
      timeout: 15000,
    });
    let stdout = "",
      stderr = "";
    task.stdout.on("data", (b) => {
      stdout += b;
    });
    task.stderr.on("data", (b) => {
      stderr += b;
    });
    const [code] = await once(task, "exit");
    assert.equal(code, 0, stderr);
    return stdout;
  }
  async observe(target, relative = false) {
    const result = JSON.parse(
      await this.exec({
        command: process.execPath,
        args: [
          path.join(__dirname, "clock-vm-probe.cjs"),
          ...(target === undefined
            ? []
            : [String(target), ...(relative ? ["offset"] : [])]),
        ],
      }),
    );
    if (this.initial)
      assert.equal(result.boot, this.initial.boot, "guest kernel changed");
    this.record(result);
    return result;
  }
  async move(offset) {
    const after = await this.observe(offset, true);
    const before = after.before;
    assert.ok(after.mono >= before.mono, "monotonic clock moved backwards");
    assert.ok(
      Math.abs(after.wall - before.wall - (after.mono - before.mono) - offset) <
        1000,
      "guest CLOCK_REALTIME change did not take effect",
    );
    this.changed = after;
    return after;
  }
  async verifyClock() {
    const after = await this.observe();
    assert.ok(
      Math.abs(
        after.wall - this.changed.wall - (after.mono - this.changed.mono),
      ) < 1000,
      "guest CLOCK_REALTIME was restored by time synchronization",
    );
  }
  async close() {
    if (this.vm && this.vm.exitCode === null && this.vm.signalCode === null)
      this.vm.kill("SIGTERM");
    if (this.exited) await this.exited;
    fs.writeFileSync(
      path.join(this.directory, `${this.label}-vm.log`),
      this.output || "",
    );
  }
}
module.exports = { ClockVM };
