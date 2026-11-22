#!/usr/bin/env node
const assert = require("node:assert/strict");
const { test } = require("node:test");
const fs = require("node:fs/promises");
const path = require("node:path");
const crypto = require("node:crypto");
const vm = require("node:vm");
const { publish } = require("../core/publish-prefix.cjs");
const manifest = "share/zlink/core-package-provenance.json";
const provenance = Buffer.from('{"source":"verified"}\n');

function simulatedPublisher(operations) {
  const module = { exports: {} };
  vm.runInNewContext(
    require("node:fs").readFileSync(
      path.join(__dirname, "../core/publish-prefix.cjs"),
      "utf8",
    ),
    {
      module,
      require: (name) =>
        name === "node:fs/promises" ? operations : require(name),
    },
  );
  return module.exports.publish;
}

function failure(code) {
  return Object.assign(new Error(code), { code });
}

for (const code of ["EEXIST", "ENOTEMPTY", "EPERM", "EACCES", "EBUSY"]) {
  for (const same of [true, false]) {
    test(`${code}: ${same ? "reuse identical" : "reject different"} provenance`, async () => {
      let renames = 0;
      const publish = simulatedPublisher({
        mkdir: async () => {},
        rename: async () => {
          renames++;
          throw failure(code);
        },
        readFile: async (file) =>
          file.startsWith(`stage${path.sep}`) || same
            ? provenance
            : Buffer.from("different"),
      });
      if (same) assert.match(await publish("stage", "cache"), /[a-f0-9]{16}$/);
      else
        await assert.rejects(publish("stage", "cache"), /provenance collision/);
      assert.equal(renames, 1);
    });
  }
}

for (const code of ["EPERM", "EACCES", "EBUSY"]) {
  for (const outcome of ["visible", "rename", "busy", "different"]) {
    test(`${code}: bounded recheck (${outcome})`, async () => {
      let renames = 0;
      let checks = 0;
      const original = failure(code);
      const publish = simulatedPublisher({
        mkdir: async () => {},
        rename: async () => {
          renames++;
          if (outcome === "rename" && renames === 2) return;
          throw original;
        },
        readFile: async (file) => {
          if (file.startsWith(`stage${path.sep}`)) return provenance;
          checks++;
          if (checks < 3 || outcome === "busy")
            throw failure(checks === 1 ? "ENOENT" : code);
          return outcome === "different"
            ? Buffer.from("different")
            : provenance;
        },
      });
      if (outcome === "busy")
        await assert.rejects(publish("stage", "cache"), (e) => e === original);
      else if (outcome === "different")
        await assert.rejects(publish("stage", "cache"), /provenance collision/);
      else await publish("stage", "cache");
      assert.equal(renames, outcome === "rename" ? 2 : 3);
    });
  }
}

test("unrelated rename failures propagate without retry", async () => {
  const error = failure("EXDEV");
  let renames = 0;
  const publish = simulatedPublisher({
    mkdir: async () => {},
    readFile: async () => provenance,
    rename: async () => {
      renames++;
      throw error;
    },
  });
  await assert.rejects(publish("stage", "cache"), (e) => e === error);
  assert.equal(renames, 1);
});

test("real concurrent publish, short digest, collision and duplicate cleanup ownership", async () => {
  const artifacts = path.resolve(__dirname, "../../../.artifacts");
  await fs.mkdir(artifacts, { recursive: true });
  const base = await fs.mkdtemp(path.join(artifacts, "publish-prefix-"));
  try {
    const cache = path.join(base, "cache");
    const stages = [path.join(base, "a"), path.join(base, "b")];
    for (const stage of stages) {
      await fs.mkdir(path.dirname(path.join(stage, manifest)), {
        recursive: true,
      });
      await fs.writeFile(path.join(stage, manifest), provenance);
    }
    const [first, second] = await Promise.all(
      stages.map((stage) => publish(stage, cache)),
    );
    assert.equal(first, second);
    assert.equal(
      path.basename(first),
      crypto.createHash("sha256").update(provenance).digest("hex").slice(0, 16),
    );
    assert.deepEqual(await fs.readFile(path.join(first, manifest)), provenance);
    const duplicate =
      stages[
        await fs.stat(stages[0]).then(
          () => 0,
          () => 1,
        )
      ];
    assert.deepEqual(
      await fs.readFile(path.join(duplicate, manifest)),
      provenance,
    );
    // Force a truncated digest collision without relying on a probabilistic search.
    await fs.writeFile(path.join(first, manifest), "different");
    await assert.rejects(publish(duplicate, cache), /provenance collision/);
    assert.equal(
      await fs.readFile(path.join(first, manifest), "utf8"),
      "different",
    );
  } finally {
    await fs.rm(base, { recursive: true, force: true });
  }
});

test("manifest I/O failure propagates without another rename", async () => {
  let renames = 0;
  const error = failure("EIO");
  const publish = simulatedPublisher({
    mkdir: async () => {},
    rename: async () => {
      renames++;
      throw failure("EPERM");
    },
    readFile: async (file) => {
      if (file.startsWith(`stage${path.sep}`)) return provenance;
      throw error;
    },
  });
  await assert.rejects(publish("stage", "cache"), (e) => e === error);
  assert.equal(renames, 1);
});

test("both shell publishers use Node without Python fallbacks", async () => {
  for (const file of [
    "../core/fetch-release.sh",
    "../../gate/materialize-local-core-prefix.sh",
  ]) {
    const script = await fs.readFile(path.join(__dirname, file), "utf8");
    assert.doesNotMatch(script, /python3/);
    assert.match(script, /node .*publish-prefix\.cjs/);
  }
});
