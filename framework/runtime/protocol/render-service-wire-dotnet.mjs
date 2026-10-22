#!/usr/bin/env node
import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { lowerSchema } from "./service-wire-lowering.mjs";

const [mode, schemaArg, outputArg, ...extra] = process.argv.slice(2);
if (
  !["--write", "--check"].includes(mode) ||
  !schemaArg ||
  !outputArg ||
  extra.length
) {
  console.error(
    "usage: node render-service-wire-dotnet.mjs --write|--check <schema> <output>",
  );
  process.exit(2);
}
const ir = lowerSchema(path.resolve(schemaArg));
const output = path.resolve(outputArg);
const types = new Map(ir.types.map((x) => [x.name, x]));
const flagBits = new Map(ir.flags.map((x) => [x.name, x.bit]));
const negotiatedContexts = [
  ...new Set(
    ir.types.flatMap((type) =>
      type.operations
        .filter((operation) => operation.op === "negotiated-bound")
        .map((operation) => operation.context.name),
    ),
  ),
];
const seen = new Set();
const id = (s) => {
  const x = s.replace(/(^|[^A-Za-z0-9]+)([A-Za-z0-9])/g, (_m, _s, c) =>
    c.toUpperCase(),
  );
  return /^\d/.test(x) ? `N${x}` : x;
};
const rn = (r) => id(r.$ref);
const op = (owner, name) => {
  const x = owner.operations.find((o) => o.op === name);
  if (!x) throw Error(`${owner.name}: missing ${name}`);
  seen.add(name);
  return x;
};
const main = (owner) => {
  const x = owner.operations.find((o) => o.op !== "runtime-predicate");
  if (!x) throw Error(`${owner.name}: no operation`);
  seen.add(x.op);
  return x;
};
const intType = (e) =>
  ({ u8: "byte", u16: "ushort", u32: "uint", u64: "ulong", i64: "long" })[e] ??
  (() => {
    throw Error(`encoding ${e}`);
  })();
const lit = (v, e = "u64") =>
  `${v}${e === "u64" ? "UL" : e === "u32" ? "U" : e === "i64" ? "L" : ""}`;
const typeOf = (r, n = false) => `${rn(r)}${n ? "?" : ""}`;
const optional = (f) => Boolean(f.when) || f.required === false;
const fieldsDecl = (fs) =>
  fs.map((f) => `${typeOf(f.type, optional(f))} ${id(f.name)}`).join(", ");
const cases = (o) => Object.entries(o.cases);
const caseFields = (selected) =>
  selected.operations.filter((operation) => operation.op === "field");
const unionFields = (o, selected) => [
  ...o.discriminators
    .filter((d) => d.source.kind === "wire")
    .map((d) => ({ name: d.name, type: d.type })),
  ...caseFields(selected),
];

function declaration(t) {
  const o = main(t),
    n = id(t.name);
  switch (o.op) {
    case "integer":
      return `internal sealed record ${n}(${intType(o.encoding)} Value);`;
    case "enum":
      return `internal enum ${n} : ${intType(o.encoding)}\n{\n${o.values.map((v) => `    ${id(v.name)} = ${v.value}`).join(",\n")}\n}`;
    case "length-prefixed":
      return `internal sealed record ${n}(${o.content === "text" ? "string" : "byte[]"}${o.zeroLengthMeaning ? "?" : ""} Value);`;
    case "struct":
    case "versioned-length-delimited":
      return `internal sealed record ${n}(${fieldsDecl(o.fields)});`;
    case "vector":
      return `internal sealed record ${n}(IReadOnlyList<${typeOf(o.item)}> Items);`;
    case "versioned-vector": {
      const r = o.layout.find((x) => x.kind === "repeat");
      return `internal sealed record ${n}(IReadOnlyList<${typeOf(r.item)}> ${id(r.name)});`;
    }
    case "conditional-union": {
      const d = cases(o).map(
        ([, v], i) =>
          `internal sealed record ${n}Case${i}(${fieldsDecl(unionFields(o, v))}) : ${n};`,
      );
      if (o.otherwise.kind === "fields")
        d.push(
          `internal sealed record ${n}Otherwise(${fieldsDecl(o.otherwise.fields)}) : ${n};`,
        );
      return `internal abstract record ${n};\n${d.join("\n")}`;
    }
    case "tlv32":
      return `internal sealed record ${n}(${o.fields.map((f) => `${typeOf(f.type, true)} ${id(f.name)}`).join(", ")});`;
    default:
      throw Error(`${t.name}: declaration ${o.op}`);
  }
}

function expected(r, v) {
  const t = types.get(r.$ref),
    o = main(t);
  if (o.op === "enum") return `${id(t.name)}.${id(v)}`;
  if (o.op === "integer") return `new ${id(t.name)}(${lit(v, o.encoding)})`;
  throw Error(`${t.name}: constant`);
}
const scalar = (r, x) =>
  main(types.get(r.$ref)).op === "integer" ? `${x}.Value` : x;
function external(r) {
  const o = main(types.get(r.$ref));
  return o.op === "conditional-union"
    ? o.discriminators.filter((d) => d.source.kind === "enclosingField")
    : [];
}
function readCall(r, reader, env) {
  return `Read${rn(r)}(${[
    reader,
    "context",
    ...external(r).map(
      (d) =>
        env.get(d.source.name) ??
        (() => {
          throw Error(`${r.$ref}: missing ${d.source.name}`);
        })(),
    ),
  ].join(", ")})`;
}
function writeCall(r, writer, value, env) {
  return `Write${rn(r)}(${[
    writer,
    value,
    "context",
    ...external(r).map(
      (d) =>
        env.get(d.source.name) ??
        (() => {
          throw Error(`${r.$ref}: missing ${d.source.name}`);
        })(),
    ),
  ].join(", ")})`;
}
function cond(c, env, flags = "flags") {
  return (
    c.all
      .map((a) => {
        if (a.kind === "fieldPresent") {
          const field = env.get(`${a.operand.name}:field`),
            target = main(types.get(field.type.$ref)),
            value = env.get(a.operand.name),
            p = a.presence;
          if (
            p.internal?.absent !== null ||
            p.internal?.present !== "non-null" ||
            p.wire?.kind !== "length-prefix-sentinel" ||
            p.wire.absent !== 0 ||
            p.wire.present?.minimum !== 1 ||
            target.op !== "length-prefixed" ||
            target.zeroLengthMeaning !== "absent" ||
            p.wire.lengthType.$ref !== target.lengthType.$ref
          )
            throw Error(`fieldPresent ${a.operand.name}`);
          return `${value}.Value is not null`;
        }
        if (a.kind === "fieldEquals")
          return `${env.get(a.operand.name)} == ${expected(env.get(`${a.operand.name}:field`).type, a.value)}`;
        if (a.kind === "contextEquals") {
          const d = ir.semanticContexts.find((x) => x.name === a.operand.name);
          return `context.${id(a.operand.name)} == ${expected(d.valueType, a.value)}`;
        }
        if (a.kind === "allFlagsSet" || a.kind === "anyFlagsSet")
          return `(${a.operands.map((x) => `(${flags} & ${flagBits.get(x.name)}) != 0`).join(a.kind === "allFlagsSet" ? " && " : " || ")})`;
        throw Error(`condition ${a.kind}`);
      })
      .join(" && ") || "true"
  );
}
function fieldChecks(f, x) {
  const a = [];
  if (f.constant !== undefined)
    a.push(
      `if (${x} != ${expected(f.type, f.constant)}) throw Error("${f.name}: constant");`,
    );
  const e = main(types.get(f.type.$ref)).encoding;
  if (f.minimum !== undefined)
    a.push(
      `if (${scalar(f.type, x)} < ${lit(f.minimum, e)}) throw Error("${f.name}: minimum");`,
    );
  if (f.maximum !== undefined)
    a.push(
      `if (${scalar(f.type, x)} > ${lit(f.maximum, e)}) throw Error("${f.name}: maximum");`,
    );
  for (const c of f.constraints ?? []) {
    seen.add(c.op);
    if (
      c.kind !== "contains-protocol-required-capability" ||
      c.requiredCapability?.kind !== "protocol" ||
      c.requiredCapability.name !== "requiredCapability"
    )
      throw Error(`field constraint ${c.kind}`);
    a.push(
      `if (!${x}.Items.Any(item => item.Value == ${JSON.stringify(ir.protocol.requiredCapability)})) throw Error("required capability");`,
    );
  }
  return a;
}
function readFields(fs, reader, base = new Map(), flags = "0") {
  const env = new Map(base),
    a = [];
  for (const f of fs) {
    seen.add(f.op);
    const n = id(f.name);
    env.set(f.name, n);
    env.set(`${f.name}:field`, f);
    if (f.when) {
      a.push(
        `${typeOf(f.type, true)} ${n};`,
        `if (${cond(f.when, env, flags)})`,
        `{`,
        `    ${n} = ${readCall(f.type, reader, env)};`,
        ...fieldChecks(f, n).map((x) => `    ${x}`),
        `}`,
        `else ${n} = null;`,
      );
    } else
      a.push(
        `var ${n} = ${readCall(f.type, reader, env)};`,
        ...fieldChecks(f, n),
      );
  }
  return { lines: a, env, args: fs.map((f) => id(f.name)).join(", ") };
}
function writeFields(fs, writer, owner, base = new Map(), flags = "0") {
  const env = new Map(base),
    a = [];
  for (const f of fs) {
    seen.add(f.op);
    const x = `${owner}.${id(f.name)}`;
    env.set(f.name, x);
    env.set(`${f.name}:field`, f);
    if (f.when)
      a.push(
        `if (${cond(f.when, env, flags)})`,
        `{`,
        `    if (${x} is null) throw Error("${f.name}: required");`,
        ...fieldChecks(f, `${x}!`).map((y) => `    ${y}`),
        `    ${writeCall(f.type, writer, `${x}!`, env)};`,
        `}`,
        `else if (${x} is not null) throw Error("${f.name}: forbidden");`,
      );
    else a.push(...fieldChecks(f, x), `${writeCall(f.type, writer, x, env)};`);
  }
  return { lines: a, env };
}

