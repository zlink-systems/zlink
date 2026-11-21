import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import { jcsBytes, sha256, storageValidators } from "./aggregate-storage.mjs";
import { generateAggregateStorageFixtures } from "./generate-aggregate-storage-fixtures.mjs";
import { validateSchema, SchemaValidationError } from "./validate-service-wire-schema.mjs";

const schema = JSON.parse(readFileSync(new URL("./service-wire-v1.schema.json", import.meta.url)));

test("Actor authority preserves existing values and represents Closing as two", () => {
  assert.deepEqual(schema.types.find((entry) => entry.name === "actor-authority-state").values, [
    { name: "creating", value: 0 },
    { name: "ready", value: 1 },
    { name: "closing", value: 2 },
  ]);
});

test("one close rule covers Actor and every Spot kind without relocation or recovery", () => {
  const integrity = schema.semanticConstraints.find(
    (entry) => entry.kind === "authority-operation-state-integrity",
  );
  assert.deepEqual(integrity.rules.filter((entry) => entry.operationKind === "close"), [{
    operationKind: "close",
    objectKinds: ["actor", "spot"],
    spotKinds: ["entry", "user", "instance"],
    relocationState: "absent",
    authorityStates: ["closing"],
    activationRecoveryState: "absent",
  }]);
});

test("validator rejects a Spot-only close rule", () => {
  const candidate = structuredClone(schema);
  const close = candidate.semanticConstraints.find(
    (entry) => entry.kind === "authority-operation-state-integrity",
  ).rules.find((entry) => entry.operationKind === "close");
  delete close.objectKinds;
  close.objectKind = "spot";
  assert.throws(() => validateSchema(candidate), (error) =>
    error instanceof SchemaValidationError
    && error.errors.some((message) => message.includes("authority-operation-state-integrity")));
});

test("validator rejects a missing or renumbered Actor Closing state", () => {
  for (const value of [undefined, 3]) {
    const candidate = structuredClone(schema);
    const state = candidate.types.find((entry) => entry.name === "actor-authority-state");
    state.values = state.values.filter((entry) => entry.name !== "closing");
    if (value !== undefined) state.values.push({ name: "closing", value });
    assert.throws(() => validateSchema(candidate), (error) =>
      error instanceof SchemaValidationError
      && error.errors.some((message) => message.includes("Actor authority state")));
  }
});

test("location defaults retain the start-based lease timing contract", () => {
  validateSchema(schema);
  const lease = schema.semanticConstraints.find((entry) => entry.kind === "owner-lease-timing-integrity");
  assert.equal(lease.startupRelation,
    "max-renewInterval-renewTimeout-plus-renewTimeout-strictly-less-than-ttl-minus-fencingMargin");
});

test("lease timeout longer than interval fails the timing relation itself", () => {
  const candidate = structuredClone(schema);
  const lease = candidate.semanticConstraints.find((entry) => entry.kind === "owner-lease-timing-integrity");
  lease.renewIntervalMs = 1000;
  lease.renewTimeoutMs = 8000;
  assert.throws(() => validateSchema(candidate), (error) =>
    error instanceof SchemaValidationError
    && error.errors.some((message) => message.includes("max(renew interval, renew timeout)")));
});

test("weight bound follows the glossary", () => {
  assert.equal(schema.bounds.find((entry) => entry.name === "weightMax").value, 10000);
});

test("obsolete weight bound is rejected by its owning validator", () => {
  const candidate = structuredClone(schema);
  candidate.bounds.find((entry) => entry.name === "weightMax").value = 100;
  assert.throws(() => validateSchema(candidate), (error) =>
    error instanceof SchemaValidationError
    && error.errors.some((message) => message.includes("bound weightMax must equal 10000")));
});

test("generated weight rejection uses the schema boundary", () => {
  const fixtures = JSON.parse(readFileSync(new URL("./generated/fixtures/index.json", import.meta.url)));
  const range = fixtures.operationCases.find((entry) => entry.name === "tlv-encode-field-range");
  assert.equal(range.input.placementWeight,
    schema.bounds.find((entry) => entry.name === "weightMax").value + 1);
});

test("aggregate storage has a separate closed JSON contract", () => {
  const storage = schema.jsonStorageFormats;
  assert.equal(storage?.canonicalization, "RFC8785");
  assert.equal(Object.keys(storage.keys).length, 5);
  assert.deepEqual(Object.keys(storage.records.$defs.aggregate.properties), [
    "aggregateId", "aggregateGeneration", "state", "participantCount", "inventoryDigestSha256",
    "targetDescriptor", "targetDescriptorLifecycleGeneration", "targetOwner", "capacity",
  ]);
});

