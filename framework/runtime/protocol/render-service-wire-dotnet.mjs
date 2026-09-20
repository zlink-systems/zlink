#!/usr/bin/env node

import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";
import { lowerSchema } from "./service-wire-lowering.mjs";

const directory = path.dirname(fileURLToPath(import.meta.url));
const output = path.join(directory, "generated", "dotnet", "ServiceWireCodec.g.cs");

function identifier(value) {
  const result = value.replace(/(^|[^A-Za-z0-9]+)([A-Za-z0-9])/g,
    (_, _separator, letter) => letter.toUpperCase());
  return /^[0-9]/.test(result) ? `N${result}` : result;
}

function integerType(encoding) {
  return { u8: "byte", u16: "ushort", u32: "uint", u64: "ulong", i64: "long" }[encoding];
}

function enumDeclaration(type) {
  const values = type.values.map(({ name, value }) => `    ${identifier(name)} = ${value}`).join(",\n");
  return `internal enum ${identifier(type.name)} : ${integerType(type.encoding)}\n{\n${values}\n}`;
}

function typeReference(reference, optional = false) {
  return `${identifier(reference.$ref)}${optional ? "?" : ""}`;
}

function fieldsDeclaration(fields) {
  return fields.map((field) => `${typeReference(field, Boolean(field.when))} ${identifier(field.name)}`).join(", ");
}

function unionDeclaration(type) {
  const name = identifier(type.name);
  const cases = Object.values(type.cases).map((selected, index) =>
    `internal sealed record ${name}Case${index}(${fieldsDeclaration(selected.fields)}) : ${name};`).join("\n");
  const otherwise = type.otherwise.kind === "fields"
    ? `\ninternal sealed record ${name}Otherwise(${fieldsDeclaration(type.otherwise.fields)}) : ${name};`
    : "";
  return `internal abstract record ${name};\n${cases}${otherwise}`;
}

function typeDeclaration(type) {
  if (type.kind === "enum") return enumDeclaration(type);
  if (type.kind === "conditional-union") return unionDeclaration(type);
  const name = identifier(type.name);
  if (type.kind === "integer") return `internal sealed record ${name}(${integerType(type.encoding)} Value);`;
  if (type.kind === "length-prefixed-bytes") return `internal sealed record ${name}(byte[]${type.zeroLengthMeaning === "absent" ? "?" : ""} Value);`;
  if (type.kind === "length-prefixed-text") return `internal sealed record ${name}(string${type.zeroLengthMeaning === "absent" ? "?" : ""} Value);`;
  if (type.kind === "struct") return `internal sealed record ${name}(${fieldsDeclaration(type.fields)});`;
  if (type.kind === "vector") return `internal sealed record ${name}(IReadOnlyList<${typeReference(type.item)}> Items);`;
  if (type.kind === "versioned-vector") {
    const repeat = type.layout.find((entry) => entry.kind === "repeat");
    return `internal sealed record ${name}(IReadOnlyList<${typeReference(repeat.item)}> ${identifier(repeat.name)});`;
  }
  if (type.kind === "versioned-length-delimited") return `internal sealed record ${name}(${fieldsDeclaration(type.body)});`;
  if (type.kind === "tlv32") return `internal sealed record ${name}(${type.fields.map((field) => `${typeReference(field, !field.required)} ${identifier(field.name)}`).join(", ")});`;
  throw new Error(`unsupported .NET type kind ${type.kind}`);
}

function toWire(reference, expression, optional = false) {
  const call = `ToWire${identifier(reference.$ref)}(${expression}${optional ? "!" : ""})`;
  return optional ? `${expression} is null ? null : ${call}` : call;
}

function fromWire(reference, expression, optional = false) {
  const call = `FromWire${identifier(reference.$ref)}(${expression})`;
  return optional ? `${expression} is null ? null : ${call}` : call;
}

function dictionaryExpression(fields, valueName) {
  if (fields.length === 0) return "new Dictionary<string, object?>()";
  return `new Dictionary<string, object?> { ${fields.map((field) =>
    `["${field.name}"] = ${toWire(field, `${valueName}.${identifier(field.name)}`, Boolean(field.when))}`).join(", ")} }`;
}

function constructFields(fields, dictionaryName) {
  return fields.map((field) => fromWire(
    field,
    `${dictionaryName}["${field.name}"]`,
    Boolean(field.when),
  )).join(", ");
}

function projectionMethods(type) {
  const name = identifier(type.name);
  if (type.kind === "integer") {
    const convert = { u8: "ToByte", u16: "ToUInt16", u32: "ToUInt32", u64: "ToUInt64", i64: "ToInt64" }[type.encoding];
    return `    private static object ToWire${name}(${name} value) => value.Value;\n`
      + `    private static ${name} FromWire${name}(object? value) => new(Convert.${convert}(value));`;
  }
  if (type.kind === "enum") {
    return `    private static object ToWire${name}(${name} value) => value.ToString();\n`
      + `    private static ${name} FromWire${name}(object? value) => Enum.Parse<${name}>(identifier((string)value!), true);`;
  }
  if (type.kind === "length-prefixed-bytes") return `    private static object? ToWire${name}(${name} value) => value.Value;\n    private static ${name} FromWire${name}(object? value) => new((byte[]?)value${type.zeroLengthMeaning === "absent" ? "" : "!"});`;
  if (type.kind === "length-prefixed-text") return `    private static object? ToWire${name}(${name} value) => value.Value;\n    private static ${name} FromWire${name}(object? value) => new((string?)value${type.zeroLengthMeaning === "absent" ? "" : "!"});`;
  if (type.kind === "struct" || type.kind === "versioned-length-delimited") {
    const fields = type.kind === "struct" ? type.fields : type.body;
    return `    private static object ToWire${name}(${name} value) => ${dictionaryExpression(fields, "value")};\n`
      + `    private static ${name} FromWire${name}(object? value)\n    {\n        var fields = Fields(value);\n        return new(${constructFields(fields, "fields")});\n    }`;
  }
  if (type.kind === "vector") {
    return `    private static object ToWire${name}(${name} value) => value.Items.Select(item => ${toWire(type.item, "item")}).ToArray();\n`
      + `    private static ${name} FromWire${name}(object? value) => new(Items(value).Select(item => ${fromWire(type.item, "item")}).ToArray());`;
  }
  if (type.kind === "versioned-vector") {
    const repeat = type.layout.find((entry) => entry.kind === "repeat");
    const property = identifier(repeat.name);
    return `    private static object ToWire${name}(${name} value) => value.${property}.Select(item => ${toWire(repeat.item, "item")}).ToArray();\n`
      + `    private static ${name} FromWire${name}(object? value) => new(Items(value).Select(item => ${fromWire(repeat.item, "item")}).ToArray());`;
  }
  if (type.kind === "conditional-union") {
    const entries = Object.entries(type.cases);
    const toArms = entries.map(([signature, selected], index) =>
      `            ${name}Case${index} item => new WireUnion(${index}, ${JSON.stringify(signature)}, ${dictionaryExpression(selected.fields, "item")}),`).join("\n");
    const fromArms = entries.map(([, selected], index) =>
      `            ${index} => new ${name}Case${index}(${constructFields(selected.fields, "union.Fields")}),`).join("\n");
    const otherwise = type.otherwise.kind === "fields"
      ? `\n            ${name}Otherwise item => new WireUnion(-1, "", ${dictionaryExpression(type.otherwise.fields, "item")}),`
      : "";
    const fromOtherwise = type.otherwise.kind === "fields"
      ? `\n            _ => new ${name}Otherwise(${constructFields(type.otherwise.fields, "union.Fields")}),`
      : `\n            _ => throw Error("unknown union case"),`;
    return `    private static object ToWire${name}(${name} value) => value switch\n        {\n${toArms}${otherwise}\n            _ => throw Error("unknown union variant")\n        };\n`
      + `    private static ${name} FromWire${name}(object? value)\n    {\n        var union = (WireUnion)value!;\n        return union.Ordinal switch\n        {\n${fromArms}${fromOtherwise}\n        };\n    }`;
  }
  if (type.kind === "tlv32") {
    const locals = type.fields.map((field) => `        fields.TryGetValue("${field.name}", out var ${identifier(field.name)}Value);`).join("\n");
    return `    private static object ToWire${name}(${name} value) => ${dictionaryExpression(type.fields.map((field) => ({ ...field, when: !field.required })), "value")};\n`
      + `    private static ${name} FromWire${name}(object? value)\n    {\n        var fields = Fields(value);\n${locals}\n        return new(${type.fields.map((field) => fromWire(field, `${identifier(field.name)}Value`, !field.required)).join(", ")});\n    }`;
  }
  throw new Error(`unsupported .NET projection kind ${type.kind}`);
}

