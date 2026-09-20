const CONDITION_KINDS = new Set([
  "fieldPresent",
  "fieldEquals",
  "allFlagsSet",
  "anyFlagsSet",
  "contextEquals",
]);
const FLAG_CONDITION_KINDS = new Set(["allFlagsSet", "anyFlagsSet"]);
const VECTOR_CONSTRAINT_KINDS = new Set(["sorted", "unique"]);
const STRUCT_CONSTRAINT_KINDS = new Set(["not-both-zero", "field-less-than-or-equal"]);
const FIELD_CONSTRAINT_KINDS = new Set(["contains-protocol-required-capability"]);
const FLAG_CONSTRAINT_KINDS = new Set(["all-or-none", "implies"]);

function isObject(value) {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

function hasOwn(value, key) {
  return Object.prototype.hasOwnProperty.call(value, key);
}

function toBigInt(value) {
  if (typeof value === "number" && Number.isSafeInteger(value)) {
    return BigInt(value);
  }
  if (typeof value === "string" && /^(0|[1-9][0-9]*)$/.test(value)) {
    return BigInt(value);
  }
  return null;
}

function buildNamedMap(entries, label, location, fail) {
  const result = new Map();
  if (!Array.isArray(entries)) {
    fail(location, `${label} must be an array so duplicate names remain detectable`);
    return result;
  }
  entries.forEach((entry, index) => {
    if (!isObject(entry)) {
      fail(`${location}[${index}]`, "must be an object");
      return;
    }
    if (typeof entry.name !== "string" || entry.name.length === 0) {
      fail(`${location}[${index}].name`, "must be a non-empty string");
      return;
    }
    if (result.has(entry.name)) {
      fail(`${location}[${index}].name`, `duplicates ${entry.name}`);
      return;
    }
    Object.defineProperty(entry, "__index", {
      configurable: true,
      enumerable: false,
      value: index,
    });
    result.set(entry.name, entry);
  });
  return result;
}

function resolveInteger(value, bounds) {
  if (isObject(value) && typeof value.$bound === "string" && bounds.has(value.$bound)) {
    return toBigInt(bounds.get(value.$bound).value);
  }
  return toBigInt(value);
}

function resolveReference(reference, entries) {
  if (!isObject(reference) || typeof reference.$ref !== "string") {
    return null;
  }
  return entries.get(reference.$ref) ?? null;
}

function resolveReferencedMaximum(typeName, types, bounds) {
  const type = types.get(typeName);
  if (!type || type.kind !== "integer") {
    return null;
  }
  return resolveInteger(type.maximum, bounds);
}

function conditionSignature(value) {
  return JSON.stringify(
    Object.fromEntries(Object.entries(value).sort(([left], [right]) => left.localeCompare(right))),
  );
}

export {
  buildNamedMap,
  CONDITION_KINDS,
  conditionSignature,
  FIELD_CONSTRAINT_KINDS,
  FLAG_CONDITION_KINDS,
  FLAG_CONSTRAINT_KINDS,
  hasOwn,
  isObject,
  resolveInteger,
  resolveReference,
  resolveReferencedMaximum,
  STRUCT_CONSTRAINT_KINDS,
  toBigInt,
  VECTOR_CONSTRAINT_KINDS,
};