function constraints(
  owner,
  value = "value",
  selectedOperations = null,
  caseSignature = null,
) {
  const o = main(owner),
    operations = selectedOperations ?? o.constraints ?? [],
    fields = selectedOperations
      ? caseFields({ operations: selectedOperations })
      : (o.fields ?? []),
    lines = [],
    helpers = [],
    ownerFields = new Map(fields.map((field) => [field.name, field]));
  const equals = (name, expectedValue) =>
    `${value}.${id(name)} == ${expected(ownerFields.get(name).type, expectedValue)}`;
  const constraintOperations = operations.filter(
    (operation) => operation.op === "constraint",
  );
  constraintOperations.forEach((constraint, index) => {
    seen.add(constraint.op);
    if (caseSignature !== null) {
      const expectedPath = ["types", owner.name, "cases", caseSignature];
      if (
        constraint.owner?.kind !== "conditional-union-case" ||
        JSON.stringify(constraint.owner.path) !== JSON.stringify(expectedPath)
      )
        throw Error(`${owner.name}: constraint owner`);
    } else if (constraint.owner !== undefined)
      throw Error(`${owner.name}: unexpected constraint owner`);
    if (constraint.kind === "not-both-zero")
      lines.push(
        `if (${constraint.fields.map((field) => `${value}.${id(field.name)}.Value == 0`).join(" && ")}) throw Error("not-both-zero");`,
      );
    else if (constraint.kind === "field-less-than-or-equal")
      lines.push(
        `if (${value}.${id(constraint.left.name)}.Value > ${value}.${id(constraint.right.name)}.Value) throw Error("field order");`,
      );
    else if (constraint.kind === "sorted" || constraint.kind === "unique") {
      const matchingSorted = constraintOperations.find(
        (candidate) =>
          candidate.kind === "sorted" &&
          JSON.stringify(candidate.key) === JSON.stringify(constraint.key),
      );
      if (constraint.kind === "unique" && matchingSorted) return;
      const strict =
        constraint.kind === "sorted" &&
        constraintOperations.some(
          (candidate) =>
            candidate.kind === "unique" &&
            JSON.stringify(candidate.key) === JSON.stringify(constraint.key),
        );
      if (
        constraint.comparison !== undefined &&
        !new Set([
          "utf-8-bytes",
          "canonical-authority-key-bytes",
          "wire-value-then-utf-8-bytes",
          "unsigned-wire-value",
        ]).has(constraint.comparison)
      )
        throw Error(`${owner.name}: comparison ${constraint.comparison}`);
      const property =
          o.op === "vector"
            ? "Items"
            : id(o.layout.find((x) => x.kind === "repeat").name),
        item =
          o.op === "vector"
            ? o.item
            : o.layout.find((x) => x.kind === "repeat").item,
        key = `Key${id(owner.name)}${index}`,
        body = [];
      const resolve = (reference, expression, pathValue) => {
        for (const part of pathValue ? pathValue.split(".") : []) {
          const field = main(types.get(reference.$ref)).fields.find(
            (entry) => entry.name === part,
          );
          reference = field.type;
          expression += `.${id(part)}`;
        }
        return { reference, expression };
      };
      if (constraint.key.kind === "tuple") {
        body.push("var writer = new Writer();");
        for (const part of constraint.key.parts) {
          const source = part.source,
            found = resolve(
              item,
              "item",
              source.kind === "item" ? "" : source.path,
            );
          if (part.kind === "utf-8-bytes") {
            if (part.lengthPrefix !== "excluded")
              throw Error(`${owner.name}: utf8 key prefix`);
            body.push(
              `writer.Bytes(Utf8.GetBytes(${found.expression}.Value!));`,
            );
          } else {
            const encoding = main(types.get(found.reference.$ref)).encoding;
            if (
              !new Set(["wire-value", "unsigned-wire-value"]).has(part.kind) ||
              part.encoding !== encoding ||
              part.byteOrder !== "big-endian"
            )
              throw Error(`${owner.name}: wire key`);
            body.push(
              `${writeCall(found.reference, "writer", found.expression, new Map())};`,
            );
          }
        }
        body.push("return writer.ToArray();");
      } else if (constraint.key.kind === "canonical-authority-key-bytes") {
        const source = resolve(item, "item", constraint.key.source.path),
          union = types.get(source.reference.$ref),
          uo = main(union),
          format = constraint.key.format;
        if (
          format.encoding !== "canonical-ascii-utf8" ||
          format.componentLayout !==
            "decimal-raw-byte-length-colon-percent-encoded-bytes" ||
          format.escaping !==
            "rfc3986-unreserved-literal-otherwise-uppercase-percent-hex" ||
          format.unicodeNormalization !== "none"
        )
          throw Error(`${owner.name}: authority key format`);
        const arms = [];
        for (const [variantName, variant] of Object.entries(
          constraint.key.variants,
        )) {
          const caseIndex = cases(uo).findIndex(
            ([signature]) =>
              JSON.parse(signature)[uo.discriminators[0].name] === variantName,
          );
          if (caseIndex < 0)
            throw Error(`${owner.name}: authority variant ${variantName}`);
          const selected = cases(uo)[caseIndex][1],
            kind = format.kindDiscriminators.find(
              (x) => x.objectKind === variant.objectKind,
            ),
            components = [];
          for (const component of variant.components) {
            const parts = component.split(".");
            let reference = caseFields(selected).find(
                (field) => field.name === parts[0],
              ).type,
              expression = `selected.${id(parts[0])}`;
            for (const part of parts.slice(1)) {
              const field = main(types.get(reference.$ref)).fields.find(
                (x) => x.name === part,
              );
              reference = field.type;
              expression += `.${id(part)}`;
            }
            components.push(`AuthorityComponent(${expression}.Value!)`);
          }
          arms.push(
            `${id(union.name)}Case${caseIndex} selected => Utf8.GetBytes(${JSON.stringify(`${format.prefix}${format.separator}${kind.wire}${format.separator}`)} + ${components.join(` + ${JSON.stringify(format.separator)} + `)})`,
          );
        }
        body.push(
          `return ${source.expression} switch { ${arms.join(", ")}, _ => throw Error("authority key variant") };`,
        );
      } else throw Error(`${owner.name}: key ${constraint.key.kind}`);
      helpers.push(
        `    private static byte[] ${key}(${typeOf(item)} item, DecodeContext context)\n    {\n        ${body.join("\n        ")}\n    }`,
      );
      if (constraint.kind === "sorted") {
        lines.push(
          `try { for (var index = 1; index < ${value}.${property}.Count; index++) { var previous = ${key}(${value}.${property}[index - 1], context); var current = ${key}(${value}.${property}[index], context); var compared = CompareBytes(previous, current); ${strict ? `if (compared == 0) throw Error("duplicate vector item"); ` : ""}if (compared > 0) throw Error("unsorted vector"); } } catch (OutOfMemoryException) { throw Capacity("${owner.name} constraint capacity"); }`,
        );
      } else {
        lines.push(
          `try { var keys${index} = ${value}.${property}.Select(item => ${key}(item, context)).ToArray(); if (keys${index}.Select(Convert.ToHexString).Distinct(StringComparer.Ordinal).Count() != keys${index}.Length) throw Error("duplicate vector item"); } catch (OutOfMemoryException) { throw Capacity("${owner.name} constraint capacity"); }`,
        );
      }
    } else if (constraint.kind === "terminal-success-shape") {
      const when = Object.entries(constraint.when)
          .map(([name, expectedValue]) => equals(name, expectedValue))
          .join(" && "),
        required = Object.entries(constraint.requires)
          .map(([name, expectedValue]) => equals(name, expectedValue))
          .join(" && ");
      lines.push(
        `if (${when} && !(${required})) throw Error("terminal success shape");`,
      );
    } else if (constraint.kind === "terminal-failure-shape") {
      const [rawName, expectedValue] = Object.entries(constraint.when)[0],
        name = rawName.endsWith("Not") ? rawName.slice(0, -3) : rawName,
        required = Object.entries(constraint.requires)
          .map(([fieldName, fieldValue]) => equals(fieldName, fieldValue))
          .join(" && ");
      lines.push(
        `if (!(${equals(name, expectedValue)}) && !(${required})) throw Error("terminal failure shape");`,
      );
    } else if (constraint.kind === "existing-has-no-application-payload") {
      const [pathValue, discriminatorValue] = Object.entries(
          constraint.when,
        )[0],
        [fieldName, discriminatorName] = pathValue.split("."),
        union = types.get(ownerFields.get(fieldName).type.$ref),
        caseIndex = cases(main(union)).findIndex(
          ([signature]) =>
            JSON.parse(signature)[discriminatorName] === discriminatorValue,
        ),
        required = Object.entries(constraint.requires)
          .map(([name, expectedValue]) => equals(name, expectedValue))
          .join(" && ");
      if (caseIndex < 0) throw Error(`${owner.name}: constraint case`);
      lines.push(
        `if (${value}.${id(fieldName)} is ${id(union.name)}Case${caseIndex} && !(${required})) throw Error("existing payload");`,
      );
    } else throw Error(`constraint ${constraint.kind}`);
  });
  return { lines, helpers };
}
function predicates(owner, value, operations = owner.operations) {
  const a = [];
  for (const p of operations.filter((x) => x.op === "runtime-predicate")) {
    seen.add(p.op);
    if (
      p.reference.asset !== "service-wire-constants" ||
      p.reference.name !== "valid-terminal-failure"
    )
      throw Error(`predicate ${p.reference.name}`);
    if (
      main(owner).op === "conditional-union" &&
      operations === owner.operations
    ) {
      const i = cases(main(owner)).findIndex(([, c]) =>
        caseFields(c).some((f) => f.name === "failureCode"),
      );
      if (i < 0) throw Error(`${owner.name}: predicate target`);
      a.push(
        `if (${value} is ${id(owner.name)}Case${i} terminal) ValidateTerminalFailure(terminal.TerminalResult, terminal.FailureCode);`,
      );
    } else
      a.push(
        `ValidateTerminalFailure(${value}.TerminalResult, ${value}.FailureCode);`,
      );
  }
  return a;
}