test("participant records retain raw bytes and inventory partition is deterministic", () => {
  assert.deepEqual(schema.jsonStorageFormats.rawBytesRecords, {
    authorityPayload: "application-defined-opaque-bytes",
    membershipMutation: "aggregate-participant-mutation-bytes",
  });
  assert.deepEqual(schema.jsonStorageFormats.pagePartition, {
    maximumItems: 1024,
    leaf: "greedy-entry-index-order-until-next-item-exceeds-byte-or-item-limit",
    parent: "greedy-child-page-index-order-until-next-item-exceeds-byte-or-item-limit",
    promotion: "only-when-current-level-has-multiple-pages",
    root: "single-highest-page-reference",
    empty: { totalCount: 0, topLevel: 0, topPages: [] },
  });
  const empty = storageGolden.cases.find((fixture) => fixture.name === "empty-0");
  assert(empty, "zero-participant inventory fixture is required");
  assert.deepEqual(empty.pages, []);
  const { digestSha256, ...root } = empty.root.decoded;
  assert.deepEqual(root, { totalCount: 0, topLevel: 0, topPages: [] });
  for (const fixture of storageGolden.cases) {
    for (const hex of Object.values(fixture.participantHashInputs)) {
      assert.notEqual(hex, "7b7d", "participant input must exercise non-JSON bytes");
    }
  }
});

test("application snapshots do not require durable relocation roots", () => {
  const integrity = schema.semanticConstraints.find(
    (entry) => entry.kind === "location-relocation-storage-integrity",
  );
  assert.equal(integrity.publicationOrder, undefined);
  assert.equal(schema.relocationRetentionPolicy, undefined);
  assert.equal(schema.relocationStorageProfile.writeOrder, undefined);
});


const validators = storageValidators(schema.jsonStorageFormats);
const storageGolden = JSON.parse(readFileSync(new URL("./golden/aggregate-storage-v1.json", import.meta.url)));

test("JCS uses RFC 8785 number serialization and UTF-16 property ordering", () => {
  assert.equal(jcsBytes({ numbers: [333333333.33333329, 1e30, 4.50, 2e-3, 1e-27] }).toString(),
    '{"numbers":[333333333.3333333,1e+30,4.5,0.002,1e-27]}');
  assert.equal(jcsBytes({ "דּ": 1, "😀": 2, "€": 3, "ö": 4, "1": 5, "\r": 6 }).toString(),
    '{"\\r":6,"1":5,"ö":4,"€":3,"😀":2,"דּ":1}');
});

