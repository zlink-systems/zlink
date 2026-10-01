const enum DynamicValueField {
  Kind = 1,
  Bool = 2,
  Number = 3,
  String = 4,
  Object = 5,
  Array = 6,
  Bytes = 7
}
const enum ObjectEntryField {
  Key = 1,
  Value = 2
}

const enum WireType {
  Varint = 0,
  Fixed64 = 1,
  LengthDelimited = 2
}

const enum ValueKind {
  Null = 0,
  Bool = 1,
  Number = 2,
  String = 3,
  Object = 4,
  Array = 5,
  Bytes = 6
}

export function createDynamicValueProtobufType(): {
  encode(value: unknown): { finish(): Uint8Array };
  decode(reader: Uint8Array): unknown;
} {
  return {
    encode(value: unknown): { finish(): Uint8Array } {
      return { finish: () => encodeDynamicValue(value) };
    },
    decode(reader: Uint8Array): unknown {
      return decodeDynamicValue(Buffer.from(reader));
    }
  };
}

export function encodeDynamicValue(value: unknown): Buffer {
  if (value === null || value === undefined) {
    return encodeFields([encodeVarintField(DynamicValueField.Kind, ValueKind.Null)]);
  }
  if (typeof value === 'boolean') {
    return encodeFields([
      encodeVarintField(DynamicValueField.Kind, ValueKind.Bool),
      encodeVarintField(DynamicValueField.Bool, value ? 1 : 0)
    ]);
  }
  if (typeof value === 'number') {
    return encodeFields([
      encodeVarintField(DynamicValueField.Kind, ValueKind.Number),
      encodeDoubleField(DynamicValueField.Number, value)
    ]);
  }
  if (typeof value === 'string') {
    return encodeFields([
      encodeVarintField(DynamicValueField.Kind, ValueKind.String),
      encodeBytesField(DynamicValueField.String, Buffer.from(value))
    ]);
  }
  if (value instanceof Uint8Array) {
    return encodeFields([
      encodeVarintField(DynamicValueField.Kind, ValueKind.Bytes),
      encodeBytesField(
        DynamicValueField.Bytes,
        Buffer.from(value.buffer, value.byteOffset, value.byteLength)
      )
    ]);
  }
  if (Array.isArray(value)) {
    return encodeFields([
      encodeVarintField(DynamicValueField.Kind, ValueKind.Array),
      ...value.map((item) => encodeBytesField(DynamicValueField.Array, encodeDynamicValue(item)))
    ]);
  }
  if (typeof value === 'object') {
    return encodeFields([
      encodeVarintField(DynamicValueField.Kind, ValueKind.Object),
      ...Object.entries(value as Record<string, unknown>).map(([key, entryValue]) =>
        encodeBytesField(DynamicValueField.Object, encodeObjectEntry(key, entryValue))
      )
    ]);
  }
  throw new Error(`Protobuf serializer cannot encode value of type '${typeof value}'.`);
}

export function decodeDynamicValue(bytes: Buffer): unknown {
  let kind = ValueKind.Null;
  let boolValue = false;
  let numberValue = 0;
  let stringValue = '';
  let bytesValue = Buffer.alloc(0);
  const objectValue: Record<string, unknown> = {};
  const arrayValue: unknown[] = [];

  for (const field of readFields(bytes)) {
    switch (field.fieldNumber) {
      case DynamicValueField.Kind:
        kind = Number(readVarintPayload(field)) as ValueKind;
        break;
      case DynamicValueField.Bool:
        boolValue = readVarintPayload(field) !== 0n;
        break;
      case DynamicValueField.Number:
        numberValue = readDoublePayload(field);
        break;
      case DynamicValueField.String:
        stringValue = readBytesPayload(field).toString();
        break;
      case DynamicValueField.Object: {
        const entry = decodeObjectEntry(readBytesPayload(field));
        objectValue[entry.key] = entry.value;
        break;
      }
      case DynamicValueField.Array:
        arrayValue.push(decodeDynamicValue(readBytesPayload(field)));
        break;
      case DynamicValueField.Bytes:
        bytesValue = Buffer.from(readBytesPayload(field));
        break;
      default:
        break;
    }
  }

  switch (kind) {
    case ValueKind.Null:
      return null;
    case ValueKind.Bool:
      return boolValue;
    case ValueKind.Number:
      return numberValue;
    case ValueKind.String:
      return stringValue;
    case ValueKind.Object:
      return objectValue;
    case ValueKind.Array:
      return arrayValue;
    case ValueKind.Bytes:
      return bytesValue;
    default:
      throw new Error(`Protobuf serializer cannot decode value kind '${kind}'.`);
  }
}

