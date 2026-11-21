import Ajv2020 from "ajv/dist/2020.js";
import canonicalize from "canonicalize";
import { createHash } from "node:crypto";

// Use the RFC 8785 implementation for stored bytes, never a JSON.stringify sort.
export const jcsBytes = (value) => Buffer.from(canonicalize(value), "utf8");
export const sha256 = (bytes) => createHash("sha256").update(bytes).digest("hex");

export function storageValidators(storage) {
  const ajv = new Ajv2020({ allErrors: true, strict: true });
  return Object.fromEntries(["aggregate", "root", "page", "reference", "entry"].map((name) => [
    name, ajv.compile({ ...storage.records, $ref: `#/$defs/${name}` }),
  ]));
}

export function validateJsonStorageFormats(storage, fail) {
  const location = "$.jsonStorageFormats";
  if (!storage || typeof storage !== "object") {
    fail(location, "must define aggregate JSON storage (location §8.3)");
    return;
  }
  const requireEqual = (name, actual, expected) => {
    if (canonicalize(actual ?? null) !== canonicalize(expected)) {
      fail(`${location}.${name}`, "must match location §8.3");
    }
  };
  requireEqual("spec", storage.spec, "01-location-runtime.ko.md#83-aggregate-기록과-이동-대상-목록의-저장-형식");
  requireEqual("sourceInventorySha256", storage.sourceInventorySha256, {
    authorityField: "aggregate.relocation.sourceInventorySha256",
    value: "sealed-inventory-root.digestSha256",
    immutableAfter: "preparing",
    singleActorWithoutAggregate: "single-actor-entry-inventory-root.digestSha256",
  });
  requireEqual("canonicalization", storage.canonicalization, "RFC8785");
  requireEqual("encoding", storage.encoding, "utf-8-without-bom");
  const base = "{prefix}aggregate-inventory:{aggregateId}:{aggregateGeneration}";
  requireEqual("keys", storage.keys, {
    aggregate: "{prefix}aggregate:{aggregateId}:{aggregateGeneration}",
    root: `${base}:root`, page: `${base}:page:{level}:{index}`,
    authorityPayload: `${base}:participant:{index}:authorityPayload`,
    membershipMutation: `${base}:participant:{index}:membershipMutation`,
  });
  requireEqual("keyComponents", storage.keyComponents, {
    prefix: "authority-provider-storage-namespace-prefix", aggregateId: "lowercase-canonical-uuid",
    aggregateGeneration: "unpadded-decimal", level: "unpadded-decimal", index: "unpadded-decimal",
  });
  const fields = {
    descriptor: "meshName routingIdHex", owner: "ownerId ownerLeaseGeneration",
    spotType: "objectKind stableType count", capacity: "actors spots spotType",
    aggregate: "aggregateId aggregateGeneration state participantCount inventoryDigestSha256 targetDescriptor targetDescriptorLifecycleGeneration targetOwner capacity",
    root: "totalCount topLevel topPages digestSha256",
    page: "level index startIndex entryCount entries children",
    reference: "level index startIndex entryCount sha256",
    entry: "index authorityKey expectedStoreVersion ownerTransition authorityPayloadSha256 membershipMutationSha256",
  };
  const defs = storage.records?.$defs;
  if (!defs) {
    fail(`${location}.records`, "must define JSON Schema record types");
    return;
  }
  for (const [name, names] of Object.entries(fields)) {
    const definition = defs[name];
    requireEqual(`records.${name}.fields`, Object.keys(definition?.properties ?? {}).sort(), names.split(" ").sort());
    requireEqual(`records.${name}.required`, [...(definition?.required ?? [])].sort(), names.split(" ").sort());
    requireEqual(`records.${name}.additionalProperties`, definition?.additionalProperties, false);
  }
  const propertyTypes = {
    descriptor: { meshName: { type: "string" }, routingIdHex: { type: "string", pattern: "^(?:[0-9a-f]{2})*$" } },
    owner: { ownerId: { type: "string" }, ownerLeaseGeneration: "generation" },
    spotType: { objectKind: { enum: ["userSpot", "instanceSpot"] }, stableType: { type: "string" }, count: "count" },
    capacity: { actors: "count", spots: "count", spotType: { anyOf: [{ type: "null" }, { $ref: "#/$defs/spotType" }] } },
    aggregate: {
      aggregateId: "uuid", aggregateGeneration: "generation", participantCount: "count",
      inventoryDigestSha256: "sha256", targetDescriptor: "descriptor",
      targetDescriptorLifecycleGeneration: "generation", targetOwner: "owner", capacity: "capacity",
    },
    root: { totalCount: "count", topLevel: "count", topPages: { type: "array", items: { $ref: "#/$defs/reference" } }, digestSha256: "sha256" },
    page: { level: "count", index: "count", startIndex: "count", entryCount: "count",
      entries: { type: "array", items: { $ref: "#/$defs/entry" }, maxItems: 1024 },
      children: { type: "array", items: { $ref: "#/$defs/reference" }, maxItems: 1024 } },
    reference: { level: "count", index: "count", startIndex: "count", entryCount: "count", sha256: "sha256" },
    entry: { index: "count", authorityKey: { type: "string" }, expectedStoreVersion: { type: "string" },
      authorityPayloadSha256: "sha256", membershipMutationSha256: "sha256" },
  };
  for (const [record, properties] of Object.entries(propertyTypes)) {
    for (const [field, type] of Object.entries(properties)) {
      const expected = typeof type === "string" ? { $ref: `#/$defs/${type}` } : type;
      requireEqual(`records.${record}.${field}`, defs[record]?.properties?.[field], expected);
    }
  }
  requireEqual("records.uuid", defs.uuid, { type: "string", pattern: "^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$" });
  requireEqual("records.page.allOf", defs.page?.allOf, [{
    if: { properties: { level: { const: 0 } } },
    then: { properties: { children: { maxItems: 0, type: "array" } } },
    else: { properties: { entries: { maxItems: 0, type: "array" } } },
  }]);
  requireEqual("participantOrder", storage.participantOrder, "canonical-authority-key-utf8-bytes-ascending");
  requireEqual("savedWorkParticipantId", storage.savedWorkParticipantId, "index-plus-one");
  requireEqual("keyAgreement", storage.keyAgreement, { aggregate: ["aggregateId", "aggregateGeneration"], page: ["level", "index"] });
  requireEqual("countAgreement", storage.countAgreement, "aggregate.participantCount-equals-root.totalCount");
  requireEqual("referenceAgreement", storage.referenceAgreement, ["level", "index", "startIndex", "entryCount"]);
  requireEqual("replacement", storage.replacement, "new-aggregateGeneration-for-different-content");
  requireEqual("records.generation", defs.generation, { type: "string", pattern: "^(?:0|[1-9][0-9]{0,18}|1[0-7][0-9]{18}|18[0-3][0-9]{17}|184[0-3][0-9]{16}|1844[0-5][0-9]{15}|18446[0-6][0-9]{14}|184467[0-3][0-9]{13}|1844674[0-3][0-9]{12}|184467440[0-6][0-9]{10}|1844674407[0-2][0-9]{9}|18446744073[0-6][0-9]{8}|1844674407370[0-8][0-9]{6}|18446744073709[0-4][0-9]{5}|184467440737095[0-4][0-9]{4}|18446744073709550[0-9]{3}|18446744073709551[0-5][0-9]{2}|1844674407370955160[0-9]{1}|1844674407370955161[0-4]|18446744073709551615)$" });
  requireEqual("records.count", defs.count, { type: "integer", minimum: 0, maximum: 2147483647 });
  requireEqual("records.sha256", defs.sha256, { type: "string", pattern: "^[0-9a-f]{64}$" });
  requireEqual("records.aggregate.state", defs.aggregate?.properties?.state, { enum: ["preparing", "prepared", "committed", "aborted"] });
  requireEqual("records.entry.ownerTransition", defs.entry?.properties?.ownerTransition, { enum: ["preserve", "newOwner"] });
  for (const name of ["entries", "children"]) {
    requireEqual(`records.page.${name}.maxItems`, defs.page?.properties?.[name]?.maxItems, 1024);
  }
  requireEqual("rawBytesRecords", storage.rawBytesRecords, {
    authorityPayload: "application-defined-opaque-bytes",
    membershipMutation: "aggregate-participant-mutation-bytes",
  });
  requireEqual("pagePartition", storage.pagePartition, {
    maximumItems: 1024,
    leaf: "greedy-entry-index-order-until-next-item-exceeds-byte-or-item-limit",
    parent: "greedy-child-page-index-order-until-next-item-exceeds-byte-or-item-limit",
    promotion: "only-when-current-level-has-multiple-pages",
    root: "single-highest-page-reference",
    empty: { totalCount: 0, topLevel: 0, topPages: [] },
  });
  requireEqual("pageMaximumEncodedBytes", storage.pageMaximumEncodedBytes, 1048576);
  requireEqual("hashInputs", storage.hashInputs, {
    "reference.sha256": "referenced-page-entire-stored-jcs-bytes",
    "root.digestSha256": "root-jcs-bytes-with-only-digestSha256-omitted",
    "entry.authorityPayloadSha256": "participant-authorityPayload-stored-bytes",
    "entry.membershipMutationSha256": "participant-membershipMutation-stored-bytes",
  });
  requireEqual("digestAgreement", storage.digestAgreement, "aggregate.inventoryDigestSha256-equals-root.digestSha256");
  requireEqual("requestEquality", storage.requestEquality, "all-aggregate-fields-except-state");
  requireEqual("stateTransitions", storage.stateTransitions, {
    preparing: ["prepared", "aborted"], prepared: ["committed", "aborted"], committed: [], aborted: [],
  });
  requireEqual("immutableRecords", storage.immutableRecords, ["root", "page", "authorityPayload", "membershipMutation"]);
  try {
    storageValidators(storage);
  } catch (error) {
    fail(`${location}.records`, error.message);
  }
}