for (const fixture of storageGolden.cases) {
  test(`aggregate JCS bytes and the entire hash tree agree: ${fixture.name}`, () => {
    const records = [["aggregate", fixture.aggregate], ["root", fixture.root],
      ...fixture.pages.map((page) => ["page", page])];
    for (const [kind, record] of records) {
      assert(validators[kind](record.decoded), JSON.stringify(validators[kind].errors));
      assert.equal(jcsBytes(record.decoded).toString("hex"), record.jcsHex);
      assert.equal(Buffer.from(record.jcsHex, "hex").toString("utf8"), record.jcsUtf8);
      assert.equal(sha256(Buffer.from(record.jcsHex, "hex")), record.sha256);
    }
    const { digestSha256, ...input } = fixture.root.decoded;
    assert.equal(jcsBytes(input).toString("hex"), fixture.rootDigestInputJcsHex);
    assert.equal(sha256(Buffer.from(fixture.rootDigestInputJcsHex, "hex")), digestSha256);
    assert.equal(fixture.aggregate.decoded.inventoryDigestSha256, digestSha256);
    const pages = new Map(fixture.pages.map((page) => [`${page.decoded.level}:${page.decoded.index}`, page]));
    const references = [...fixture.root.decoded.topPages, ...fixture.pages.flatMap((p) => p.decoded.children)];
    for (const reference of references) {
      const page = pages.get(`${reference.level}:${reference.index}`);
      assert.equal(reference.sha256, page.sha256);
      for (const field of ["level", "index", "startIndex", "entryCount"]) {
        assert.equal(reference[field], page.decoded[field]);
      }
    }
    assert.equal(fixture.root.decoded.topPages.reduce((sum, p) => sum + p.entryCount, 0), fixture.root.decoded.totalCount);
    for (const page of fixture.pages) {
      const value = page.decoded;
      assert(Buffer.from(page.jcsHex, "hex").length <= 1048576);
      assert.equal(value.entryCount, value.level === 0 ? value.entries.length
        : value.children.reduce((sum, child) => sum + child.entryCount, 0));
      for (const child of value.children) assert.equal(child.level, value.level - 1);
      const expectedKey = schema.jsonStorageFormats.keys.page.replace(/\{([^}]+)\}/g, (_, name) =>
        ({ prefix: "golden:", ...fixture.aggregate.decoded, ...value })[name]);
      assert.equal(page.key, expectedKey);
    }
    const entries = fixture.pages.flatMap((page) => page.decoded.entries);
    assert.equal(entries.length, fixture.root.decoded.totalCount);
    assert(entries.every((entry) => entry.authorityKey.startsWith("zla1:a:")));
    assert.deepEqual(entries.map((entry) => entry.index), Array.from({ length: entries.length }, (_, i) => i));
    assert.deepEqual(entries.map((entry) => entry.authorityKey), entries.map((entry) => entry.authorityKey)
      .sort((a, b) => Buffer.compare(Buffer.from(a), Buffer.from(b))));
    let level = 0;
    let pageCount = 0;
    while (entries.length > 0) {
      const levelPages = fixture.pages.filter((page) => page.decoded.level === level);
      let itemIndex = 0;
      for (const [index, page] of levelPages.entries()) {
        const value = page.decoded;
        assert.equal(value.index, index);
        const items = level === 0 ? value.entries : value.children;
        assert(items.length > 0 && items.length <= 1024);
        assert.deepEqual(items.map((item) => item.index),
          Array.from({ length: items.length }, (_, i) => itemIndex + i));
        itemIndex += items.length;
        assert.equal(value.startIndex, level === 0 ? items[0].index : items[0].startIndex);
        const next = levelPages[index + 1]?.decoded;
        if (next && items.length < 1024) {
          const candidate = structuredClone(value);
          if (level === 0) {
            candidate.entries.push(next.entries[0]);
            candidate.entryCount += 1;
          } else {
            candidate.children.push(next.children[0]);
            candidate.entryCount += next.children[0].entryCount;
          }
          assert(jcsBytes(candidate).length > 1048576, "page must include every item that fits");
        }
      }
      pageCount += levelPages.length;
      if (levelPages.length === 1) break;
      level += 1;
    }
    assert.equal(fixture.pages.length, pageCount);
    assert.equal(fixture.root.decoded.topLevel, level);
    assert.equal(fixture.root.decoded.topPages.length, entries.length === 0 ? 0 : 1);
    for (const entry of entries) {
      assert.equal(entry.authorityPayloadSha256, sha256(Buffer.from(fixture.participantHashInputs.authorityPayloadHex, "hex")));
      assert.equal(entry.membershipMutationSha256, sha256(Buffer.from(fixture.participantHashInputs.membershipMutationHex, "hex")));
    }
  });
}

test("storage validator rejects missing, additional, mistyped and obsolete fields", () => {
  const mutations = [
    (s) => delete s.jsonStorageFormats,
    (s) => { s.jsonStorageFormats.canonicalization = "JSON.stringify"; },
    (s) => { s.jsonStorageFormats.keys.page += ":v1"; },
    (s) => { s.jsonStorageFormats.rawBytesRecords = { authorityPayload: "JSON", membershipMutation: "JSON" }; },
    (s) => { s.jsonStorageFormats.pagePartition = {}; },
    (s) => { s.jsonStorageFormats.records.$defs.aggregate.properties.version = { type: "integer" }; },
    (s) => { s.jsonStorageFormats.records.$defs.aggregate.required.pop(); },
    (s) => { s.jsonStorageFormats.records.$defs.aggregate.properties.aggregateGeneration = { type: "number" }; },
    (s) => { s.jsonStorageFormats.records.$defs.page.allOf = []; },
    (s) => { s.jsonStorageFormats.hashInputs["root.digestSha256"] = "entries-only"; },
    (s) => { s.relocationRetentionPolicy = {}; },
    (s) => { s.relocationStorageProfile.writeOrder = ["location-authority-reference-cas"]; },
  ];
  for (const mutate of mutations) {
    const candidate = structuredClone(schema);
    mutate(candidate);
    assert.throws(() => validateSchema(candidate), SchemaValidationError);
  }
});