function typeMethods(type) {
  const name = identifier(type.name);
  return `    internal static byte[] Encode${name}(${name} value) => EncodeNamed("${type.name}", ToWire${name}(value));\n`
    + `    internal static ${name} Decode${name}(byte[] bytes) => FromWire${name}(DecodeNamed("${type.name}", bytes));\n`
    + projectionMethods(type);
}

function commandFrames(command, ir) {
  const allowed = new Set(command.allowedFlags.map((flag) => flag.name));
  return ir.flags.filter((flag) => allowed.has(flag.name) && flag.frame !== undefined);
}

function commandDeclaration(command, ir) {
  const name = `${identifier(command.name)}${command.id}`;
  const members = ["byte Flags", ...command.body.map((field) => `${typeReference(field, Boolean(field.when))} ${identifier(field.name)}`)];
  members.push(...commandFrames(command, ir).map((flag) => `${typeReference(flag.frame, true)} ${identifier(flag.name)}`));
  if (command.payload.policy !== "forbidden") members.push(`${typeReference(command.payload.type, command.payload.policy === "optional")} Payload`);
  return `internal sealed record ${name}(${members.join(", ")});`;
}

function commandMethods(command, ir) {
  const name = `${identifier(command.name)}${command.id}`;
  const frameFlags = commandFrames(command, ir);
  const frames = frameFlags.length === 0 ? "new Dictionary<string, object?>()" : `new Dictionary<string, object?> { ${frameFlags.map((flag) => `["${flag.name}"] = ${toWire(flag.frame, `value.${identifier(flag.name)}`, true)}`).join(", ")} }`;
  const payloadToWire = command.payload.policy === "forbidden" ? "null" : toWire(command.payload.type, "value.Payload", command.payload.policy === "optional");
  const payloadFromWire = command.payload.policy === "forbidden" ? [] : [fromWire(command.payload.type, "value.Payload", command.payload.policy === "optional")];
  const frameLocals = frameFlags.map((flag) => `        value.Frames.TryGetValue("${flag.name}", out var ${identifier(flag.name)}Frame);`).join("\n");
  const fromFrames = frameFlags.map((flag) => fromWire(flag.frame, `${identifier(flag.name)}Frame`, true));
  const constructor = ["value.Flags", ...command.body.map((field) => fromWire(field, `value.Fields["${field.name}"]`, Boolean(field.when))), ...fromFrames, ...payloadFromWire].join(", ");
  return `    internal static ${name} Decode${name}(IReadOnlyList<byte[]> frames, DecodeContext? context = null) => FromWire${name}(DecodeCommand(${command.id}, frames, context));\n`
    + `    internal static ${name} Decode${name}(byte[] frame, DecodeContext? context = null) => FromWire${name}(DecodeCommand(${command.id}, new[] { frame }, context));\n`
    + `    internal static byte[][] Encode${name}(${name} value, DecodeContext? context = null) => EncodeCommand(${command.id}, ToWire${name}(value), context);\n`
    + `    private static WireCommand ToWire${name}(${name} value) => new(value.Flags, ${dictionaryExpression(command.body, "value")}, ${frames}, ${payloadToWire});\n`
    + `    private static ${name} FromWire${name}(WireCommand value)\n    {\n${frameLocals}${frameLocals ? "\n" : ""}        return new(${constructor});\n    }`;
}

function formatMethods(format) {
  const name = identifier(format.name);
  const body = identifier(format.body.$ref);
  return `    internal static ${body} DecodeDurable${name}(byte[] bytes) => FromWire${body}(DecodeDurable("${format.name}", bytes));\n`
    + `    internal static byte[] EncodeDurable${name}(${body} value) => EncodeDurable("${format.name}", ToWire${body}(value));`;
}

function render(ir) {
  const json = JSON.stringify(ir);
  const declarations = ir.types.map(typeDeclaration).join("\n\n");
  const commandDeclarations = ir.commands.map((command) => commandDeclaration(command, ir)).join("\n\n");
  const methods = ir.types.map(typeMethods).join("\n\n");
  const commands = ir.commands.map((command) => commandMethods(command, ir)).join("\n\n");
  const formats = ir.durableFormats.map(formatMethods).join("\n\n");
  const logicalName = identifier(ir.relocationLogicalStreamFormat.name);
  const logicalType = identifier(ir.relocationLogicalStreamFormat.body.$ref);

  return `// <auto-generated> DO NOT EDIT. Generated solely from the validated service-wire IR.\n`
    + `#nullable enable\nusing System;\nusing System.Buffers.Binary;\nusing System.Collections.Generic;\nusing System.IO;\nusing System.Linq;\nusing System.Text;\nusing System.Text.Json;\n\n`
    + `namespace Systems.Zlink.Framework.Runtime.Protocol;\n\n`
    + `internal static class ServiceWireCodec\n{\n`
    + `    internal sealed record WireUnion(int Ordinal, string Signature, IReadOnlyDictionary<string, object?> Fields);\n`
    + `    internal sealed record WireCommand(byte Flags, IReadOnlyDictionary<string, object?> Fields, IReadOnlyDictionary<string, object?> Frames, object? Payload);\n`
    + `    internal sealed class DecodeContext : Dictionary<string, object?>;\n\n`
    + `${declarations.split("\n").map(line => `    ${line}`).join("\n")}\n\n`
    + `${commandDeclarations.split("\n").map(line => `    ${line}`).join("\n")}\n\n`
    + `    private static readonly UTF8Encoding Utf8 = new(false, true);\n`
    + `    private static readonly JsonDocument Ir = JsonDocument.Parse("""${json}""");\n`
    + `    private static readonly Dictionary<string, JsonElement> Types = Ir.RootElement.GetProperty("types").EnumerateArray().ToDictionary(x => x.GetProperty("name").GetString()!, x => x);\n`
    + `    private static readonly Dictionary<int, JsonElement> Commands = Ir.RootElement.GetProperty("commands").EnumerateArray().ToDictionary(x => x.GetProperty("id").GetInt32(), x => x);\n`
    + `    private static readonly Dictionary<string, JsonElement> Formats = Ir.RootElement.GetProperty("durableFormats").EnumerateArray().ToDictionary(x => x.GetProperty("name").GetString()!, x => x);\n\n`
    + runtimeSource() + `\n\n${methods}\n\n${commands}\n\n${formats}\n\n`
    + `    internal static ${logicalType} DecodeLogical${logicalName}(byte[] bytes) { if ((ulong)bytes.LongLength > ${ir.relocationLogicalStreamFormat.maximumBytes}UL) throw Error("logical stream maximum"); return FromWire${logicalType}(DecodeNamed("${ir.relocationLogicalStreamFormat.body.$ref}", bytes)); }\n`
    + `    internal static byte[] EncodeLogical${logicalName}(${logicalType} value) { var bytes = EncodeNamed("${ir.relocationLogicalStreamFormat.body.$ref}", ToWire${logicalType}(value)); if ((ulong)bytes.LongLength > ${ir.relocationLogicalStreamFormat.maximumBytes}UL) throw Error("logical stream maximum"); return bytes; }\n}`;
}

