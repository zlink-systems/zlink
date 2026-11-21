// Executed only inside the disposable QEMU guest.
const fs = require("node:fs");
const { execFileSync } = require("node:child_process");
const before = {
  wall: Date.now(),
  mono: Number(process.hrtime.bigint() / 1000000n),
};
if (process.argv[2] !== undefined) {
  const value = Number(process.argv[2]);
  const target = process.argv[3] === "offset" ? before.wall + value : value;
  if (!Number.isSafeInteger(target)) throw new Error("invalid clock target");
  execFileSync("/bin/date", ["-s", `@${target / 1000}`]);
}
console.log(
  JSON.stringify({
    before,
    boot: fs.readFileSync("/proc/sys/kernel/random/boot_id", "utf8").trim(),
    wall: Date.now(),
    mono: Number(process.hrtime.bigint() / 1000000n),
  }),
);