function logicalDotnetFrameCall(reference, end, values) {
  const target = types.get(reference.$ref),
    u = main(target),
    args = [end];
  if (u.op === "conditional-union")
    for (const d of u.discriminators.filter((x) => x.source.kind !== "wire"))
      args.push(
        d.source.kind === "enclosingField"
          ? values.get(d.source.name)
          : `m.Context.${id(d.source.name)} ?? throw Error("missing context ${d.source.name}")`,
      );
  return `new Logical${id(target.name)}Frame(${args.join(",")})`;
}
function logicalDotnetFieldCheck(field, value) {
  return fieldChecks(field, value).join("");
}
function logicalDotnetFields(
  fields,
  startState,
  endExpression,
  valueArray = "values",
  base = new Map(),
) {
  const values = new Map(base),
    lines = [];
  let state = startState;
  fields.forEach((field, index) => {
    const typed = `((${typeOf(field.type)})${valueArray}[${index}]!)`;
    values.set(`${field.name}:field`, field);
    const skip = field.when
      ? `if(!(${cond(field.when, values, "0")})){${valueArray}[${index}]=null;State=${state + 2};return;}`
      : "";
    lines.push(
      `case ${state}:{${skip}State=${state + 1};m.Push(${logicalDotnetFrameCall(field.type, endExpression, values)});return;}`,
      `case ${state + 1}:{${valueArray}[${index}]=TakeChild();${logicalDotnetFieldCheck(field, typed)}State=${state + 2};return;}`,
    );
    values.set(field.name, typed);
    state += 2;
  });
  return { lines, values, nextState: state };
}
function logicalDotnetLimit(type, expression) {
  const limit = type.operations.find((x) => x.op === "encoded-limit");
  if (!limit) return "";
  seen.add(limit.op);
  return `if((ulong)${expression}>${lit(limit.maximumEncodedBytes)})throw Error("${type.name}: encoded limit");`;
}
function logicalDotnetFrame(type) {
  const o = main(type),
    n = id(type.name),
    frame = `Logical${n}Frame`,
    limit = logicalDotnetLimit(type, "(m.Consumed-Start)");
  if (o.op === "integer") {
    const raw =
        o.encoding === "i64"
          ? "unchecked((long)m.UInt(bytes))"
          : `checked((${intType(o.encoding)})m.UInt(bytes))`,
      checks = [];
    if (o.minimum !== undefined)
      checks.push(
        `if(value<${lit(o.minimum, o.encoding)})throw Error("${type.name}: range");`,
      );
    if (o.maximum !== undefined)
      checks.push(
        `if(value>${lit(o.maximum, o.encoding)})throw Error("${type.name}: range");`,
      );
    return `    private sealed class ${frame}:LogicalFrame{internal ${frame}(long end):base(end){}internal override void Resume(LogicalMachine m){var context=m.Context;var bytes=m.Take(this,${o.width},true);if(bytes is null)return;var value=${raw};${checks.join("")}${limit}m.Complete(new ${n}(value));}}`;
  }
  if (o.op === "enum") {
    const arms = o.values
      .map((v) => `${lit(v.value, o.encoding)}=>${n}.${id(v.name)}`)
      .join(",");
    return `    private sealed class ${frame}:LogicalFrame{internal ${frame}(long end):base(end){}internal override void Resume(LogicalMachine m){var context=m.Context;var bytes=m.Take(this,${o.width},true);if(bytes is null)return;var raw=m.UInt(bytes);var value=raw switch{${arms},_=>throw Error("${type.name}: unknown enum")};${limit}m.Complete(value);}}`;
  }
  if (o.op === "length-prefixed") {
    const length = id(o.lengthType.$ref),
      absent = o.zeroLengthMeaning
        ? `if(length==0){${limit}m.Complete(new ${n}(null));return;}`
        : "",
      text = type.operations.some((x) => x.op === "text-validation"),
      negotiated = type.operations.find((x) => x.op === "negotiated-bound");
    if (text) seen.add("text-validation");
    if (negotiated) seen.add(negotiated.op);
    const negotiatedCheck = negotiated
      ? `var maximum=m.Context.${id(negotiated.context.name)}??throw Error("missing context ${negotiated.context.name}");if(maximum<0||maximum>${lit(negotiated.context.absoluteMaximum, "i64")}||(ulong)length>(ulong)maximum)throw Error("${type.name}: negotiated bound");`
      : "";
    const prefix = `case 0:{State=1;m.Push(new Logical${length}Frame(End));return;}case 1:{var encodedLength=(ulong)((${typeOf(o.lengthType)})TakeChild()).Value;if(encodedLength>${lit(o.encodeCapacity.representationCeiling)})throw Capacity("${type.name}: capacity");if(encodedLength<${lit(o.minimumBytes)}||encodedLength>${lit(o.encodeCapacity.declaredMaximumBytes)})throw Error("${type.name}: length");length=checked((int)encodedLength);${negotiatedCheck}${absent}`;
    if (text) {
      const validation = type.operations.find(
          (entry) => entry.op === "text-validation",
        ),
        forbidNul = validation?.nul === "forbidden";
      return `    private sealed class ${frame}:LogicalFrame{private int length,remaining,codePoint,needed,minimum;private readonly StringBuilder value=new();internal ${frame}(long end):base(end){}private void Append(int item){try{if(item<=0xffff)value.Append((char)item);else{item-=0x10000;value.Append((char)(0xd800+(item>>10)));value.Append((char)(0xdc00+(item&0x3ff)));}}catch(OutOfMemoryException){throw Capacity("${type.name}: capacity");}}private string Finish(){try{return value.ToString();}catch(OutOfMemoryException){throw Capacity("${type.name}: capacity");}}internal override void Resume(LogicalMachine m){var context=m.Context;switch(State){${prefix}remaining=length;State=2;return;}case 2:{while(remaining>0){var item=m.ReadByte(this,0);if(item<0)return;remaining--;if(needed==0){if(item<=0x7f){${forbidNul ? `if(item==0)throw Error("${type.name}: NUL");` : ""}Append(item);continue;}if(item>=0xc2&&item<=0xdf){codePoint=item&0x1f;needed=1;minimum=0x80;continue;}if(item>=0xe0&&item<=0xef){codePoint=item&0x0f;needed=2;minimum=0x800;continue;}if(item>=0xf0&&item<=0xf4){codePoint=item&0x07;needed=3;minimum=0x10000;continue;}throw Error("${type.name}: UTF-8");}if((item&0xc0)!=0x80)throw Error("${type.name}: UTF-8");codePoint=(codePoint<<6)|(item&0x3f);if(--needed==0){if(codePoint<minimum||codePoint>0x10ffff||(codePoint>=0xd800&&codePoint<=0xdfff)${forbidNul ? "||codePoint==0" : ""})throw Error("${type.name}: UTF-8");Append(codePoint);}}if(needed!=0)throw Error("${type.name}: UTF-8");${limit}m.Complete(new ${n}(Finish()));return;}default:throw new InvalidOperationException();}}}`;
    }
    return `    private sealed class ${frame}:LogicalFrame{private int length;private byte[] value=Array.Empty<byte>();internal ${frame}(long end):base(end){}internal override void Resume(LogicalMachine m){var context=m.Context;switch(State){${prefix}try{value=GC.AllocateUninitializedArray<byte>(length);}catch(OutOfMemoryException){throw Capacity("${type.name}: capacity");}State=2;return;}case 2:{if(!m.Fill(this,value))return;${limit}m.Complete(new ${n}(value));return;}default:throw new InvalidOperationException();}}}`;
  }
  if (o.op === "struct" || o.op === "versioned-length-delimited") {
    const delimited = o.op === "versioned-length-delimited";
    let prefix = "",
      start = 0,
      end = "End";
    if (delimited) {
      prefix = `case 0:{State=1;m.Push(new Logical${id(o.version.$ref)}Frame(End));return;}case 1:{var version=(${typeOf(o.version)})TakeChild();if(version.Value!=${lit(o.version.constant, main(types.get(o.version.$ref)).encoding)})throw Error("${type.name}: version");State=2;m.Push(new Logical${id(o.length.$ref)}Frame(End));return;}case 2:{bodyEnd=m.Bound(End,(ulong)((${typeOf(o.length)})TakeChild()).Value);State=3;return;}`;
      start = 3;
      end = "bodyEnd";
    }
    const f = logicalDotnetFields(o.fields, start, end),
      args = o.fields
        .map(
          (x, i) =>
            `((${typeOf(x.type, optional(x))})values[${i}]${optional(x) ? "" : "!"})`,
        )
        .join(","),
      c = constraints(type, "value"),
      p = predicates(type, "value"),
      finish = `${delimited ? `m.Exact(bodyEnd,"${type.name}");` : ""}var value=new ${n}(${args});${c.lines.join("")}${p.join("")}${limit}m.Complete(value);return;`;
    return `    private sealed class ${frame}:LogicalFrame{private readonly object?[] values=new object?[${o.fields.length}];${delimited ? "private long bodyEnd;" : ""}internal ${frame}(long end):base(end){}internal override void Resume(LogicalMachine m){var context=m.Context;switch(State){${prefix}${f.lines.join("")}case ${f.nextState}:{${finish}}default:throw new InvalidOperationException();}}}`;
  }
  if (o.op === "vector" || o.op === "versioned-vector") {
    const versioned = o.op === "versioned-vector",
      repeat = versioned
        ? o.layout.find((x) => x.kind === "repeat")
        : { name: "items", item: o.item },
      countRef = versioned ? o.layout.find((x) => x.counts) : o.countType,
      itemType = typeOf(repeat.item),
      prop = versioned ? id(repeat.name) : "Items";
    let prefix, loop;
    if (versioned) {
      const v = o.layout[0];
      prefix = `case 0:{State=1;m.Push(new Logical${id(v.$ref)}Frame(End));return;}case 1:{var version=(${typeOf(v)})TakeChild();if(version.Value!=${lit(v.constant, main(types.get(v.$ref)).encoding)})throw Error("${type.name}: version");State=2;m.Push(new Logical${id(countRef.$ref)}Frame(End));return;}case 2:{var encodedCount=(ulong)((${typeOf(countRef)})TakeChild()).Value;if(encodedCount>int.MaxValue)throw Capacity("${type.name}: capacity");count=(int)encodedCount;${o.maximumItems !== undefined ? `if(count>${o.maximumItems})throw Error("${type.name}: count");` : ""}values=new List<${itemType}>();State=3;return;}`;
      loop = 3;
    } else {
      prefix = `case 0:{State=1;m.Push(new Logical${id(countRef.$ref)}Frame(End));return;}case 1:{var encodedCount=(ulong)((${typeOf(countRef)})TakeChild()).Value;if(encodedCount>int.MaxValue)throw Capacity("${type.name}: capacity");count=(int)encodedCount;${o.maximumItems !== undefined ? `if(count>${o.maximumItems})throw Error("${type.name}: count");` : ""}values=new List<${itemType}>();State=2;return;}`;
      loop = 2;
    }
    const c = constraints(type, "value");
    return `    private sealed class ${frame}:LogicalFrame{private int count,index;private List<${itemType}> values=null!;internal ${frame}(long end):base(end){}internal override void Resume(LogicalMachine m){var context=m.Context;switch(State){${prefix}case ${loop}:{if(index==count){var value=new ${n}(values);${c.lines.join("")}${limit}m.Complete(value);return;}State=${loop + 1};m.Push(${logicalDotnetFrameCall(repeat.item, "End", new Map())});return;}case ${loop + 1}:{try{values.Add((${itemType})TakeChild());}catch(OutOfMemoryException){throw Capacity("${type.name}: capacity");}index++;State=${loop};return;}default:throw new InvalidOperationException();}}}`;
  }
  if (o.op === "conditional-union") {
    const ds = o.discriminators,
      ext = ds.filter((x) => x.source.kind !== "wire"),
      parms = ext.map((x, i) => `${typeOf(x.type)} external${i}`).join(","),
      assigns = [];
    let ei = 0,
      state = 0;
    const values = new Map(),
      lines = [];
    ds.forEach((d, i) => {
      if (d.source.kind !== "wire")
        assigns.push(`values[${i}]=external${ei++};`);
      values.set(d.name, `((${typeOf(d.type)})values[${i}]!)`);
    });
    ds.forEach((d, i) => {
      if (d.source.kind === "wire") {
        lines.push(
          `case ${state}:{State=${state + 1};m.Push(${logicalDotnetFrameCall(d.type, "End", values)});return;}`,
          `case ${state + 1}:{values[${i}]=TakeChild();State=${state + 2};return;}`,
        );
        state += 2;
      }
    });
    let bodyEnd = "End";
    if (o.bodyLengthType) {
      lines.push(
        `case ${state}:{State=${state + 1};m.Push(new Logical${id(o.bodyLengthType.$ref)}Frame(End));return;}`,
        `case ${state + 1}:{bodyEnd=m.Bound(End,(ulong)((${typeOf(o.bodyLengthType)})TakeChild()).Value);State=${state + 2};return;}`,
      );
      state += 2;
      bodyEnd = "bodyEnd";
    }
    const select = state,
      blocks = [];
    let next = state + 1;
    cases(o).forEach(([sig, selected], ci) => {
      const expectedValues = JSON.parse(sig),
        test = ds
          .map((d) => {
            const target = main(types.get(d.type.$ref)),
              value = values.get(d.name),
              expectedValue = expectedValues[d.name];
            return target.op === "enum"
              ? `${value}==${id(d.type.$ref)}.${id(expectedValue)}`
              : `${value}.Value==${lit(expectedValue, target.encoding)}`;
          })
          .join("&&"),
        fs = caseFields(selected),
        f = logicalDotnetFields(fs, next, bodyEnd, "caseValues", values),
        wire = ds
          .filter((d) => d.source.kind === "wire")
          .map((d) => values.get(d.name)),
        args = [
          ...wire,
          ...fs.map(
            (x, i) =>
              `((${typeOf(x.type, optional(x))})caseValues[${i}]${optional(x) ? "" : "!"})`,
          ),
        ].join(","),
        operations = selected.operations,
        c = constraints(type, "value", operations, sig),
        fieldNames = new Set(fs.map((x) => x.name)),
        p =
          fieldNames.has("terminalResult") && fieldNames.has("failureCode")
            ? predicates(type, "value", operations)
            : [];
      blocks.push(
        `${ci ? "else " : ""}if(${test}){caseValues=new object?[${fs.length}];State=${next};return;}`,
      );
      lines.push(
        ...f.lines,
        `case ${f.nextState}:{${o.bodyLengthType ? `m.Exact(bodyEnd,"${type.name}");` : ""}var value=new ${n}Case${ci}(${args});${c.lines.join("")}${p.join("")}${limit}m.Complete(value);return;}`,
      );
      next = f.nextState + 1;
    });
    lines.push(
      `case ${select}:{${blocks.join("")}throw Error("${type.name}: discriminator");}`,
    );
    return `    private sealed class ${frame}:LogicalFrame{private readonly object?[] values=new object?[${ds.length}];private object?[] caseValues=Array.Empty<object?>();${o.bodyLengthType ? "private long bodyEnd;" : ""}internal ${frame}(long end${parms ? "," + parms : ""}):base(end){${assigns.join("")}}internal override void Resume(LogicalMachine m){var context=m.Context;switch(State){${lines.join("")}default:throw new InvalidOperationException();}}}`;
  }
  throw Error(`${type.name}: unsupported logical frame ${o.op}`);
}

