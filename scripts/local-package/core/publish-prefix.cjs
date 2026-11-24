#!/usr/bin/env node
// Publish a verified stage atomically. The caller cleans up duplicate stages.
const fs = require("node:fs/promises");
const path = require("node:path");
const { createHash } = require("node:crypto");
const { setImmediate } = require("node:timers/promises");

async function publish(stage, cache) {
  const manifest = path.join("share", "zlink", "core-package-provenance.json");
  const provenance = await fs.readFile(path.join(stage, manifest));
  const digest = createHash("sha256")
    .update(provenance)
    .digest("hex")
    .slice(0, 16);
  const prefix = path.join(cache, digest);
  await fs.mkdir(cache, { recursive: true });
  // Windows may report a sharing violation before the winning rename is visible.
  // Recheck on at most two subsequent event-loop turns; never replace an entry.
  for (let attempt = 0; ; attempt++) {
    try {
      await fs.rename(stage, prefix);
      return prefix;
    } catch (error) {
      if (
        !["EEXIST", "ENOTEMPTY", "EPERM", "EACCES", "EBUSY"].includes(
          error.code,
        )
      ) {
        throw error;
      }
      try {
        const existing = await fs.readFile(path.join(prefix, manifest));
        if (!existing.equals(provenance)) {
          throw new Error(`Core cache provenance collision: ${prefix}`);
        }
        return prefix;
      } catch (checkError) {
        if (!["ENOENT", "EPERM", "EACCES", "EBUSY"].includes(checkError.code)) {
          throw checkError;
        }
        if (attempt === 2) throw error;
      }
      await setImmediate();
    }
  }
}

module.exports = { publish };
if (require.main === module) {
  publish(...process.argv.slice(2)).then(
    (prefix) => console.log(prefix.split(path.sep).join("/")),
    (error) => {
      console.error(error);
      process.exitCode = 1;
    },
  );
}
