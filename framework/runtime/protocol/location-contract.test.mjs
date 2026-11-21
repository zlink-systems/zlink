import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
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