function tlvPresence(operation, environment) {
  const lines = [];
  for (const rule of operation.presenceRules) {
    const test = cond(rule.when, environment);
    for (const name of rule.require ?? [])
      lines.push(
        `if (${test} && ${environment.get(name)} is null) throw Error("${name}: presence required");`,
      );
    for (const name of rule.forbid ?? [])
      lines.push(
        `if (${test} && ${environment.get(name)} is not null) throw Error("${name}: presence forbidden");`,
      );
  }
  return lines;
}

function assertSyntaxCoverage() {
  const fail = (owner, operation, detail) => {
    throw Error(`${owner}:${operation.op}: unsupported ${detail}`);
  };
  const inspectField = (owner, field) => {
    if (
      field.when &&
      (field.whenFalse?.encoder !== "reject-present-value" ||
        field.whenFalse?.decoder !== "consume-no-bytes")
    )
      fail(owner, field, "whenFalse");
    if (field.otherwise !== undefined && field.otherwise !== "forbidden")
      fail(owner, field, "otherwise");
  };
  for (const type of ir.types) {
    for (const operation of type.operations) {
      if (operation.op === "integer" || operation.op === "enum") {
        if (operation.byteOrder !== "big-endian")
          fail(type.name, operation, "byte order");
        if (operation.op === "enum" && operation.unknown !== "protocol-error")
          fail(type.name, operation, "enum unknown policy");
      } else if (operation.op === "length-prefixed") {
        const capacity = operation.encodeCapacity;
        if (
          !new Set(["bytes", "text"]).has(operation.content) ||
          capacity.declaredMaximumBytes !== operation.maximumBytes ||
          !Number.isSafeInteger(capacity.representationCeiling) ||
          !Number.isSafeInteger(capacity.requiredThroughBytes) ||
          capacity.applications.join(",") !== "encode,decode" ||
          capacity.aboveDeclaredMaximum !== "protocol-error" ||
          capacity.aboveRepresentationCeiling !== "capacity-error"
        )
          fail(type.name, operation, "content or encode capacity");
      } else if (operation.op === "text-validation") {
        if (
          operation.encoding !== "utf-8" ||
          operation.malformed !== "protocol-error" ||
          operation.nul !== "forbidden" ||
          operation.decode.bom !== "preserve" ||
          operation.decode.overlong !== "protocol-error" ||
          operation.decode.surrogateCodePoint !== "protocol-error" ||
          operation.encode.loneSurrogate !== "protocol-error"
        )
          fail(type.name, operation, "text policy");
      } else if (operation.op === "struct") {
        if (operation.order !== "sequential")
          fail(type.name, operation, "struct order");
        operation.fields.forEach((field) => inspectField(type.name, field));
      } else if (operation.op === "versioned-length-delimited") {
        if (
          operation.reader.op !== "bounded-reader" ||
          operation.reader.trailingBytes !== "forbidden"
        )
          fail(type.name, operation, "bounded reader");
        operation.fields.forEach((field) => inspectField(type.name, field));
      } else if (operation.op === "versioned-vector") {
        if (
          operation.order !== "sequential" ||
          operation.trailingBytes !== "forbidden"
        )
          fail(type.name, operation, "versioned vector policy");
      } else if (operation.op === "conditional-union") {
        if (
          operation.discriminators.some(
            (discriminator) =>
              !new Set(["wire", "context", "enclosingField"]).has(
                discriminator.source.kind,
              ),
          )
        )
          fail(type.name, operation, "discriminator source");
        if (
          operation.encode.selection !== "variant" ||
          operation.encode.discriminatorAgreement !== "required" ||
          operation.encode.mismatch !== "protocol-error"
        )
          fail(type.name, operation, "encode agreement");
        for (const selected of Object.values(operation.cases)) {
          for (const selectedOperation of selected.operations) {
            if (selectedOperation.op === "field")
              inspectField(type.name, selectedOperation);
            else if (
              !new Set(["constraint", "runtime-predicate"]).has(
                selectedOperation.op,
              )
            )
              fail(type.name, selectedOperation, "union case operation");
          }
        }
        if (operation.otherwise.kind === "fields")
          operation.otherwise.fields.forEach((field) =>
            inspectField(type.name, field),
          );
      } else if (operation.op === "tlv32") {
        if (
          operation.encodingOrder !== "ascending-field-id" ||
          operation.duplicateField !== "protocol-error" ||
          operation.unknownField !== "skip-after-length-validation" ||
          JSON.stringify(operation.requiredFields) !==
            JSON.stringify(
              operation.fields
                .filter((field) => field.required)
                .map((field) => field.name),
            )
        )
          fail(type.name, operation, "TLV policy");
      } else if (operation.op === "encoded-limit") {
        if (
          operation.measured !== "complete-encoded-value" ||
          operation.applications.join(",") !== "encode,decode" ||
          operation.exceeded !== "protocol-error" ||
          (!operation.includes.includes("body") &&
            !operation.includes.includes("fields"))
        )
          fail(type.name, operation, "encoded limit policy");
      } else if (operation.op === "negotiated-bound") {
        if (
          operation.topology !== "clientServer" ||
          operation.context.missing !== "protocol-error" ||
          operation.context.negative !== "protocol-error" ||
          operation.context.aboveAbsoluteMaximum !== "protocol-error" ||
          operation.applications.encode.context.kind !== "encoder-context" ||
          operation.applications.decode.context.kind !== "decoder-context" ||
          operation.applications.encode.context.name !==
            operation.context.name ||
          operation.applications.decode.context.name !==
            operation.context.name ||
          !new Set(["content-bytes", "encoded-bytes"]).has(
            operation.measured,
          ) ||
          operation.comparison !== "less-than-or-equal"
        )
          fail(type.name, operation, "negotiated bound policy");
      } else if (
        operation.op !== "vector" &&
        operation.op !== "runtime-predicate"
      )
        fail(type.name, operation, "operation");
    }
  }
  for (const command of ir.commands) {
    for (const operation of command.operations) {
      if (
        operation.op === "command-header" &&
        operation.byteOrder !== "big-endian"
      )
        fail(command.name, operation, "byte order");
      else if (
        operation.op === "flags" &&
        operation.unknown !== "protocol-error"
      )
        fail(command.name, operation, "flag policy");
      else if (
        operation.op === "flag-constraint" &&
        !new Set(["all-or-none", "implies"]).has(operation.kind)
      )
        fail(command.name, operation, "flag constraint");
      else if (
        operation.op === "metadata-flag-frame" &&
        (operation.whenSet !== "frame-required" ||
          operation.whenClear !== "frame-forbidden")
      )
        fail(command.name, operation, "metadata frame policy");
      else if (
        operation.op === "payload" &&
        !new Set(["forbidden", "optional", "required"]).has(operation.policy)
      )
        fail(command.name, operation, "payload policy");
      else if (operation.op === "field") inspectField(command.name, operation);
      else if (
        !new Set([
          "command-header",
          "flags",
          "flag-constraint",
          "metadata-flag-frame",
          "payload",
          "runtime-predicate",
        ]).has(operation.op)
      )
        fail(command.name, operation, "operation");
    }
  }
  for (const format of ir.durableFormats) {
    const header = op(format, "durable-header"),
      bounded = op(format, "bounded-reader"),
      checksum = op(format, "checksum");
    if (header.byteOrder !== "big-endian" || header.flagsComparison !== "exact")
      fail(format.name, header, "header policy");
    if (
      bounded.boundary !== "bodyLength" ||
      bounded.trailingBytes !== "checksum-only"
    )
      fail(format.name, bounded, "bounded reader");
    if (
      checksum.algorithm !== "crc32c-castagnoli" ||
      checksum.encoding !== "u32-big-endian" ||
      checksum.coverage !== "magic-through-body" ||
      checksum.position !== "trailing" ||
      checksum.mismatch !== "protocol-error" ||
      checksum.verification !== "before-body-interpretation"
    )
      fail(format.name, checksum, "checksum policy");
    const body = op(format, "field");
    if (
      body.name !== "body" ||
      body.interpretation !== "after-checksum-verification"
    )
      fail(format.name, body, "body interpretation");
  }
  const logical = op(ir.relocationLogicalStreamFormat, "logical-stream"),
    logicalLimit = op(ir.relocationLogicalStreamFormat, "encoded-limit");
  if (
    logical.encoding !==
    "canonical-big-endian-field-stream-without-monolithic-provider-envelope"
  )
    fail(ir.relocationLogicalStreamFormat.name, logical, "logical encoding");
  if (
    logicalLimit.measured !== "complete-encoded-value" ||
    logicalLimit.includes.join(",") !== "logical-stream-bytes" ||
    logicalLimit.applications.join(",") !== "encode,decode" ||
    logicalLimit.exceeded !== "protocol-error"
  )
    fail(
      ir.relocationLogicalStreamFormat.name,
      logicalLimit,
      "logical encoded limit",
    );
}

