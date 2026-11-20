#!/usr/bin/env node
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { jcsBytes, sha256, storageValidators } from "./aggregate-storage.mjs";

const directory = path.dirname(fileURLToPath(import.meta.url));

export function generateAggregateStorageFixtures(schema) {
  const storage = schema.jsonStorageFormats;
  const validators = storageValidators(storage);
  const pageSize = storage.pagePartition.maximumItems;
  const prefix = "golden:";
  const aggregateId = "01234567-89ab-cdef-0123-456789abcdef";
  // Deliberately exceed JSON's exact integer range.
  const aggregateGeneration = "9007199254740993";
  function record(kind, decoded) {
    assert(validators[kind](decoded), JSON.stringify(validators[kind].errors));
    const bytes = jcsBytes(decoded);
    if (kind === "page") assert(bytes.length <= storage.pageMaximumEncodedBytes);
    const key = storage.keys[kind].replace(/\{([^}]+)\}/g, (_, name) =>
      ({ prefix, aggregateId, aggregateGeneration, ...decoded })[name]);
    return { key, decoded, jcsUtf8: bytes.toString("utf8"), jcsHex: bytes.toString("hex"), sha256: sha256(bytes) };
  }
  function inventory(name, count, longEntries = false) {
    // §8.3 stores participant records verbatim, outside JSON canonicalization.
    const authorityPayloadHex = "00ff807061796c6f6164";
    const membershipMutationHex = "0100fe806d75746174696f6e";
    const authorityPayloadSha256 = sha256(Buffer.from(authorityPayloadHex, "hex"));
    const membershipMutationSha256 = sha256(Buffer.from(membershipMutationHex, "hex"));
    const entries = Array.from({ length: count }, (_, index) => ({
      index, authorityKey: longEntries
        ? `zla1:a:255:${"%EA%B0%80".repeat(84)}${[...Buffer.from(String.fromCharCode(0xAC00 + index))].map((b) => `%${b.toString(16).toUpperCase()}`).join("")}`
        : `zla1:a:11:golden-${String(index).padStart(4, "0")}`,
      expectedStoreVersion: longEntries ? "v".repeat(4096) : `opaque:${index}:한글`,
      ownerTransition: index % 2 === 0 ? "preserve" : "newOwner",
      authorityPayloadSha256, membershipMutationSha256,
    }));
    const pages = [];
    const reference = (page) => ({
      level: page.decoded.level, index: page.decoded.index, startIndex: page.decoded.startIndex,
      entryCount: page.decoded.entryCount, sha256: page.sha256,
    });
    // One partition rule for entries and references, using the whole stored page.
    function partition(items, level) {
      const result = [];
      const makePage = (group) => ({
        level, index: result.length,
        startIndex: level === 0 ? group[0].index : group[0].startIndex,
        entryCount: level === 0 ? group.length : group.reduce((sum, child) => sum + child.entryCount, 0),
        entries: level === 0 ? group : [], children: level === 0 ? [] : group,
      });
      const flush = (group) => {
        const page = record("page", makePage(group));
        pages.push(page);
        result.push(reference(page));
      };
      let group = [];
      for (const item of items) {
        const candidate = [...group, item];
        if (group.length && (candidate.length > pageSize
          || jcsBytes(makePage(candidate)).length > storage.pageMaximumEncodedBytes)) {
          flush(group);
          group = [];
        }
        group.push(item);
      }
      if (group.length) flush(group);
      return result;
    }
    let current = partition(entries, 0);
    let topLevel = 0;
    while (current.length > 1) current = partition(current, ++topLevel);
    const digestInput = { totalCount: count, topLevel, topPages: current };
    const digestInputBytes = jcsBytes(digestInput);
    const root = record("root", { ...digestInput, digestSha256: sha256(digestInputBytes) });
    const aggregate = record("aggregate", {
      aggregateId, aggregateGeneration, state: "prepared", participantCount: count,
      inventoryDigestSha256: root.decoded.digestSha256,
      targetDescriptor: { meshName: "golden-mesh", routingIdHex: "00ff1020" },
      targetDescriptorLifecycleGeneration: "18446744073709551615",
      targetOwner: { ownerId: "golden-owner", ownerLeaseGeneration: "9223372036854775807" },
      capacity: { actors: count, spots: 0, spotType: null },
    });
    return {
      name, aggregate, root, pages,
      rootDigestInputJcsHex: digestInputBytes.toString("hex"),
      participantHashInputs: { authorityPayloadHex, membershipMutationHex },
    };
  }
  return {
    spec: storage.spec, canonicalization: "RFC8785 (canonicalize@2.1.0)",
    byteEncoding: "jcsHex is exact UTF-8 storage bytes; no BOM or trailing newline",
    cases: [inventory("small-3", 3), inventory("two-level-1025", 1025), inventory("empty-0", 0), inventory("byte-limited-256", 256, true)],
  };
}

function main() {
  const [mode, schemaArgument, outputArgument] = process.argv.slice(2);
  assert(["--write", "--check"].includes(mode), "expected --write or --check");
  const schemaPath = path.resolve(schemaArgument ?? path.join(directory, "service-wire-v1.schema.json"));
  const outputPath = path.resolve(outputArgument ?? path.join(directory, "golden/aggregate-storage-v1.json"));
  const fixture = generateAggregateStorageFixtures(JSON.parse(fs.readFileSync(schemaPath, "utf8")));
  const expected = `${JSON.stringify(fixture, null, 2)}\n`;
  if (mode === "--write") fs.writeFileSync(outputPath, expected);
  else assert(fs.readFileSync(outputPath, "utf8") === expected, "aggregate storage golden fixture drift");
  const pages = fixture.cases.reduce((sum, inventory) => sum + inventory.pages.length, 0);
  console.log(`aggregate storage fixtures ${mode === "--write" ? "written" : "verified"}: ${fixture.cases.length} inventories, ${pages} pages`);
}
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) main();