function encodeObjectEntry(key: string, value: unknown): Buffer {
  return encodeFields([
    encodeBytesField(ObjectEntryField.Key, Buffer.from(key)),
    encodeBytesField(ObjectEntryField.Value, encodeDynamicValue(value))
  ]);
}

function decodeObjectEntry(bytes: Buffer): { readonly key: string; readonly value: unknown } {
  let key = '';
  let value: unknown = null;
  for (const field of readFields(bytes)) {
    if (field.fieldNumber === ObjectEntryField.Key) {
      key = readBytesPayload(field).toString();
    } else if (field.fieldNumber === ObjectEntryField.Value) {
      value = decodeDynamicValue(readBytesPayload(field));
    }
  }
  return { key, value };
}

function encodeFields(fields: readonly Buffer[]): Buffer {
  return Buffer.concat(fields);
}

function encodeVarintField(fieldNumber: number, value: number | bigint): Buffer {
  return Buffer.concat([encodeVarint(fieldKey(fieldNumber, WireType.Varint)), encodeVarint(value)]);
}

function encodeDoubleField(fieldNumber: number, value: number): Buffer {
  const payload = Buffer.allocUnsafe(8);
  payload.writeDoubleLE(value);
  return Buffer.concat([encodeVarint(fieldKey(fieldNumber, WireType.Fixed64)), payload]);
}

function encodeBytesField(fieldNumber: number, value: Buffer): Buffer {
  return Buffer.concat([
    encodeVarint(fieldKey(fieldNumber, WireType.LengthDelimited)),
    encodeVarint(value.length),
    value
  ]);
}

function fieldKey(fieldNumber: number, wireType: WireType): number {
  return (fieldNumber << 3) | wireType;
}

function encodeVarint(value: number | bigint): Buffer {
  let remaining = BigInt(value);
  const bytes = Buffer.allocUnsafe(10);
  let offset = 0;
  do {
    let byte = Number(remaining & 0x7fn);
    remaining >>= 7n;
    if (remaining !== 0n) byte |= 0x80;
    bytes[offset++] = byte;
  } while (remaining !== 0n);
  return Buffer.from(bytes.subarray(0, offset));
}

type WireField = {
  readonly fieldNumber: number;
  readonly wireType: number;
  readonly payload: Buffer;
};

function readFields(bytes: Buffer): WireField[] {
  const fields: WireField[] = [];
  let offset = 0;
  while (offset < bytes.length) {
    const key = readVarint(bytes, offset);
    offset = key.offset;
    const fieldNumber = Number(key.value >> 3n);
    const wireType = Number(key.value & 0x07n);
    if (wireType === WireType.Varint) {
      const value = readVarint(bytes, offset);
      fields.push({ fieldNumber, wireType, payload: bytes.subarray(offset, value.offset) });
      offset = value.offset;
    } else if (wireType === WireType.Fixed64) {
      fields.push({ fieldNumber, wireType, payload: bytes.subarray(offset, offset + 8) });
      offset += 8;
    } else if (wireType === WireType.LengthDelimited) {
      const length = readVarint(bytes, offset);
      offset = length.offset;
      const end = offset + Number(length.value);
      fields.push({ fieldNumber, wireType, payload: bytes.subarray(offset, end) });
      offset = end;
    } else {
      throw new Error(`Protobuf serializer cannot read wire type '${wireType}'.`);
    }
  }
  return fields;
}

function readVarintPayload(field: WireField): bigint {
  ensureWireType(field, WireType.Varint);
  return readVarint(field.payload, 0).value;
}

function readDoublePayload(field: WireField): number {
  ensureWireType(field, WireType.Fixed64);
  return field.payload.readDoubleLE();
}

function readBytesPayload(field: WireField): Buffer {
  ensureWireType(field, WireType.LengthDelimited);
  return field.payload;
}

function ensureWireType(field: WireField, expected: WireType): void {
  if (field.wireType !== expected) {
    throw new Error(
      `Protobuf field '${field.fieldNumber}' has wire type '${field.wireType}', not '${expected}'.`
    );
  }
}

function readVarint(
  bytes: Buffer,
  start: number
): { readonly value: bigint; readonly offset: number } {
  let value = 0n;
  let shift = 0n;
  let offset = start;
  while (offset < bytes.length) {
    const byte = BigInt(bytes[offset]);
    value |= (byte & 0x7fn) << shift;
    offset += 1;
    if ((byte & 0x80n) === 0n) return { value, offset };
    shift += 7n;
  }
  throw new Error('Protobuf varint is truncated.');
}