function commandDecl(c) {
  const fs = c.operations.filter((x) => x.op === "field"),
    p = op(c, "payload"),
    a = [
      "byte Flags",
      ...fs.map((f) => `${typeOf(f.type, !!f.when)} ${id(f.name)}`),
    ];
  const m = c.operations.find((x) => x.op === "metadata-flag-frame");
  if (m) a.push(`${typeOf(m.frame, true)} Metadata`);
  if (p.policy !== "forbidden")
    a.push(`${typeOf(p.type, p.policy === "optional")} Payload`);
  return `internal sealed record ${id(c.name)}${c.id}(${a.join(",")});`;
}
function flagChecks(c, x) {
  const f = op(c, "flags"),
    allow = f.allowed.reduce((a, v) => a | flagBits.get(v.name), 0),
    req = f.required.reduce((a, v) => a | flagBits.get(v.name), 0),
    r = [`if((${x}&~${allow})!=0||(${x}&${req})!=${req})throw Error("flags");`];
  for (const q of c.operations.filter((v) => v.op === "flag-constraint")) {
    seen.add(q.op);
    const bs = (q.flags ?? []).map((v) => flagBits.get(v.name));
    r.push(
      q.kind === "all-or-none"
        ? `var fc=${bs.map((b) => `((${x}&${b})!=0?1:0)`).join("+")};if(fc!=0&&fc!=${bs.length})throw Error("flag constraint");`
        : `if((${x}&${flagBits.get(q.if.name)})!=0&&!(${q.then.map((v) => `(${x}&${flagBits.get(v.name)})!=0`).join("&&")}))throw Error("flag implication");`,
    );
  }
  return r;
}
function commandMethods(c) {
  const n = `${id(c.name)}${c.id}`,
    h = op(c, "command-header"),
    fs = c.operations.filter((x) => x.op === "field"),
    p = op(c, "payload"),
    m = c.operations.find((x) => x.op === "metadata-flag-frame"),
    rf = readFields(fs, "body", new Map(), "flags"),
    wf = writeFields(fs, "body", "value", new Map(), "value.Flags"),
    ctor = ["flags", ...fs.map((f) => id(f.name))],
    r = [
      'if(frames.Count==0)throw Error("frames");',
      "var body=new Reader(frames[0]);",
      ...h.magic.map((v) => `if(body.U8()!=${v})throw Error("magic");`),
      `if(body.U8()!=${h.wireMajor}||body.U8()!=${h.commandId})throw Error("header");`,
      `var flags=body.U8();`,
      ...flagChecks(c, "flags"),
      ...rf.lines,
      'body.End("body");',
      "var index=1;",
    ],
    w = [
      ...flagChecks(c, "value.Flags"),
      "var body=new Writer();",
      ...h.magic.map((v) => `body.U8(${v});`),
      `body.U8(${h.wireMajor});body.U8(${h.commandId});body.U8(value.Flags);`,
      ...wf.lines,
      "var frames=new List<byte[]>{body.ToArray()};",
    ];
  if (m) {
    seen.add(m.op);
    const b = flagBits.get(m.flag.name);
    r.push(
      `${typeOf(m.frame, true)} metadata=null;`,
      `if((flags&${b})!=0){if(index>=frames.Count)throw Error("metadata required");var x=new Reader(frames[index++]);metadata=${readCall(m.frame, "x", new Map())};x.End("metadata");}`,
    );
    ctor.push("metadata");
    w.push(
      `if((value.Flags&${b})!=0){if(value.Metadata is null)throw Error("metadata required");var x=new Writer();${writeCall(m.frame, "x", "value.Metadata", new Map())};frames.Add(x.ToArray());}else if(value.Metadata is not null)throw Error("metadata forbidden");`,
    );
  }
  if (p.policy !== "forbidden") {
    r.push(
      `${typeOf(p.type, true)} payload=null;`,
      p.policy === "required"
        ? `if(index>=frames.Count)throw Error("payload required");`
        : "",
      `if(index<frames.Count){var x=new Reader(frames[index++]);payload=${readCall(p.type, "x", new Map())};x.End("payload");}`,
    );
    ctor.push(`payload${p.policy === "required" ? "!" : ""}`);
    w.push(
      p.policy === "required"
        ? `if(value.Payload is null)throw Error("payload required");`
        : "",
      `if(value.Payload is not null){var x=new Writer();${writeCall(p.type, "x", "value.Payload", new Map())};frames.Add(x.ToArray());}`,
    );
  }
  r.push(
    'if(index!=frames.Count)throw Error("extra frame");',
    `var value=new ${n}(${ctor.join(",")});`,
    ...predicates(c, "value"),
    "return value;",
  );
  w.push(...predicates(c, "value"), "return frames.ToArray();");
  return `    internal static ${n} Decode${n}(IReadOnlyList<byte[]> frames,DecodeContext context)\n    {\n        ${r.filter(Boolean).join("\n        ")}\n    }\n    internal static ${n} Decode${n}(byte[] frame,DecodeContext context)=>Decode${n}(new[]{frame},context);\n    internal static byte[][] Encode${n}(${n} value,DecodeContext context)\n    {\n        ${w.filter(Boolean).join("\n        ")}\n    }`;
}

function durable(f) {
  const h = op(f, "durable-header"),
    br = op(f, "bounded-reader"),
    cs = op(f, "checksum"),
    l = op(f, "encoded-limit"),
    bf = op(f, "field");
  seen.add(br.op);
  seen.add(cs.op);
  if (
    l.measured !== "complete-encoded-value" ||
    l.applications.join(",") !== "encode,decode" ||
    l.exceeded !== "protocol-error" ||
    bf.interpretation !== "after-checksum-verification"
  )
    throw Error(`${f.name}: durable operation policy`);
  const n = id(f.name),
    b = rn(bf.type);
  return `    internal static ${b} DecodeDurable${n}(byte[] bytes,DecodeContext context){if(bytes.LongLength>${l.maximumEncodedBytes}L)throw Error("limit");var r=new Reader(bytes);${h.magic.map((v) => `if(r.U8()!=${v})throw Error("magic");`).join("")}if(r.U8()!=${h.formatVersion})throw Error("version");if(Read${rn(h.flagsType)}(r,context).Value!=${h.flags})throw Error("flags");var x=r.Slice(checked((int)Read${rn(h.bodyLengthType)}(r,context).Value));var sum=r.U32();r.End("durable");if(Crc32C(bytes.AsSpan(0,bytes.Length-4))!=sum)throw Error("checksum");var v=Read${b}(x,context);x.End("body");return v;}\n    internal static byte[] EncodeDurable${n}(${b} value,DecodeContext context){var body=new Writer();Write${b}(body,value,context);var w=new Writer();${h.magic.map((v) => `w.U8(${v});`).join("")}w.U8(${h.formatVersion});Write${rn(h.flagsType)}(w,new ${rn(h.flagsType)}(${h.flags}),context);Write${rn(h.bodyLengthType)}(w,new ${rn(h.bodyLengthType)}(checked((${intType(main(types.get(h.bodyLengthType.$ref)).encoding)})body.Length)),context);w.Bytes(body.ToArray());var p=w.ToArray();w.U32(Crc32C(p));var r=w.ToArray();if(r.LongLength>${l.maximumEncodedBytes}L)throw Error("limit");return r;}`;
}
const runtime = String.raw`    private sealed class Reader{private readonly byte[]b;private readonly int limit;private int p;internal Reader(byte[] bytes){b=bytes;limit=bytes.Length;}internal int Remaining=>limit-p;private void N(int n){if(n<0||Remaining<n)throw new EndOfStreamException("truncated");}internal byte U8()=>Bytes(1)[0];internal ushort U16()=>BinaryPrimitives.ReadUInt16BigEndian(Bytes(2));internal uint U32()=>BinaryPrimitives.ReadUInt32BigEndian(Bytes(4));internal ulong U64()=>BinaryPrimitives.ReadUInt64BigEndian(Bytes(8));internal long I64()=>BinaryPrimitives.ReadInt64BigEndian(Bytes(8));internal byte[] Bytes(int n){N(n);var value=b.AsSpan(p,n).ToArray();p+=n;return value;}internal Reader Slice(int n)=>new(Bytes(n));internal void End(string n){if(Remaining!=0)throw Error(n+": trailing");}} private sealed class Writer{private readonly List<byte[]>chunks=[];private long length;internal long Length=>length;private void Add(byte[]v){chunks.Add(v);length=checked(length+v.LongLength);}internal void U8(byte v)=>Add([v]);internal void U16(ushort v){var x=new byte[2];BinaryPrimitives.WriteUInt16BigEndian(x,v);Add(x);}internal void U32(uint v){var x=new byte[4];BinaryPrimitives.WriteUInt32BigEndian(x,v);Add(x);}internal void U64(ulong v){var x=new byte[8];BinaryPrimitives.WriteUInt64BigEndian(x,v);Add(x);}internal void I64(long v){var x=new byte[8];BinaryPrimitives.WriteInt64BigEndian(x,v);Add(x);}internal void Bytes(byte[]v)=>Add(v);internal byte[]ToArray(){if(length>int.MaxValue)throw Capacity("writer capacity");var result=GC.AllocateUninitializedArray<byte>((int)length);var offset=0;foreach(var chunk in chunks){chunk.CopyTo(result,offset);offset+=chunk.Length;}return result;}} private sealed class CapacityException(string message):Exception(message);private static CapacityException Capacity(string m)=>new(m);private static readonly UTF8Encoding Utf8=new(false,true);private static string StrictText(byte[] bytes,string name){try{return Utf8.GetString(bytes);}catch(DecoderFallbackException failure){throw new InvalidDataException(name+": UTF-8",failure);}}private static InvalidDataException Error(string m)=>new(m);private static int CompareBytes(byte[]a,byte[]b){var n=Math.Min(a.Length,b.Length);for(var i=0;i<n;i++){var c=a[i].CompareTo(b[i]);if(c!=0)return c;}return a.Length.CompareTo(b.Length);}private static string AuthorityComponent(string value){var bytes=Utf8.GetBytes(value);var result=new StringBuilder().Append(bytes.Length).Append(':');foreach(var b in bytes){if((b>=(byte)'A'&&b<=(byte)'Z')||(b>=(byte)'a'&&b<=(byte)'z')||(b>=(byte)'0'&&b<=(byte)'9')||b is (byte)'-' or (byte)'.' or (byte)'_' or (byte)'~')result.Append((char)b);else result.Append('%').Append(b.ToString("X2"));}return result.ToString();}private static uint Crc32C(ReadOnlySpan<byte>b){var c=uint.MaxValue;foreach(var v in b){c^=v;for(var i=0;i<8;i++)c=(c>>1)^(0x82f63b78u&(uint)-(int)(c&1));}return~c;}
    internal static void ValidateTerminalFailure(RequestTerminalResult result, FrameworkErrorCode failure){if(!ServiceWireConstants.ValidTerminalFailure((uint)result,(uint)failure))throw Error("terminal failure integrity");}`;