function runtimeSource() {
  return String.raw`    private sealed class Reader(byte[] bytes)
    {
        private int position;
        internal int Remaining => bytes.Length - position;
        internal byte U8() { Need(1); return bytes[position++]; }
        internal ushort U16() { Need(2); var value = BinaryPrimitives.ReadUInt16BigEndian(bytes.AsSpan(position)); position += 2; return value; }
        internal uint U32() { Need(4); var value = BinaryPrimitives.ReadUInt32BigEndian(bytes.AsSpan(position)); position += 4; return value; }
        internal ulong U64() { Need(8); var value = BinaryPrimitives.ReadUInt64BigEndian(bytes.AsSpan(position)); position += 8; return value; }
        internal long I64() { Need(8); var value = BinaryPrimitives.ReadInt64BigEndian(bytes.AsSpan(position)); position += 8; return value; }
        internal byte[] Bytes(int count) { Need(count); var value = bytes.AsSpan(position, count).ToArray(); position += count; return value; }
        internal Reader Slice(int count) => new(Bytes(count));
        internal void End(string name) { if (Remaining != 0) throw Error($"{name}: trailing bytes"); }
        private void Need(int count) { if (count < 0 || Remaining < count) throw new EndOfStreamException("truncated field"); }
    }

    private sealed class Writer
    {
        private readonly MemoryStream stream = new();
        internal long Length => stream.Length;
        internal void U8(byte value) => stream.WriteByte(value);
        internal void U16(ushort value) { Span<byte> b = stackalloc byte[2]; BinaryPrimitives.WriteUInt16BigEndian(b, value); stream.Write(b); }
        internal void U32(uint value) { Span<byte> b = stackalloc byte[4]; BinaryPrimitives.WriteUInt32BigEndian(b, value); stream.Write(b); }
        internal void U64(ulong value) { Span<byte> b = stackalloc byte[8]; BinaryPrimitives.WriteUInt64BigEndian(b, value); stream.Write(b); }
        internal void I64(long value) { Span<byte> b = stackalloc byte[8]; BinaryPrimitives.WriteInt64BigEndian(b, value); stream.Write(b); }
        internal void Bytes(byte[] value) => stream.Write(value);
        internal byte[] ToArray() => stream.ToArray();
    }

    private sealed class Environment(DecodeContext? context, IReadOnlyDictionary<string, object?>? enclosing, byte flags)
    {
        internal DecodeContext Context { get; } = context ?? new();
        internal IReadOnlyDictionary<string, object?> Enclosing { get; } = enclosing ?? new Dictionary<string, object?>();
        internal byte Flags { get; } = flags;
    }

    private static InvalidDataException Error(string message) => new(message);
    private static JsonElement Ref(JsonElement value) => Types[value.GetProperty("$ref").GetString()!];
    private static ulong Unsigned(JsonElement value) => value.ValueKind == JsonValueKind.String ? ulong.Parse(value.GetString()!) : value.GetUInt64();
    private static long Signed(JsonElement value) => value.ValueKind == JsonValueKind.String ? long.Parse(value.GetString()!) : value.GetInt64();
    private static object? DecodeNamed(string name, byte[] bytes)
    {
        var reader = new Reader(bytes);
        var value = Read(Types[name], reader, new Environment(null, null, 0));
        reader.End(name);
        return value;
    }
    private static byte[] EncodeNamed(string name, object? value)
    {
        var writer = new Writer();
        Write(Types[name], value, writer, new Environment(null, null, 0));
        return writer.ToArray();
    }

    private static object? Read(JsonElement type, Reader reader, Environment environment)
    {
        var before = reader.Remaining;
        var kind = type.GetProperty("kind").GetString();
        var value = kind switch
        {
            "integer" => ReadInteger(type, reader),
            "enum" => ReadEnum(type, reader),
            "length-prefixed-bytes" => ReadBytes(type, reader),
            "length-prefixed-text" => ReadText(type, reader),
            "struct" => ReadFields(type.GetProperty("fields"), reader, environment, type),
            "vector" => ReadVector(type, reader, environment),
            "versioned-vector" => ReadVersionedVector(type, reader, environment),
            "versioned-length-delimited" => ReadDelimited(type, reader, environment),
            "conditional-union" => ReadUnion(type, reader, environment),
            "tlv32" => ReadTlv(type, reader, environment),
            _ => throw Error($"unknown layout kind {kind}")
        };
        ValidateEncodedSize(type, before - reader.Remaining);
        return value;
    }

    private static void Write(JsonElement type, object? value, Writer writer, Environment environment)
    {
        var before = writer.Length;
        switch (type.GetProperty("kind").GetString())
        {
            case "integer": WriteInteger(type, value!, writer); break;
            case "enum": WriteEnum(type, value!, writer); break;
            case "length-prefixed-bytes": WriteBytes(type, (byte[]?)value, writer); break;
            case "length-prefixed-text": WriteText(type, (string?)value, writer); break;
            case "struct": WriteFields(type.GetProperty("fields"), Fields(value), writer, environment, type); break;
            case "vector": WriteVector(type, Items(value), writer, environment); break;
            case "versioned-vector": WriteVersionedVector(type, Items(value), writer, environment); break;
            case "versioned-length-delimited": WriteDelimited(type, Fields(value), writer, environment); break;
            case "conditional-union": WriteUnion(type, (WireUnion)value!, writer, environment); break;
            case "tlv32": WriteTlv(type, Fields(value), writer, environment); break;
            default: throw Error("unknown layout kind");
        }
        ValidateEncodedSize(type, writer.Length - before);
    }
    private static void ValidateEncodedSize(JsonElement type, long length)
    {
        if (type.TryGetProperty("maximumEncodedBytes", out var maximum) && (ulong)length > Unsigned(maximum)) throw Error(type.GetProperty("name").GetString() + ": maximum encoded bytes");
    }

    private static object ReadInteger(JsonElement type, Reader reader)
    {
        var encoding = type.GetProperty("encoding").GetString();
        object value = encoding switch { "u8" => reader.U8(), "u16" => reader.U16(), "u32" => reader.U32(), "u64" => reader.U64(), "i64" => reader.I64(), _ => throw Error("integer encoding") };
        ValidateRange(type, value);
        return value;
    }
    private static void WriteInteger(JsonElement type, object value, Writer writer)
    {
        ValidateRange(type, value);
        switch (type.GetProperty("encoding").GetString())
        {
            case "u8": writer.U8(Convert.ToByte(value)); break;
            case "u16": writer.U16(Convert.ToUInt16(value)); break;
            case "u32": writer.U32(Convert.ToUInt32(value)); break;
            case "u64": writer.U64(Convert.ToUInt64(value)); break;
            case "i64": writer.I64(Convert.ToInt64(value)); break;
            default: throw Error("integer encoding");
        }
    }
    private static void ValidateRange(JsonElement type, object value)
    {
        if (type.GetProperty("encoding").GetString() == "i64")
        {
            var actual = Convert.ToInt64(value);
            if (type.TryGetProperty("minimum", out var min) && actual < Signed(min) || type.TryGetProperty("maximum", out var max) && actual > Signed(max)) throw Error("integer range");
        }
        else
        {
            var actual = Convert.ToUInt64(value);
            if (type.TryGetProperty("minimum", out var min) && actual < Unsigned(min) || type.TryGetProperty("maximum", out var max) && actual > Unsigned(max)) throw Error("integer range");
        }
    }
    private static object ReadEnum(JsonElement type, Reader reader)
    {
        var raw = ReadIntegerLike(type.GetProperty("encoding").GetString()!, reader);
        foreach (var value in type.GetProperty("values").EnumerateArray()) if (Convert.ToInt64(raw) == value.GetProperty("value").GetInt64()) return value.GetProperty("name").GetString()!;
        throw Error("unknown enum value");
    }
    private static void WriteEnum(JsonElement type, object value, Writer writer)
    {
        var name = value.ToString()!;
        foreach (var item in type.GetProperty("values").EnumerateArray()) if (string.Equals(identifier(item.GetProperty("name").GetString()!), name, StringComparison.OrdinalIgnoreCase) || item.GetProperty("name").GetString() == name) { WriteIntegerLike(type.GetProperty("encoding").GetString()!, item.GetProperty("value").GetInt64(), writer); return; }
        throw Error("unknown enum value");
    }
    private static string identifier(string value) => string.Concat(value.Split(new[] {'-', '_'}, StringSplitOptions.RemoveEmptyEntries).Select(x => char.ToUpperInvariant(x[0]) + x[1..]));
    private static object ReadIntegerLike(string encoding, Reader reader) => encoding switch { "u8" => reader.U8(), "u16" => reader.U16(), "u32" => reader.U32(), "u64" => reader.U64(), "i64" => reader.I64(), _ => throw Error("integer encoding") };
    private static void WriteIntegerLike(string encoding, long value, Writer writer) { switch (encoding) { case "u8": writer.U8(checked((byte)value)); break; case "u16": writer.U16(checked((ushort)value)); break; case "u32": writer.U32(checked((uint)value)); break; case "u64": writer.U64(checked((ulong)value)); break; case "i64": writer.I64(value); break; default: throw Error("integer encoding"); } }

    private static int ReadLength(JsonElement reference, Reader reader) => checked((int)Convert.ToUInt64(Read(Ref(reference), reader, new Environment(null, null, 0))));
    private static void WriteLength(JsonElement reference, int length, Writer writer) => Write(Ref(reference), ConvertLength(Ref(reference), length), writer, new Environment(null, null, 0));
    private static object ConvertLength(JsonElement type, int length) => type.GetProperty("encoding").GetString() switch { "u8" => checked((byte)length), "u16" => checked((ushort)length), "u32" => checked((uint)length), "u64" => checked((ulong)length), _ => throw Error("length type") };
    private static byte[]? ReadBytes(JsonElement type, Reader reader)
    {
        var length = ReadLength(type.GetProperty("lengthType"), reader);
        ValidateLength(type, length);
        if (length == 0 && type.TryGetProperty("zeroLengthMeaning", out _)) return null;
        return reader.Bytes(length);
    }
    private static void WriteBytes(JsonElement type, byte[]? value, Writer writer)
    {
        var bytes = value ?? Array.Empty<byte>(); ValidateLength(type, bytes.Length); WriteLength(type.GetProperty("lengthType"), bytes.Length, writer); writer.Bytes(bytes);
    }
    private static string? ReadText(JsonElement type, Reader reader)
    {
        var bytes = ReadBytes(type, reader); if (bytes is null) return null; if (type.TryGetProperty("nul", out _) && Array.IndexOf(bytes, (byte)0) >= 0) throw Error("NUL is forbidden"); return Utf8.GetString(bytes);
    }
    private static void WriteText(JsonElement type, string? value, Writer writer)
    {
        if (value is null) { WriteBytes(type, null, writer); return; } var bytes = Utf8.GetBytes(value); if (type.TryGetProperty("nul", out _) && Array.IndexOf(bytes, (byte)0) >= 0) throw Error("NUL is forbidden"); WriteBytes(type, bytes, writer);
    }
    private static void ValidateLength(JsonElement type, int length)
    {
        if (type.TryGetProperty("minimumBytes", out var min) && (ulong)length < Unsigned(min) || type.TryGetProperty("maximumBytes", out var max) && (ulong)length > Unsigned(max)) throw Error("length range");
    }

    private static Dictionary<string, object?> ReadFields(JsonElement definitions, Reader reader, Environment parent, JsonElement? owner = null)
    {
        var fields = new Dictionary<string, object?>(); var environment = new Environment(parent.Context, fields, parent.Flags);
        foreach (var field in definitions.EnumerateArray())
        {
            var name = field.GetProperty("name").GetString()!;
            if (!Present(field, environment)) { fields[name] = null; continue; }
            try { var value = Read(Ref(field), reader, environment); ValidateField(field, value!); fields[name] = value; }
            catch (Exception error) when (error is InvalidDataException or EndOfStreamException or OverflowException) { throw Error($"{name}: {error.Message}"); }
        }
        if (owner.HasValue) ValidateLayout(owner.Value, fields);
        return fields;
    }
    private static void WriteFields(JsonElement definitions, IReadOnlyDictionary<string, object?> fields, Writer writer, Environment parent, JsonElement? owner = null)
    {
        var environment = new Environment(parent.Context, fields, parent.Flags);
        foreach (var field in definitions.EnumerateArray())
        {
            var name = field.GetProperty("name").GetString()!; fields.TryGetValue(name, out var value);
            if (!Present(field, environment)) { if (value is not null && field.TryGetProperty("whenFalse", out _)) throw Error($"{name}: forbidden value"); continue; }
            if (value is null && !AllowsNull(field)) throw Error($"{name}: required value"); ValidateField(field, value); Write(Ref(field), value, writer, environment);
        }
        if (owner.HasValue) ValidateLayout(owner.Value, fields);
    }
    private static bool AllowsNull(JsonElement reference) => Ref(reference).TryGetProperty("zeroLengthMeaning", out var meaning) && meaning.GetString() == "absent";
    private static bool Present(JsonElement field, Environment environment)
    {
        if (!field.TryGetProperty("when", out var when)) return true;
        return Condition(when, environment.Enclosing, environment);
    }
    private static bool Condition(JsonElement when, IReadOnlyDictionary<string, object?> values, Environment environment)
    {
        foreach (var atom in when.GetProperty("all").EnumerateArray())
        {
            var kind = atom.GetProperty("kind").GetString();
            if (kind == "fieldPresent" && (!values.TryGetValue(atom.GetProperty("operand").GetProperty("name").GetString()!, out var present) || present is null)) return false;
            if (kind == "fieldEquals" && (!values.TryGetValue(atom.GetProperty("operand").GetProperty("name").GetString()!, out var actual) || !Matches(actual, atom.GetProperty("value")))) return false;
            if (kind == "contextEquals" && (!environment.Context.TryGetValue(atom.GetProperty("operand").GetProperty("name").GetString()!, out var context) || !Matches(context, atom.GetProperty("value")))) return false;
            if (kind is "allFlagsSet" or "anyFlagsSet")
            {
                var bits = atom.GetProperty("operands").EnumerateArray().Select(x => FlagBit(x.GetProperty("name").GetString()!)).ToArray();
                if (kind == "allFlagsSet" && bits.Any(bit => (environment.Flags & bit) == 0) || kind == "anyFlagsSet" && bits.All(bit => (environment.Flags & bit) == 0)) return false;
            }
        }
        return true;
    }
    private static bool Matches(object? actual, JsonElement expected) => actual?.ToString()?.Equals(expected.ToString().Trim('"'), StringComparison.OrdinalIgnoreCase) == true;
    private static byte FlagBit(string name) => checked((byte)Ir.RootElement.GetProperty("flags").EnumerateArray().Single(x => x.GetProperty("name").GetString() == name).GetProperty("bit").GetInt32());
    private static void ValidateField(JsonElement field, object? value)
    {
        if (field.TryGetProperty("constant", out var constant) && !Matches(value, constant)) throw Error("constant mismatch");
        if (field.TryGetProperty("minimum", out var min) && Convert.ToDecimal(value) < decimal.Parse(min.ToString().Trim('"')) || field.TryGetProperty("maximum", out var max) && Convert.ToDecimal(value) > decimal.Parse(max.ToString().Trim('"'))) throw Error("field range");
        if (field.TryGetProperty("constraints", out var constraints)) foreach (var constraint in constraints.EnumerateArray()) if (constraint.GetProperty("kind").GetString() == "contains-protocol-required-capability" && !Items(value).Any(x => x?.ToString() == Ir.RootElement.GetProperty("protocol").GetProperty("requiredCapability").GetString())) throw Error("required capability");
    }
    private static void ValidateLayout(JsonElement owner, IReadOnlyDictionary<string, object?> fields)
    {
        if (!owner.TryGetProperty("constraints", out var constraints)) return;
        foreach (var constraint in constraints.EnumerateArray())
        {
            var kind = constraint.GetProperty("kind").GetString();
            if (kind == "not-both-zero" && !constraint.TryGetProperty("unless", out _) && constraint.GetProperty("fields").EnumerateArray().All(x => IsZero(fields[x.GetProperty("name").GetString()!]))) throw Error("not-both-zero");
            if (kind == "field-less-than-or-equal" && Convert.ToDecimal(fields[constraint.GetProperty("left").GetProperty("name").GetString()!]!) > Convert.ToDecimal(fields[constraint.GetProperty("right").GetProperty("name").GetString()!]!)) throw Error("field order");
            if (kind == "terminal-success-shape" && MatchesLiteral(fields, constraint.GetProperty("when"), "terminalResult") && (!MatchesLiteral(fields, constraint.GetProperty("requires"), "failureCode") || !MatchesLiteral(fields, constraint.GetProperty("requires"), "hasCreation"))) throw Error("terminal success shape");
            if (kind == "terminal-failure-shape" && !MatchesLiteral(fields, constraint.GetProperty("when"), "terminalResultNot", "terminalResult") && (!MatchesLiteral(fields, constraint.GetProperty("requires"), "hasCreation") || !MatchesLiteral(fields, constraint.GetProperty("requires"), "hasApplicationPayload"))) throw Error("terminal failure shape");
            if (kind == "existing-has-no-application-payload" && MatchesPath(fields, "creation.createResult", constraint.GetProperty("when").GetProperty("creation.createResult")) && !MatchesLiteral(fields, constraint.GetProperty("requires"), "hasApplicationPayload")) throw Error("existing creation payload");
        }
    }
    private static bool MatchesLiteral(IReadOnlyDictionary<string, object?> fields, JsonElement source, string sourceName, string? fieldName = null) => fields.TryGetValue(fieldName ?? sourceName, out var value) && Matches(value, source.GetProperty(sourceName));
    private static bool MatchesPath(IReadOnlyDictionary<string, object?> fields, string path, JsonElement expected)
    {
        object? value = fields; foreach (var part in path.Split('.')) value = value switch { IReadOnlyDictionary<string, object?> map when map.TryGetValue(part, out var entry) => entry, WireUnion union when union.Fields.TryGetValue(part, out var entry) => entry, _ => null }; return Matches(value, expected);
    }
    private static bool IsZero(object? value) => value is null || Convert.ToDecimal(value) == 0;

    private static List<object?> ReadVector(JsonElement type, Reader reader, Environment environment)
    {
        var count = ReadLength(type.GetProperty("countType"), reader); if (type.TryGetProperty("maximumItems", out var maximum) && (ulong)count > Unsigned(maximum)) throw Error("vector count");
        var items = new List<object?>(count); for (var i = 0; i < count; ++i) items.Add(Read(Ref(type.GetProperty("item")), reader, environment)); ValidateVector(type, items); return items;
    }
    private static void WriteVector(JsonElement type, IReadOnlyList<object?> items, Writer writer, Environment environment)
    {
        if (type.TryGetProperty("maximumItems", out var maximum) && (ulong)items.Count > Unsigned(maximum)) throw Error("vector count"); ValidateVector(type, items); WriteLength(type.GetProperty("countType"), items.Count, writer); foreach (var item in items) Write(Ref(type.GetProperty("item")), item, writer, environment);
    }
    private static void ValidateVector(JsonElement type, IReadOnlyList<object?> items)
    {
        if (!type.TryGetProperty("constraints", out var constraints)) return;
        foreach (var constraint in constraints.EnumerateArray())
        {
            var keys = items.Select(item => CollectionKey(type, constraint, item)).ToArray(); var kind = constraint.GetProperty("kind").GetString();
            if (kind == "unique" && keys.Select(Convert.ToHexString).Distinct(StringComparer.Ordinal).Count() != keys.Length) throw Error("duplicate vector item");
            if (kind == "sorted" && keys.Zip(keys.Skip(1), (left, right) => CompareBytes(left, right) <= 0).Any(ok => !ok)) throw Error("unsorted vector");
        }
    }
    private static byte[] CollectionKey(JsonElement collection, JsonElement constraint, object? item)
    {
        var itemReference = collection.GetProperty("kind").GetString() == "versioned-vector"
            ? collection.GetProperty("layout").EnumerateArray().Single(x => x.TryGetProperty("kind", out var kind) && kind.GetString() == "repeat").GetProperty("item")
            : collection.GetProperty("item");
        var paths = constraint.TryGetProperty("field", out var field) ? new[] { field.GetProperty("path").GetString()! }
            : constraint.TryGetProperty("fields", out var fields) ? fields.EnumerateArray().Select(x => x.GetProperty("path").GetString()!).ToArray()
            : Array.Empty<string>();
        if (paths.Length == 0) return EncodeKey(itemReference, item, constraint.TryGetProperty("comparison", out var direct) ? direct.GetString() : null);
        var writer = new Writer(); var comparison = constraint.TryGetProperty("comparison", out var declared) ? declared.GetString() : null;
        for (var index = 0; index < paths.Length; ++index)
        {
            var reference = FieldReference(itemReference, paths[index]); var value = Path(item, paths[index]);
            var componentComparison = comparison == "wire-value-then-utf-8-bytes" && index == paths.Length - 1 ? "utf-8-bytes" : comparison;
            writer.Bytes(EncodeKey(reference, value, componentComparison));
        }
        return writer.ToArray();
    }
    private static JsonElement FieldReference(JsonElement reference, string path)
    {
        var current = reference; foreach (var part in path.Split('.')) { var type = Ref(current); var definitions = type.GetProperty("kind").GetString() == "struct" ? type.GetProperty("fields") : type.GetProperty("body"); current = definitions.EnumerateArray().Single(x => x.GetProperty("name").GetString() == part); } return current;
    }
    private static object? Path(object? value, string path)
    {
        foreach (var part in path.Split('.')) value = value switch { IReadOnlyDictionary<string, object?> map => map[part], WireUnion union => union.Fields[part], _ => throw Error("collection key path") }; return value;
    }
    private static byte[] EncodeKey(JsonElement reference, object? value, string? comparison)
    {
        if (comparison == "utf-8-bytes") return Utf8.GetBytes((string)value!);
        var writer = new Writer(); Write(Ref(reference), value, writer, new Environment(null, null, 0)); return writer.ToArray();
    }
    private static int CompareBytes(byte[] left, byte[] right)
    {
        var length = Math.Min(left.Length, right.Length); for (var index = 0; index < length; ++index) { var comparison = left[index].CompareTo(right[index]); if (comparison != 0) return comparison; } return left.Length.CompareTo(right.Length);
    }

    private static List<object?> ReadVersionedVector(JsonElement type, Reader reader, Environment environment)
    {
        var layout = type.GetProperty("layout").EnumerateArray().ToArray(); var version = Read(Ref(layout[0]), reader, environment); ValidateField(layout[0], version); var count = ReadLength(layout[1], reader); var items = new List<object?>(count); for (var i = 0; i < count; ++i) items.Add(Read(Ref(layout[2].GetProperty("item")), reader, environment)); ValidateVector(type, items); return items;
    }
    private static void WriteVersionedVector(JsonElement type, IReadOnlyList<object?> items, Writer writer, Environment environment)
    {
        var layout = type.GetProperty("layout").EnumerateArray().ToArray(); Write(Ref(layout[0]), ConstantValue(Ref(layout[0]), layout[0].GetProperty("constant")), writer, environment); WriteLength(layout[1], items.Count, writer); ValidateVector(type, items); foreach (var item in items) Write(Ref(layout[2].GetProperty("item")), item, writer, environment);
    }
    private static object ConstantValue(JsonElement type, JsonElement constant) => type.GetProperty("kind").GetString() == "enum" ? constant.GetString()! : type.GetProperty("encoding").GetString() == "i64" ? Signed(constant) : Unsigned(constant);

    private static Dictionary<string, object?> ReadDelimited(JsonElement type, Reader reader, Environment environment)
    {
        var version = type.GetProperty("version"); var actual = Read(Ref(version), reader, environment); ValidateField(version, actual); var body = reader.Slice(ReadLength(type.GetProperty("length"), reader)); var fields = ReadFields(type.GetProperty("body"), body, environment); body.End(type.GetProperty("name").GetString()!); ValidateLayout(type, fields); return fields;
    }
    private static void WriteDelimited(JsonElement type, IReadOnlyDictionary<string, object?> fields, Writer writer, Environment environment)
    {
        var version = type.GetProperty("version"); Write(Ref(version), ConstantValue(Ref(version), version.GetProperty("constant")), writer, environment); var body = new Writer(); WriteFields(type.GetProperty("body"), fields, body, environment); var bytes = body.ToArray(); WriteLength(type.GetProperty("length"), bytes.Length, writer); writer.Bytes(bytes); ValidateLayout(type, fields);
    }

    private static WireUnion ReadUnion(JsonElement type, Reader reader, Environment environment)
    {
        var discriminators = new Dictionary<string, object?>();
        foreach (var discriminator in type.GetProperty("discriminators").EnumerateArray())
        {
            var name = discriminator.GetProperty("name").GetString()!; var source = discriminator.GetProperty("source");
            discriminators[name] = source.GetProperty("kind").GetString() switch { "wire" => Read(Ref(discriminator), reader, environment), "enclosingField" => Lookup(environment.Enclosing, source.GetProperty("name").GetString()!, "enclosing discriminator"), "context" => Lookup(environment.Context, source.GetProperty("name").GetString()!, "context discriminator"), _ => throw Error("discriminator source") };
        }
        var cases = type.GetProperty("cases").EnumerateObject().ToArray(); var ordinal = Array.FindIndex(cases, item => CaseMatches(item.Name, discriminators)); JsonElement fields;
        if (ordinal >= 0) fields = cases[ordinal].Value.GetProperty("fields"); else if (type.GetProperty("otherwise").GetProperty("kind").GetString() == "fields") fields = type.GetProperty("otherwise").GetProperty("fields"); else throw Error("union case");
        var selected = type.GetProperty("bodyLengthType").ValueKind == JsonValueKind.Null ? reader : reader.Slice(ReadLength(type.GetProperty("bodyLengthType"), reader)); var values = ReadFields(fields, selected, environment); if (!ReferenceEquals(selected, reader)) selected.End(type.GetProperty("name").GetString()!); return new WireUnion(ordinal, ordinal >= 0 ? cases[ordinal].Name : "", values);
    }
    private static void WriteUnion(JsonElement type, WireUnion union, Writer writer, Environment environment)
    {
        var cases = type.GetProperty("cases").EnumerateObject().ToArray(); JsonElement definitions;
        if (union.Ordinal >= 0 && union.Ordinal < cases.Length) definitions = cases[union.Ordinal].Value.GetProperty("fields"); else if (type.GetProperty("otherwise").GetProperty("kind").GetString() == "fields") definitions = type.GetProperty("otherwise").GetProperty("fields"); else throw Error("union case");
        using var expectedDocument = union.Ordinal >= 0 ? JsonDocument.Parse(cases[union.Ordinal].Name) : null; var actualDiscriminators = new Dictionary<string, object?>();
        foreach (var discriminator in type.GetProperty("discriminators").EnumerateArray()) { var discriminatorName = discriminator.GetProperty("name").GetString()!; var source = discriminator.GetProperty("source"); var sourceKind = source.GetProperty("kind").GetString(); if (sourceKind == "wire") { if (expectedDocument is null) throw Error("otherwise wire discriminator requires a value"); Write(Ref(discriminator), ConstantValue(Ref(discriminator), expectedDocument.RootElement.GetProperty(discriminatorName)), writer, environment); } else { var actual = sourceKind == "enclosingField" ? Lookup(environment.Enclosing, source.GetProperty("name").GetString()!, "enclosing discriminator") : Lookup(environment.Context, source.GetProperty("name").GetString()!, "context discriminator"); actualDiscriminators[discriminatorName] = actual; if (expectedDocument is not null && !Matches(actual, expectedDocument.RootElement.GetProperty(discriminatorName))) throw Error("union discriminator does not match selected case"); } }
        if (expectedDocument is null && cases.Any(item => CaseMatches(item.Name, actualDiscriminators))) throw Error("otherwise union variant matches a declared case");
        if (type.GetProperty("bodyLengthType").ValueKind == JsonValueKind.Null) WriteFields(definitions, union.Fields, writer, environment); else { var body = new Writer(); WriteFields(definitions, union.Fields, body, environment); var bytes = body.ToArray(); WriteLength(type.GetProperty("bodyLengthType"), bytes.Length, writer); writer.Bytes(bytes); }
    }
    private static bool CaseMatches(string signature, IReadOnlyDictionary<string, object?> values)
    {
        using var document = JsonDocument.Parse(signature); return document.RootElement.EnumerateObject().All(x => values.TryGetValue(x.Name, out var actual) && Matches(actual, x.Value));
    }
    private static object? Lookup(IReadOnlyDictionary<string, object?> values, string name, string label) => values.TryGetValue(name, out var value) ? value : throw Error($"missing {label}: {name}");

    private static Dictionary<string, object?> ReadTlv(JsonElement type, Reader reader, Environment environment)
    {
        var body = reader.Slice(ReadLength(type.GetProperty("totalLengthType"), reader)); var result = new Dictionary<string, object?>(); var previous = -1;
        while (body.Remaining > 0)
        {
            var id = ReadLength(type.GetProperty("fieldIdType"), body); if (id <= previous) throw Error("TLV order or duplicate"); previous = id; var valueReader = body.Slice(ReadLength(type.GetProperty("fieldLengthType"), body)); var field = type.GetProperty("fields").EnumerateArray().FirstOrDefault(x => x.GetProperty("id").GetInt32() == id);
            if (field.ValueKind != JsonValueKind.Undefined) { var value = Read(Ref(field), valueReader, environment); valueReader.End("TLV field"); ValidateField(field, value); result[field.GetProperty("name").GetString()!] = value; }
        }
        foreach (var field in type.GetProperty("fields").EnumerateArray()) if (field.GetProperty("required").GetBoolean() && !result.ContainsKey(field.GetProperty("name").GetString()!)) throw Error("required TLV field"); ValidateTlvPresence(type, result, environment); return result;
    }
    private static void WriteTlv(JsonElement type, IReadOnlyDictionary<string, object?> fields, Writer writer, Environment environment)
    {
        ValidateTlvPresence(type, fields, environment); var body = new Writer(); foreach (var field in type.GetProperty("fields").EnumerateArray()) { var name = field.GetProperty("name").GetString()!; if (!fields.TryGetValue(name, out var value) || value is null) { if (field.GetProperty("required").GetBoolean()) throw Error("required TLV field"); continue; } var encoded = new Writer(); ValidateField(field, value); Write(Ref(field), value, encoded, environment); var bytes = encoded.ToArray(); WriteLength(type.GetProperty("fieldIdType"), field.GetProperty("id").GetInt32(), body); WriteLength(type.GetProperty("fieldLengthType"), bytes.Length, body); body.Bytes(bytes); } var all = body.ToArray(); WriteLength(type.GetProperty("totalLengthType"), all.Length, writer); writer.Bytes(all);
    }
    private static void ValidateTlvPresence(JsonElement type, IReadOnlyDictionary<string, object?> fields, Environment environment)
    {
        if (!type.TryGetProperty("presenceRules", out var rules)) return; foreach (var rule in rules.EnumerateArray()) { if (!Condition(rule.GetProperty("when"), fields, environment)) continue; if (rule.TryGetProperty("require", out var required)) foreach (var name in required.EnumerateArray()) if (!fields.TryGetValue(name.GetString()!, out var value) || value is null) throw Error("required TLV presence"); if (rule.TryGetProperty("forbid", out var forbidden)) foreach (var name in forbidden.EnumerateArray()) if (fields.TryGetValue(name.GetString()!, out var value) && value is not null) throw Error("forbidden TLV presence"); }
    }

    private static WireCommand DecodeCommand(int id, IReadOnlyList<byte[]> frames, DecodeContext? context)
    {
        if (frames.Count == 0) throw Error("command frames"); var command = Commands[id]; var reader = new Reader(frames[0]); var protocol = Ir.RootElement.GetProperty("protocol"); foreach (var magic in protocol.GetProperty("magic").EnumerateArray()) if (reader.U8() != magic.GetByte()) throw Error("magic"); if (reader.U8() != protocol.GetProperty("wireMajor").GetByte() || reader.U8() != id) throw Error("command"); var flags = reader.U8(); ValidateFlags(command, flags); var fields = ReadFields(command.GetProperty("body"), reader, new Environment(context, null, flags)); reader.End("command body"); var index = 1; var flagFrames = new Dictionary<string, object?>();
        foreach (var flag in Ir.RootElement.GetProperty("flags").EnumerateArray()) if (flag.TryGetProperty("frame", out var frame) && (flags & flag.GetProperty("bit").GetByte()) != 0) { if (index >= frames.Count) throw Error("missing flag frame"); flagFrames[flag.GetProperty("name").GetString()!] = Read(Ref(frame), new Reader(frames[index++]), new Environment(context, fields, flags)); }
        var policy = command.GetProperty("payload").GetProperty("policy").GetString(); object? payload = null; if (policy == "required" || policy == "optional" && index < frames.Count) { if (index >= frames.Count) throw Error("missing payload"); var payloadReader = new Reader(frames[index++]); payload = Read(Ref(command.GetProperty("payload").GetProperty("type")), payloadReader, new Environment(context, fields, flags)); payloadReader.End("payload"); } if (index != frames.Count) throw Error("extra frame"); ValidateTerminalFailure(fields); return new WireCommand(flags, fields, flagFrames, payload);
    }
    private static byte[][] EncodeCommand(int id, WireCommand value, DecodeContext? context)
    {
        var command = Commands[id]; ValidateFlags(command, value.Flags); var body = new Writer(); var protocol = Ir.RootElement.GetProperty("protocol"); foreach (var magic in protocol.GetProperty("magic").EnumerateArray()) body.U8(magic.GetByte()); body.U8(protocol.GetProperty("wireMajor").GetByte()); body.U8(checked((byte)id)); body.U8(value.Flags); WriteFields(command.GetProperty("body"), value.Fields, body, new Environment(context, value.Fields, value.Flags)); var frames = new List<byte[]> { body.ToArray() };
        foreach (var flag in Ir.RootElement.GetProperty("flags").EnumerateArray()) if (flag.TryGetProperty("frame", out var frame) && (value.Flags & flag.GetProperty("bit").GetByte()) != 0) { var name = flag.GetProperty("name").GetString()!; if (!value.Frames.TryGetValue(name, out var frameValue)) throw Error("missing flag frame"); var encoded = new Writer(); Write(Ref(frame), frameValue, encoded, new Environment(context, value.Fields, value.Flags)); frames.Add(encoded.ToArray()); }
        var policy = command.GetProperty("payload").GetProperty("policy").GetString(); if (policy == "required" && value.Payload is null || policy == "forbidden" && value.Payload is not null) throw Error("payload policy"); ValidateTerminalFailure(value.Fields); if (value.Payload is not null) { var encoded = new Writer(); Write(Ref(command.GetProperty("payload").GetProperty("type")), value.Payload, encoded, new Environment(context, value.Fields, value.Flags)); frames.Add(encoded.ToArray()); } return frames.ToArray();
    }
    private static void ValidateFlags(JsonElement command, byte flags)
    {
        var allowed = command.GetProperty("allowedFlags").EnumerateArray().Aggregate(0, (mask, x) => mask | FlagBit(x.GetProperty("name").GetString()!)); var required = command.GetProperty("requiredFlags").EnumerateArray().Aggregate(0, (mask, x) => mask | FlagBit(x.GetProperty("name").GetString()!)); if ((flags & ~allowed) != 0 || (flags & required) != required) throw Error("flags");
        if (command.TryGetProperty("flagConstraints", out var constraints)) foreach (var constraint in constraints.EnumerateArray()) { var kind = constraint.GetProperty("kind").GetString(); if (kind == "all-or-none") { var bits = constraint.GetProperty("flags").EnumerateArray().Select(x => FlagBit(x.GetProperty("name").GetString()!)).ToArray(); var count = bits.Count(bit => (flags & bit) != 0); if (count != 0 && count != bits.Length) throw Error("flag constraint"); } else if (kind == "implies" && (flags & FlagBit(constraint.GetProperty("if").GetProperty("name").GetString()!)) != 0 && constraint.GetProperty("then").EnumerateArray().Any(x => (flags & FlagBit(x.GetProperty("name").GetString()!)) == 0)) throw Error("flag implication"); }
    }
    private static void ValidateTerminalFailure(IReadOnlyDictionary<string, object?> fields)
    {
        var predicate = Ir.RootElement.GetProperty("semanticConstraints").EnumerateArray().FirstOrDefault(x => x.TryGetProperty("runtimePredicate", out _)); if (predicate.ValueKind == JsonValueKind.Undefined || !fields.TryGetValue("terminalResult", out var terminal) || !fields.TryGetValue("failureCode", out var failure)) return; var declaration = predicate.GetProperty("runtimePredicate"); if (declaration.GetProperty("asset").GetString() != "service-wire-constants" || declaration.GetProperty("name").GetString() != "valid-terminal-failure") throw Error("unknown runtime predicate"); if (!ServiceWireConstants.ValidTerminalFailure(checked((uint)EnumWireValue("request-terminal-result", terminal)), checked((uint)EnumWireValue("framework-error-code", failure)))) throw Error("terminal failure integrity");
    }
    private static long EnumWireValue(string typeName, object? value)
    {
        foreach (var item in Types[typeName].GetProperty("values").EnumerateArray()) if (Matches(value, item.GetProperty("name"))) return item.GetProperty("value").GetInt64(); throw Error("enum value");
    }

    private static object? DecodeDurable(string name, byte[] bytes)
    {
        var format = Formats[name]; if ((ulong)bytes.LongLength > Unsigned(format.GetProperty("maximumEncodedBytes"))) throw Error("durable maximum"); var reader = new Reader(bytes); var coveredLength = bytes.Length - 4; foreach (var magic in format.GetProperty("magic").EnumerateArray()) if (reader.U8() != magic.GetByte()) throw Error("durable magic"); if (reader.U8() != format.GetProperty("formatVersion").GetByte()) throw Error("durable version"); var flags = Read(Ref(format.GetProperty("flagsType")), reader, new Environment(null, null, 0)); if (Convert.ToUInt64(flags) != format.GetProperty("flags").GetUInt64()) throw Error("durable flags"); var body = reader.Slice(ReadLength(format.GetProperty("bodyLengthType"), reader)); var value = Read(Ref(format.GetProperty("body")), body, new Environment(null, null, 0)); body.End(name); var expected = reader.U32(); reader.End(name); if (Crc32C(bytes.AsSpan(0, coveredLength)) != expected) throw Error("durable checksum"); return value;
    }
    private static byte[] EncodeDurable(string name, object? value)
    {
        var format = Formats[name]; var body = new Writer(); Write(Ref(format.GetProperty("body")), value, body, new Environment(null, null, 0)); var bodyBytes = body.ToArray(); var writer = new Writer(); foreach (var magic in format.GetProperty("magic").EnumerateArray()) writer.U8(magic.GetByte()); writer.U8(format.GetProperty("formatVersion").GetByte()); Write(Ref(format.GetProperty("flagsType")), format.GetProperty("flags").GetUInt64(), writer, new Environment(null, null, 0)); WriteLength(format.GetProperty("bodyLengthType"), bodyBytes.Length, writer); writer.Bytes(bodyBytes); var covered = writer.ToArray(); writer.U32(Crc32C(covered)); var result = writer.ToArray(); if ((ulong)result.LongLength > Unsigned(format.GetProperty("maximumEncodedBytes"))) throw Error("durable maximum"); return result;
    }
    private static uint Crc32C(ReadOnlySpan<byte> bytes) { var crc = uint.MaxValue; foreach (var value in bytes) { crc ^= value; for (var bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0x82f63b78u & (uint)-(int)(crc & 1)); } return ~crc; }
    private static IReadOnlyDictionary<string, object?> Fields(object? value) => value as IReadOnlyDictionary<string, object?> ?? throw Error("record value");
    private static IReadOnlyList<object?> Items(object? value) => value as IReadOnlyList<object?> ?? throw Error("vector value");`;
}

const [mode, schemaArgument, ...rest] = process.argv.slice(2);
if (!(["--write", "--check"].includes(mode)) || schemaArgument === undefined || rest.length !== 0) {
  console.error("usage: node render-service-wire-dotnet.mjs --write|--check <schema>");
  process.exit(2);
}
const rendered = `${render(lowerSchema(path.resolve(schemaArgument)))}\n`;
if (mode === "--write") {
  fs.mkdirSync(path.dirname(output), { recursive: true });
  fs.writeFileSync(output, rendered, "utf8");
} else if (!fs.existsSync(output) || fs.readFileSync(output, "utf8") !== rendered) {
  console.error(`generated .NET service-wire codec is stale: ${output}`);
  process.exit(1);
}
