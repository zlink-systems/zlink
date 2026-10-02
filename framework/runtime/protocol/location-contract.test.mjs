import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import { validateSchema, SchemaValidationError } from "./validate-service-wire-schema.mjs";

const schema = JSON.parse(readFileSync(new URL("./service-wire-v1.schema.json", import.meta.url)));

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