function render() {
  assertSyntaxCoverage();
  const contextFields = [
    ...ir.semanticContexts.map(
      (x) =>
        `${x.valueType ? typeOf(x.valueType, true) : "bool?"} ${id(x.name)}`,
    ),
    ...negotiatedContexts.map((name) => `long? ${id(name)}`),
  ];
  const ctx = `internal sealed record DecodeContext(${contextFields.join(",")}){internal static readonly DecodeContext Empty=new(${contextFields.map(() => "null").join(",")});}`;
  const lo = ir.relocationLogicalStreamFormat,
    lop = op(lo, "logical-stream"),
    ll = op(lo, "encoded-limit"),
    ln = id(lo.name),
    lb = rn(lop.body),
    machine = lop.decode;
  if (
    JSON.stringify(machine?.replay) !== JSON.stringify(lop.replay) ||
    machine.replay.mode !== "bounded-incremental-decode" ||
    machine.replay.wholeStreamInputAllocation !== "forbidden" ||
    machine.replay.completion !== "success-only-on-final-chunk" ||
    machine.replay.incompleteFinalChunk !== "truncation" ||
    machine.replay.bytesAfterRoot !== "trailing-bytes" ||
    !Array.isArray(machine.frames?.kinds) ||
    !Array.isArray(machine.frames.reachableTypes) ||
    !Number.isInteger(machine.frames.maximumDepth)
  )
    throw Error(`${lo.name}: unsupported incremental decode machine`);
  const frames = machine.frames.reachableTypes
    .map((name) => logicalDotnetFrame(types.get(name)))
    .join("\n");
  const logical = `    internal enum LogicalDecodeProgress { NeedMore, Complete }
    internal enum LogicalFailureKind { Truncated, Trailing, Capacity, Limit, Protocol }
    internal sealed class LogicalDecodeException(LogicalFailureKind kind,long offset,Exception inner):IOException($"{kind.ToString().ToLowerInvariant()} at byte {offset}",inner){internal LogicalFailureKind Kind{get;}=kind;internal long Offset{get;}=offset;}
    private sealed class LogicalInternalException(LogicalFailureKind kind,string message):IOException(message){internal LogicalFailureKind Kind{get;}=kind;}
    internal sealed record LogicalDecodeStep<T>(LogicalDecodeProgress Progress,T? Value,int BufferedInputByteCount,long ConsumedByteCount,int ContinuationDepth);
    private abstract class LogicalFrame{internal readonly long End;internal long Start;internal int State,Filled;internal object? Child;internal byte[]? Scratch;protected LogicalFrame(long end){End=end;}protected object TakeChild(){var value=Child!;Child=null;return value;}internal abstract void Resume(LogicalMachine machine);}
    private sealed class LogicalMachine
    {
        internal readonly DecodeContext Context;private readonly Stack<LogicalFrame> stack=new();private byte[] chunk=Array.Empty<byte>();private int at,buffered;private bool finalChunk,waiting,awaitingFinal;private object? result;
        internal long Consumed{get;private set;}internal int Depth=>stack.Count;internal int Buffered=>buffered;
        internal LogicalMachine(DecodeContext context){Context=context;Push(new Logical${lb}Frame(long.MaxValue));}
        internal ulong UInt(byte[] bytes){ulong value=0;foreach(var item in bytes)value=(value<<8)|item;return value;}
        internal long Bound(long parentEnd,ulong length){if(Consumed>parentEnd||length>(ulong)(parentEnd-Consumed))throw Error("incremental bounded length");return Consumed+checked((long)length);}
        internal void Exact(long expected,string name){if(Consumed!=expected)throw Error(name+": bounded body remainder");}
        internal void Push(LogicalFrame frame){frame.Start=Consumed;stack.Push(frame);}
        internal void Complete(object value){stack.Pop();if(stack.Count==0)result=value;else stack.Peek().Child=value;}
        internal byte[]? Take(LogicalFrame frame,int count,bool scalar){if(!scalar||count<0||count>8)throw new InvalidOperationException("incremental scalar read");var remaining=count-frame.Filled;if(remaining<0||Consumed>frame.End||remaining>frame.End-Consumed)throw Error("bounded logical value");try{frame.Scratch??=GC.AllocateUninitializedArray<byte>(count);}catch(OutOfMemoryException){throw Capacity("incremental scalar capacity");}while(frame.Filled<count){var available=chunk.Length-at;if(available==0){if(finalChunk)throw new EndOfStreamException("truncated logical stream");waiting=true;buffered=frame.Filled;return null;}var copied=Math.Min(count-frame.Filled,available);chunk.AsSpan(at,copied).CopyTo(frame.Scratch.AsSpan(frame.Filled));at+=copied;frame.Filled+=copied;Consumed+=copied;}var value=frame.Scratch;frame.Scratch=null;frame.Filled=0;buffered=0;return value;}
        internal bool Fill(LogicalFrame frame,byte[] value){if(Consumed>frame.End||value.Length-frame.Filled>frame.End-Consumed)throw Error("bounded logical value");while(frame.Filled<value.Length){var available=chunk.Length-at;if(available==0){if(finalChunk)throw new EndOfStreamException("truncated logical stream");waiting=true;buffered=0;return false;}var copied=Math.Min(value.Length-frame.Filled,available);chunk.AsSpan(at,copied).CopyTo(value.AsSpan(frame.Filled));at+=copied;frame.Filled+=copied;Consumed+=copied;}buffered=0;return true;}
        internal int ReadByte(LogicalFrame frame,int retained){if(Consumed>=frame.End)throw Error("bounded logical value");if(at==chunk.Length){if(finalChunk)throw new EndOfStreamException("truncated logical stream");waiting=true;buffered=Math.Min(retained,7);return -1;}Consumed++;return chunk[at++];}
        internal object? Run(byte[] input,bool finalInput){chunk=input;at=0;finalChunk=finalInput;waiting=false;buffered=0;try{if(awaitingFinal){if(input.Length!=0)throw new LogicalInternalException(LogicalFailureKind.Trailing,"logical stream trailing bytes");if(finalInput)return result;waiting=true;return null;}while(!waiting&&result is null)stack.Peek().Resume(this);if(result is not null){if(at!=input.Length)throw new LogicalInternalException(LogicalFailureKind.Trailing,"logical stream trailing bytes");if(finalInput)return result;awaitingFinal=true;waiting=true;return null;}if(at!=input.Length)throw new InvalidOperationException("logical decoder did not consume chunk");if(finalInput)throw new EndOfStreamException("truncated logical stream");return null;}finally{chunk=Array.Empty<byte>();}}
    }
${frames}
    internal sealed class Logical${ln}Decoder
    {
        internal const int MaximumContinuationDepth=${machine.frames.maximumDepth};private readonly LogicalMachine machine;private bool terminal;private long totalInputByteCount;
        internal Logical${ln}Decoder(DecodeContext context){machine=new(context);}
        internal void SetSyntheticPriorInputByteCountForTest(long value){if(totalInputByteCount!=0)throw new InvalidOperationException();totalInputByteCount=value;}
        internal LogicalDecodeStep<${lb}> Push(byte[] chunk,bool finalChunk){if(terminal)throw new InvalidOperationException("terminal logical decoder push");if(chunk.LongLength>${ll.maximumEncodedBytes}L-totalInputByteCount){terminal=true;throw new LogicalDecodeException(LogicalFailureKind.Limit,machine.Consumed,Error("${lo.name}: encoded limit"));}totalInputByteCount+=chunk.LongLength;try{var value=machine.Run(chunk,finalChunk);if(value is null)return new(LogicalDecodeProgress.NeedMore,null,machine.Buffered,machine.Consumed,machine.Depth);terminal=true;return new(LogicalDecodeProgress.Complete,(${lb})value,0,machine.Consumed,0);}catch(Exception failure)when(failure is not LogicalDecodeException){terminal=true;var kind=failure is LogicalInternalException internalFailure?internalFailure.Kind:failure is CapacityException?LogicalFailureKind.Capacity:failure is EndOfStreamException?LogicalFailureKind.Truncated:LogicalFailureKind.Protocol;throw new LogicalDecodeException(kind,machine.Consumed,failure);}}
    }
    internal static ${lb} DecodeLogical${ln}(byte[] b,DecodeContext c){var step=new Logical${ln}Decoder(c).Push(b,true);if(step.Progress!=LogicalDecodeProgress.Complete)throw new EndOfStreamException("truncated logical stream");return step.Value!;}
    internal static byte[] EncodeLogical${ln}(${lb} v,DecodeContext c){var b=Encode${lb}(v,c);if(b.LongLength>${ll.maximumEncodedBytes}L)throw Error("limit");return b;}`;
  const source = `// <auto-generated> DO NOT EDIT.\n#nullable enable\nusing System;\nusing System.Buffers.Binary;\nusing System.Collections.Generic;\nusing System.IO;\nusing System.Linq;\nusing System.Text;\nnamespace Systems.Zlink.Framework.Runtime.Protocol;\ninternal static class ServiceWireCodec\n{\n${[
    ctx,
    ...ir.types.map(declaration),
    ...ir.commands.map(commandDecl),
  ]
    .map((x) =>
      x
        .split("\n")
        .map((y) => `    ${y}`)
        .join("\n"),
    )
    .join(
      "\n\n",
    )}\n${runtime}\n${ir.types.map(typeMethods).join("\n")}${ir.commands.map(commandMethods).join("\n")}${ir.durableFormats.map(durable).join("\n")}${logical}\n}\n`;
  const missing = ir.operationVocabulary.filter((x) => !seen.has(x));
  if (missing.length)
    throw Error(`unimplemented operations: ${missing.join(",")}`);
  return source;
}
process.nextTick(() => {
  try {
    const source = render();
    if (mode === "--write") {
      fs.mkdirSync(path.dirname(output), { recursive: true });
      fs.writeFileSync(output, source);
    } else if (
      !fs.existsSync(output) ||
      fs.readFileSync(output, "utf8") !== source
    ) {
      console.error(`stale output: ${output}`);
      process.exitCode = 1;
    }
  } catch (error) {
    console.error(error.stack ?? String(error));
    process.exitCode = 1;
  }
});
function typeMethods(t) {
  const o = main(t),
    n = id(t.name),
    ext = external({ $ref: t.name }),
    params = ext
      .map((d) => `, ${typeOf(d.type)} enclosing${id(d.source.name)}`)
      .join(""),
    args = ext.map((d) => `, enclosing${id(d.source.name)}`).join(""),
    c = constraints(t),
    pred = predicates(t, "value");
  let read = [],
    write = [];
  if (o.op === "integer") {
    read = [
      `var raw = reader.${o.encoding.toUpperCase()}();`,
      `if (raw < ${lit(o.minimum, o.encoding)} || raw > ${lit(o.maximum, o.encoding)}) throw Error("${t.name}: range");`,
      `return new(raw);`,
    ];
    write = [
      `if (value.Value < ${lit(o.minimum, o.encoding)} || value.Value > ${lit(o.maximum, o.encoding)}) throw Error("${t.name}: range");`,
      `writer.${o.encoding.toUpperCase()}(value.Value);`,
    ];
  } else if (o.op === "enum") {
    const vals = o.values
      .map((v) => `${lit(v.value, o.encoding)} => ${n}.${id(v.name)}`)
      .join(", ");
    read = [
      `var raw = reader.${o.encoding.toUpperCase()}();`,
      `return raw switch { ${vals}, _ => throw Error("${t.name}: unknown enum") };`,
    ];
    write = [
      `if (!Enum.IsDefined(value)) throw Error("${t.name}: unknown enum");`,
      `writer.${o.encoding.toUpperCase()}((${intType(o.encoding)})value);`,
    ];
  } else if (o.op === "length-prefixed") {
    const text = t.operations.find((x) => x.op === "text-validation");
    if (text) seen.add(text.op);
    const lr = o.lengthType,
      capacity = o.encodeCapacity;
    read = [
      `var encodedLength = (ulong)Read${rn(lr)}(reader, context).Value;`,
      `if (encodedLength > ${lit(capacity.representationCeiling)}) throw Capacity("${t.name}: capacity");`,
      `if (encodedLength < ${lit(o.minimumBytes)} || encodedLength > ${lit(capacity.declaredMaximumBytes)}) throw Error("${t.name}: length");`,
      `var length = checked((int)encodedLength);`,
      `var bytes = reader.Bytes(length);`,
      o.zeroLengthMeaning ? `if (length == 0) return new(null);` : "",
      o.content === "text"
        ? `if (bytes.Contains((byte)0)) throw Error("${t.name}: NUL");\n        return new(Utf8.GetString(bytes));`
        : `return new(bytes);`,
    ].filter(Boolean);
    write = [
      `var bytes = ${o.content === "text" ? "value.Value is null ? Array.Empty<byte>() : Utf8.GetBytes(value.Value)" : "value.Value ?? Array.Empty<byte>()"};`,
      o.content === "text"
        ? `if (bytes.Contains((byte)0)) throw Error("${t.name}: NUL");`
        : "",
      `if ((ulong)bytes.LongLength > ${lit(capacity.representationCeiling)}) throw Capacity("${t.name}: capacity");`,
      `if ((ulong)bytes.LongLength < ${lit(o.minimumBytes)} || (ulong)bytes.LongLength > ${lit(capacity.declaredMaximumBytes)}) throw Error("${t.name}: length");`,
      `Write${rn(lr)}(writer, new ${rn(lr)}(checked((${intType(main(types.get(lr.$ref)).encoding)})bytes.Length)), context);`,
      `writer.Bytes(bytes);`,
    ].filter(Boolean);
  } else if (o.op === "struct" || o.op === "versioned-length-delimited") {
    const target = o.op === "versioned-length-delimited" ? "body" : "reader";
    if (o.reader) {
      seen.add(o.reader.op);
      read.push(
        `var version = ${readCall(o.version, "reader", new Map())};`,
        `if (version != ${expected(o.version, o.version.constant)}) throw Error("${t.name}: version");`,
        `var body = reader.Slice(checked((int)${readCall(o.length, "reader", new Map())}.Value));`,
      );
    }
    const rf = readFields(o.fields, target);
    read.push(
      ...rf.lines,
      `var value = new ${n}(${rf.args});`,
      ...c.lines,
      ...pred,
      ...(o.reader ? [`body.End("${t.name}");`] : []),
      "return value;",
    );
    const wf = writeFields(o.fields, "body", "value");
    write.push(...c.lines, ...pred);
    if (o.op === "versioned-length-delimited") {
      write.push(
        "var body = new Writer();",
        ...wf.lines,
        `${writeCall(o.version, "writer", expected(o.version, o.version.constant), new Map())};`,
        `Write${rn(o.length)}(writer, new ${rn(o.length)}(checked((${intType(main(types.get(o.length.$ref)).encoding)})body.Length)), context);`,
        `writer.Bytes(body.ToArray());`,
      );
    } else write.push(...writeFields(o.fields, "writer", "value").lines);
  } else if (o.op === "vector" || o.op === "versioned-vector") {
    let item, count, prop;
    if (o.op === "vector") {
      item = o.item;
      count = o.countType;
      prop = "Items";
    } else {
      const v = o.layout[0],
        ct = o.layout[1],
        r = o.layout.find((x) => x.kind === "repeat");
      item = r.item;
      count = { $ref: ct.$ref };
      prop = id(r.name);
      read.push(
        `var version = ${readCall(v, "reader", new Map())};`,
        `if (version != ${expected(v, v.constant)}) throw Error("${t.name}: version");`,
      );
    }
    read.push(
      `var count = checked((int)${readCall(count, "reader", new Map())}.Value);`,
      ...(o.maximumItems !== undefined
        ? [`if (count > ${o.maximumItems}) throw Error("${t.name}: count");`]
        : []),
      `var items = new ${typeOf(item)}[count];`,
      `for (var i = 0; i < count; ++i) items[i] = ${readCall(item, "reader", new Map())};`,
      `var value = new ${n}(items);`,
      ...c.lines,
      "return value;",
    );
    write.push(
      ...c.lines,
      ...(o.maximumItems !== undefined
        ? [
            `if (value.${prop}.Count > ${o.maximumItems}) throw Error("${t.name}: count");`,
          ]
        : []),
    );
    if (o.op === "versioned-vector") {
      const v = o.layout[0];
      write.push(
        `${writeCall(v, "writer", expected(v, v.constant), new Map())};`,
      );
    }
    write.push(
      `Write${rn(count)}(writer, new ${rn(count)}(checked((${intType(main(types.get(count.$ref)).encoding)})value.${prop}.Count)), context);`,
      `foreach (var item in value.${prop}) ${writeCall(item, "writer", "item", new Map())};`,
    );
  } else if (o.op === "conditional-union") {
    const disc = o.discriminators,
      caseHelpers = [];
    for (const d of disc) {
      seen.add(d.op);
      if (d.source.kind === "wire")
        read.push(
          `var ${id(d.name)} = ${readCall(d.type, "reader", new Map())};`,
        );
      else if (d.source.kind === "context")
        read.push(
          `var ${id(d.name)} = context.${id(d.source.name)} ?? throw Error("missing context ${d.source.name}");`,
        );
      else read.push(`var ${id(d.name)} = enclosing${id(d.source.name)};`);
    }
    if (o.reader) {
      seen.add(o.reader.op);
      read.push(
        `var selected = reader.Slice(checked((int)${readCall(o.bodyLengthType, "reader", new Map())}.Value));`,
      );
    } else read.push("var selected = reader;");
    cases(o).forEach(([sig, v], i) => {
      const s = JSON.parse(sig),
        test = disc
          .map((d) => `${id(d.name)} == ${expected(d.type, s[d.name])}`)
          .join(" && "),
        rf = readFields(caseFields(v), "selected"),
        cc = constraints(t, "caseValue", v.operations, sig),
        cp = predicates(t, "caseValue", v.operations),
        args = [
          ...disc
            .filter((d) => d.source.kind === "wire")
            .map((d) => id(d.name)),
          rf.args,
        ]
          .filter(Boolean)
          .join(", ");
      caseHelpers.push(...cc.helpers);
      read.push(
        `${i ? "else " : ""}if (${test})`,
        "{",
        ...rf.lines.map((x) => `    ${x}`),
        ...(o.reader ? [`    selected.End("${t.name}");`] : []),
        `    var caseValue = new ${n}Case${i}(${args});`,
        ...cc.lines.map((x) => `    ${x}`),
        ...cp.map((x) => `    ${x}`),
        `    ${n} value = caseValue;`,
        ...pred.map((x) => `    ${x}`),
        "    return value;",
        "}",
      );
    });
    if (o.otherwise.kind === "fields") {
      const rf = readFields(o.otherwise.fields, "selected");
      read.push(
        ...rf.lines,
        ...(o.reader ? [`selected.End("${t.name}");`] : []),
        `return new ${n}Otherwise(${rf.args});`,
      );
    } else read.push(`throw Error("${t.name}: discriminator");`);
    write.push("switch (value)", "{");
    cases(o).forEach(([sig, v], i) => {
      const s = JSON.parse(sig),
        cc = constraints(t, "item", v.operations, sig),
        cp = predicates(t, "item", v.operations);
      write.push(
        `case ${n}Case${i} item:`,
        "{",
        ...cc.lines.map((x) => `    ${x}`),
        ...cp.map((x) => `    ${x}`),
      );
      for (const d of disc) {
        if (d.source.kind === "wire") {
          write.push(
            `    if (item.${id(d.name)} != ${expected(d.type, s[d.name])}) throw Error("${t.name}: discriminator");`,
            `    ${writeCall(d.type, "writer", `item.${id(d.name)}`, new Map())};`,
          );
        } else {
          const actual =
            d.source.kind === "context"
              ? `(context.${id(d.source.name)} ?? throw Error("missing context ${d.source.name}"))`
              : `enclosing${id(d.source.name)}`;
          write.push(
            `    if (${actual} != ${expected(d.type, s[d.name])}) throw Error("${t.name}: discriminator");`,
          );
        }
      }
      const wf = writeFields(
        caseFields(v),
        o.reader ? "body" : "writer",
        "item",
      );
      if (o.reader)
        write.push(
          "    var body = new Writer();",
          ...wf.lines.map((x) => `    ${x}`),
          `    Write${rn(o.bodyLengthType)}(writer, new ${rn(o.bodyLengthType)}(checked((${intType(main(types.get(o.bodyLengthType.$ref)).encoding)})body.Length)), context);`,
          "    writer.Bytes(body.ToArray());",
        );
      else write.push(...wf.lines.map((x) => `    ${x}`));
      write.push("    break;", "}");
    });
    if (o.otherwise.kind === "fields") {
      if (disc.some((d) => d.source.kind === "wire"))
        throw Error(`${t.name}: wire discriminator otherwise lacks a value`);
      write.push(`case ${n}Otherwise item:`, "{");
      const actuals = disc.map((d) =>
        d.source.kind === "context"
          ? `(context.${id(d.source.name)} ?? throw Error("missing context ${d.source.name}"))`
          : `enclosing${id(d.source.name)}`,
      );
      for (const [sig] of cases(o)) {
        const s = JSON.parse(sig);
        write.push(
          `    if (${disc.map((d, i) => `${actuals[i]} == ${expected(d.type, s[d.name])}`).join(" && ")}) throw Error("${t.name}: otherwise matches case");`,
        );
      }
      const wf = writeFields(
        o.otherwise.fields,
        o.reader ? "body" : "writer",
        "item",
      );
      if (o.reader)
        write.push(
          "    var body = new Writer();",
          ...wf.lines.map((x) => `    ${x}`),
          `    Write${rn(o.bodyLengthType)}(writer, new ${rn(o.bodyLengthType)}(checked((${intType(main(types.get(o.bodyLengthType.$ref)).encoding)})body.Length)), context);`,
          "    writer.Bytes(body.ToArray());",
        );
      else write.push(...wf.lines.map((x) => `    ${x}`));
      write.push("    break;", "}");
    }
    write.push(`default: throw Error("${t.name}: variant");`, "}", ...pred);
    c.helpers.push(...caseHelpers);
  } else if (o.op === "tlv32") {
    seen.add(o.reader.op);
    if (
      o.encodingOrder !== "ascending-field-id" ||
      o.duplicateField !== "protocol-error" ||
      o.unknownField !== "skip-after-length-validation"
    )
      throw Error(`${t.name}: unsupported TLV policy`);
    const ordered = [...o.fields].sort((left, right) => left.id - right.id),
      required = new Set(o.requiredFields);
    const readEnvironment = new Map(
      o.fields.flatMap((f) => [
        [f.name, id(f.name)],
        [`${f.name}:field`, f],
      ]),
    );
    read.push(
      `var body = reader.Slice(checked((int)${readCall(o.totalLengthType, "reader", new Map())}.Value));`,
      ...o.fields.map((f) => `${typeOf(f.type, true)} ${id(f.name)} = null;`),
      "var previous = -1;",
      "while (body.Remaining > 0)",
      "{",
      `    var fieldId = checked((int)${readCall(o.fieldIdType, "body", new Map())}.Value);`,
      `    if (fieldId <= previous) throw Error("${t.name}: TLV order");`,
      "    previous = fieldId;",
      `    var item = body.Slice(checked((int)${readCall(o.fieldLengthType, "body", new Map())}.Value));`,
      "    switch (fieldId)",
      "    {",
    );
    for (const f of ordered)
      read.push(
        `    case ${f.id}:`,
        `        ${id(f.name)} = ${readCall(f.type, "item", new Map())};`,
        ...fieldChecks(f, `${id(f.name)}!`).map((line) => `        ${line}`),
        `        item.End("${f.name}");`,
        "        break;",
      );
    read.push(
      "    default:",
      "        item.Bytes(item.Remaining);",
      "        break;",
      "    }",
      "}",
    );
    for (const name of o.requiredFields)
      read.push(`if (${id(name)} is null) throw Error("${name}: required");`);
    read.push(
      ...tlvPresence(o, readEnvironment),
      `var value = new ${n}(${o.fields.map((f) => `${id(f.name)}${required.has(f.name) ? "!" : ""}`).join(", ")});`,
      "return value;",
    );
    const writeEnvironment = new Map(
      o.fields.flatMap((f) => [
        [f.name, `value.${id(f.name)}`],
        [`${f.name}:field`, f],
      ]),
    );
    write.push(...tlvPresence(o, writeEnvironment), "var body = new Writer();");
    for (const f of ordered) {
      const x = `value.${id(f.name)}`,
        v = main(types.get(f.type.$ref)).op === "enum" ? `${x}.Value` : `${x}!`;
      if (required.has(f.name))
        write.push(`if (${x} is null) throw Error("${f.name}: required");`);
      write.push(
        `if (${x} is not null)`,
        "{",
        ...fieldChecks(f, v).map((line) => `    ${line}`),
        "    var item = new Writer();",
        `    ${writeCall(f.type, "item", v, new Map())};`,
        `    Write${rn(o.fieldIdType)}(body, new ${rn(o.fieldIdType)}(${lit(f.id, main(types.get(o.fieldIdType.$ref)).encoding)}), context);`,
        `    Write${rn(o.fieldLengthType)}(body, new ${rn(o.fieldLengthType)}(checked((${intType(main(types.get(o.fieldLengthType.$ref)).encoding)})item.Length)), context);`,
        "    body.Bytes(item.ToArray());",
        "}",
      );
    }
    write.push(
      `Write${rn(o.totalLengthType)}(writer, new ${rn(o.totalLengthType)}(checked((${intType(main(types.get(o.totalLengthType.$ref)).encoding)})body.Length)), context);`,
      "writer.Bytes(body.ToArray());",
    );
  } else throw Error(`${t.name}: methods ${o.op}`);
  const encodedLimit = t.operations.find(
    (operation) => operation.op === "encoded-limit",
  );
  if (encodedLimit) seen.add(encodedLimit.op);
  const negotiated = t.operations.find(
    (operation) => operation.op === "negotiated-bound",
  );
  if (negotiated) seen.add(negotiated.op);
  const max = encodedLimit?.maximumEncodedBytes ?? o.maximumEncodedBytes;
  const limitCheck =
    max === undefined
      ? null
      : `if ((ulong)(encodedStart - reader.Remaining) > ${lit(max)}) throw Error("${t.name}: encoded limit");`;
  let readBody = read;
  if (negotiated) {
    const name = negotiated.context.name,
      maximum = `context.${id(name)} ?? throw Error("missing context ${name}")`,
      measured =
        negotiated.measured === "content-bytes"
          ? "bytes.LongLength"
          : "negotiatedStart - reader.Remaining",
      check = `if (negotiatedMaximum < 0 || negotiatedMaximum > ${lit(negotiated.context.absoluteMaximum, "i64")} || (ulong)(${measured}) > (ulong)negotiatedMaximum) throw Error("${t.name}: negotiated bound");`;
    readBody = [
      `var negotiatedMaximum = ${maximum};`,
      ...(negotiated.measured === "encoded-bytes"
        ? ["var negotiatedStart = reader.Remaining;"]
        : []),
      ...readBody.map((line) => line.replace(/return /g, `${check} return `)),
    ];
  }
  if (max !== undefined)
    readBody = [
      "var encodedStart = reader.Remaining;",
      ...readBody.map((line) =>
        line.replace(/return /g, `${limitCheck} return `),
      ),
    ];
  let writeBody = write;
  if (negotiated) {
    const name = negotiated.context.name,
      maximum = `context.${id(name)} ?? throw Error("missing context ${name}")`,
      measured =
        negotiated.measured === "content-bytes"
          ? "bytes.LongLength"
          : "writer.Length - negotiatedStart",
      check = `if (negotiatedMaximum < 0 || negotiatedMaximum > ${lit(negotiated.context.absoluteMaximum, "i64")} || (ulong)(${measured}) > (ulong)negotiatedMaximum) throw Error("${t.name}: negotiated bound");`;
    writeBody = [
      `var negotiatedMaximum = ${maximum};`,
      ...(negotiated.measured === "encoded-bytes"
        ? ["var negotiatedStart = writer.Length;"]
        : []),
      ...writeBody,
      check,
    ];
  }
  if (max !== undefined)
    writeBody = [
      "var encodedStart = writer.Length;",
      ...writeBody,
      `if ((ulong)(writer.Length - encodedStart) > ${lit(max)}) throw Error("${t.name}: encoded limit");`,
    ];
  return `    internal static ${n} Decode${n}(byte[] bytes, DecodeContext context${params}) { var reader = new Reader(bytes); var value = Read${n}(reader, context${args}); reader.End("${t.name}"); return value; }\n    internal static byte[] Encode${n}(${n} value, DecodeContext context${params}) { var writer = new Writer(); Write${n}(writer, value, context${args}); return writer.ToArray(); }\n    private static ${n} Read${n}(Reader reader, DecodeContext context${params})\n    {\n        ${readBody.join("\n        ")}\n    }\n    private static void Write${n}(Writer writer, ${n} value, DecodeContext context${params})\n    {\n        ${writeBody.join("\n        ")}\n    }\n${c.helpers.join("\n")}`;
}