test("JSON record schemas reject corrupt storage records", () => {
  const valid = storageGolden.cases[0];
  for (const [kind, record] of [["aggregate", valid.aggregate], ["root", valid.root], ["page", valid.pages[0]]]) {
    for (const mutate of [
      (value) => { value.version = 1; },
      (value) => { delete value[Object.keys(value)[0]]; },
    ]) {
      const bad = structuredClone(record.decoded);
      mutate(bad);
      assert.equal(validators[kind](bad), false);
    }
  }
  for (const [kind, value] of [
    ["aggregate", { ...valid.aggregate.decoded, aggregateGeneration: 9007199254740992 }],
    ["aggregate", { ...valid.aggregate.decoded, aggregateGeneration: "01" }],
    ["aggregate", { ...valid.aggregate.decoded, state: "staging" }],
    ["root", { ...valid.root.decoded, digestSha256: "AB".repeat(32) }],
    ["page", { ...valid.pages[0].decoded, children: valid.root.decoded.topPages }],
    ["page", { ...valid.pages[0].decoded, level: 1 }],
    ["page", { ...valid.pages[0].decoded, entries: Array(1025).fill(valid.pages[0].decoded.entries[0]) }],
    ["page", { ...valid.pages[0].decoded, index: 2147483648 }],
  ]) assert.equal(validators[kind](value), false);
});


test("generation decimal strings stay within unsigned 64-bit range", () => {
  const value = structuredClone(storageGolden.cases[0].aggregate.decoded);
  value.aggregateGeneration = "18446744073709551615";
  assert.equal(validators.aggregate(value), true);
  value.aggregateGeneration = "18446744073709551616";
  assert.equal(validators.aggregate(value), false);
});

test("source inventory digest semantics cannot be omitted or changed", () => {
  assert.deepEqual(schema.jsonStorageFormats.sourceInventorySha256, {
    authorityField: "aggregate.relocation.sourceInventorySha256",
    value: "sealed-inventory-root.digestSha256",
    immutableAfter: "preparing",
    singleActorWithoutAggregate: "single-actor-entry-inventory-root.digestSha256",
  });
  for (const field of Object.keys(schema.jsonStorageFormats.sourceInventorySha256)) {
    for (const replacement of [undefined, "wrong"]) {
      const candidate = structuredClone(schema);
      if (replacement === undefined) delete candidate.jsonStorageFormats.sourceInventorySha256[field];
      else candidate.jsonStorageFormats.sourceInventorySha256[field] = replacement;
      assert.throws(() => validateSchema(candidate), SchemaValidationError);
    }
  }
});

test("terminal completion persistence survives application snapshot rule removal", () => {
  const ack = schema.semanticConstraints.find((e) => e.kind === "reply-relay-context-integrity").acknowledgement;
  assert.equal(ack.targetPersistence, "relocation-root-cas-before-ack-effect");
  assert.equal(ack.retireWhileSourceLeaseValid,
    "forceStopped-and-retain-relocation-root-and-reply-bytes-for-retention-window");
  for (const field of ["targetPersistence", "retireWhileSourceLeaseValid"]) {
    const candidate = structuredClone(schema);
    delete candidate.semanticConstraints.find((e) => e.kind === "reply-relay-context-integrity").acknowledgement[field];
    assert.throws(() => validateSchema(candidate), SchemaValidationError);
  }
});

test("byte-limited inventory splits before 1024 entries", () => {
  const fixture = generateAggregateStorageFixtures(schema).cases.find((c) => c.name === "byte-limited-256");
  assert(fixture, "long authority key and StoreVersion fixture is required");
  const leaves = fixture.pages.filter((p) => p.decoded.level === 0);
  assert(leaves.length > 1);
  assert(leaves[0].decoded.entries.length < 256);
  assert.equal(Buffer.byteLength(leaves[0].decoded.entries[0].authorityKey), 776);
  assert.equal(Buffer.byteLength(leaves[0].decoded.entries[0].expectedStoreVersion), 4096);
});

test("storage spec link uses the actual section anchor", () => {
  const anchor = "01-location-runtime.ko.md#83-aggregate-기록과-이동-대상-목록의-저장-형식";
  assert.equal(schema.jsonStorageFormats.spec, anchor);
  assert.equal(generateAggregateStorageFixtures(schema).spec, anchor);
});

test("completion roots retain verified replacement and authority-before-delete order", () => {
  const rules = {
    replacement: "write-and-verify-new-root-before-one-location-authority-cas-replaces-reference",
    orphanCleanup: "unpublished-or-replaced-root-is-not-authority-and-is-deleted-or-expires",
    deleteOrder: "release-or-replace-location-authority-reference-before-idempotent-relocation-root-delete",
  };
  const integrity = schema.semanticConstraints.find((e) => e.kind === "location-relocation-storage-integrity");
  for (const [field, value] of Object.entries(rules)) {
    assert.equal(integrity[field], value);
    const candidate = structuredClone(schema);
    delete candidate.semanticConstraints.find((e) => e.kind === "location-relocation-storage-integrity")[field];
    assert.throws(() => validateSchema(candidate), SchemaValidationError);
  }
});
