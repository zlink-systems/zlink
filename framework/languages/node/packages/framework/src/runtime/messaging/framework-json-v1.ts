import type { ZLinkJsonSchema } from '../../contracts/Handlers/JsonContract';

export const FRAMEWORK_JSON_V1_PROFILE = 'framework-json-v1';

const SIGNED_64_MIN = -(1n << 63n);
const UNSIGNED_64_MAX = (1n << 64n) - 1n;

export interface FrameworkJsonParseOptions {
  readonly rejectPropertyName?: (name: string) => boolean;
}

export function stringifyFrameworkJsonV1(value: unknown, schema?: ZLinkJsonSchema): string {
  if (value === undefined || typeof value === 'function' || typeof value === 'symbol') {
    throw new TypeError('framework-json-v1 payload is not a JSON value.');
  }
  const root = value;
  const encoded = JSON.stringify(root, function (key, item: unknown) {
    const source = key === '' ? root : (this as Record<string, unknown>)[key];
    if (typeof source === 'number' && !Number.isFinite(source)) {
      throw new TypeError('framework-json-v1 only accepts finite JSON numbers.');
    }
    if (typeof source === 'bigint') {
      if (source < SIGNED_64_MIN || source > UNSIGNED_64_MAX) {
        throw new RangeError('framework-json-v1 64-bit integer is outside the supported range.');
      }
      return source.toString(10);
    }
    if (source instanceof Uint8Array) {
      return Buffer.from(source.buffer, source.byteOffset, source.byteLength).toString('base64');
    }
    if (source instanceof Date) {
      throw new TypeError('framework-json-v1 does not implicitly encode Date values.');
    }
    if (
      typeof source === 'object'
      && source !== null
      && typeof (source as { readonly toJSON?: unknown }).toJSON === 'function'
    ) {
      throw new TypeError('framework-json-v1 does not implicitly invoke custom toJSON methods.');
    }
    return item;
  });
  if (schema !== undefined) {
    validateFrameworkJsonV1Value(JSON.parse(encoded), schema, '$', false);
  }
  return encoded;
}

export function parseFrameworkJsonV1(
  text: string,
  options: FrameworkJsonParseOptions = {},
  schema?: ZLinkJsonSchema
): unknown {
  if (text.charCodeAt(0) === 0xfeff) {
    throw new SyntaxError('framework-json-v1 does not allow a UTF-8 BOM.');
  }
  new JsonPropertyScanner(text, options).scan();
  const parsed = JSON.parse(text);
  return schema === undefined
    ? parsed
    : validateFrameworkJsonV1Value(parsed, schema, '$', true);
}

const BASE64_PATTERN = /^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/u;
const INT32_MIN = -2_147_483_648;
const INT32_MAX = 2_147_483_647;
const UINT32_MAX = 4_294_967_295;

function validateFrameworkJsonV1Value(
  value: unknown,
  schema: ZLinkJsonSchema,
  path: string,
  decode: boolean
): unknown {
  switch (schema.type) {
    case 'boolean':
      if (typeof value !== 'boolean') schemaFailure(path, 'a boolean');
      return value;
    case 'string':
      if (typeof value !== 'string') schemaFailure(path, 'a string');
      return value;
    case 'number':
      if (typeof value !== 'number' || !Number.isFinite(value)) schemaFailure(path, 'a finite number');
      return value;
    case 'int32':
      if (!Number.isInteger(value) || (value as number) < INT32_MIN || (value as number) > INT32_MAX) {
        schemaFailure(path, 'an int32 JSON number');
      }
      return value;
    case 'uint32':
      if (!Number.isInteger(value) || (value as number) < 0 || (value as number) > UINT32_MAX) {
        schemaFailure(path, 'a uint32 JSON number');
      }
      return value;
    case 'int64':
      return validateDecimalInteger(value, path, true, decode);
    case 'uint64':
      return validateDecimalInteger(value, path, false, decode);
    case 'bytes': {
      if (typeof value !== 'string' || !BASE64_PATTERN.test(value)) {
        schemaFailure(path, 'padded RFC 4648 base64');
      }
      const bytes = Buffer.from(value as string, 'base64');
      if (bytes.toString('base64') !== value) schemaFailure(path, 'canonical padded RFC 4648 base64');
      return decode ? Uint8Array.from(bytes) : value;
    }
    case 'enum':
      if (typeof value !== 'string' || !schema.names.includes(value)) {
        schemaFailure(path, `one of ${schema.names.join(', ')}`);
      }
      return value;
    case 'nullable':
      return value === null
        ? null
        : validateFrameworkJsonV1Value(value, schema.value, path, decode);
    case 'array':
      if (!Array.isArray(value)) schemaFailure(path, 'an array');
      return (value as unknown[]).map((item, index) =>
        validateFrameworkJsonV1Value(item, schema.items, `${path}[${index}]`, decode));
    case 'record': {
      if (!isJsonObject(value)) schemaFailure(path, 'an object');
      const result: Record<string, unknown> = {};
      for (const [name, item] of Object.entries(value as Record<string, unknown>)) {
        result[name] = validateFrameworkJsonV1Value(item, schema.values, `${path}.${name}`, decode);
      }
      return decode ? result : value;
    }
    case 'object': {
      if (!isJsonObject(value)) schemaFailure(path, 'an object');
      const input = value as Record<string, unknown>;
      for (const name of schema.required) {
        if (!Object.prototype.hasOwnProperty.call(input, name)) {
          throw new TypeError(`framework-json-v1 required property '${path}.${name}' is missing.`);
        }
      }
      const result: Record<string, unknown> = {};
      for (const [name, item] of Object.entries(input)) {
        const propertySchema: unknown = schema.properties[name];
        if (propertySchema === undefined) {
          if (schema.additionalProperties === false) {
            throw new TypeError(`framework-json-v1 property '${path}.${name}' is not allowed.`);
          }
          continue;
        }
        result[name] = validateFrameworkJsonV1Value(item, propertySchema as ZLinkJsonSchema, `${path}.${name}`, decode);
      }
      return decode ? result : value;
    }
  }
}

function validateDecimalInteger(value: unknown, path: string, signed: boolean, decode: boolean): unknown {
  if (typeof value !== 'string' || !/^(?:0|[1-9][0-9]*|-[1-9][0-9]*)$/u.test(value)) {
    schemaFailure(path, signed ? 'a canonical int64 decimal string' : 'a canonical uint64 decimal string');
  }
  const parsed = BigInt(value as string);
  const minimum = signed ? SIGNED_64_MIN : 0n;
  const maximum = signed ? (1n << 63n) - 1n : UNSIGNED_64_MAX;
  if (parsed < minimum || parsed > maximum) schemaFailure(path, signed ? 'an int64 value' : 'a uint64 value');
  return decode ? parsed : value;
}

function isJsonObject(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

function schemaFailure(path: string, expected: string): never {
  throw new TypeError(`framework-json-v1 value '${path}' must be ${expected}.`);
}

class JsonPropertyScanner {
  private index = 0;

  constructor(
    private readonly text: string,
    private readonly options: FrameworkJsonParseOptions
  ) {}

  scan(): void {
    this.skipWhitespace();
    this.scanValue();
    this.skipWhitespace();
    if (this.index !== this.text.length) this.fail('trailing data');
  }

  private scanValue(): void {
    this.skipWhitespace();
    const token = this.text[this.index];
    if (token === '{') return this.scanObject();
    if (token === '[') return this.scanArray();
    if (token === '"') {
      this.scanString();
      return;
    }
    if (token === 't') return this.scanLiteral('true');
    if (token === 'f') return this.scanLiteral('false');
    if (token === 'n') return this.scanLiteral('null');
    this.scanNumber();
  }

  private scanObject(): void {
    this.index++;
    this.skipWhitespace();
    if (this.text[this.index] === '}') {
      this.index++;
      return;
    }
    const properties = new Set<string>();
    for (;;) {
      this.skipWhitespace();
      if (this.text[this.index] !== '"') this.fail('object property name');
      const property = this.scanString();
      if (properties.has(property)) {
        throw new SyntaxError(`framework-json-v1 duplicate property '${property}'.`);
      }
      properties.add(property);
      if (this.options.rejectPropertyName?.(property) === true) {
        throw new SyntaxError(`framework-json-v1 property '${property}' is not allowed.`);
      }
      this.skipWhitespace();
      if (this.text[this.index] !== ':') this.fail("':' after object property");
      this.index++;
      this.scanValue();
      this.skipWhitespace();
      const delimiter = this.text[this.index++];
      if (delimiter === '}') return;
      if (delimiter !== ',') this.fail("',' or '}' after object property");
    }
  }

  private scanArray(): void {
    this.index++;
    this.skipWhitespace();
    if (this.text[this.index] === ']') {
      this.index++;
      return;
    }
    for (;;) {
      this.scanValue();
      this.skipWhitespace();
      const delimiter = this.text[this.index++];
      if (delimiter === ']') return;
      if (delimiter !== ',') this.fail("',' or ']' after array item");
    }
  }

  private scanString(): string {
    const start = this.index++;
    for (;;) {
      if (this.index >= this.text.length) this.fail('unterminated string');
      const character = this.text[this.index++]!;
      if (character === '"') {
        return JSON.parse(this.text.slice(start, this.index)) as string;
      }
      if (character === '\\') {
        if (this.index >= this.text.length) this.fail('unterminated escape');
        const escaped = this.text[this.index++]!;
        if (escaped === 'u') this.index += 4;
      }
    }
  }

  private scanLiteral(value: 'true' | 'false' | 'null'): void {
    if (!this.text.startsWith(value, this.index)) this.fail(value);
    this.index += value.length;
  }

  private scanNumber(): void {
    const match = /^-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?/u
      .exec(this.text.slice(this.index));
    if (match === null) this.fail('JSON value');
    this.index += match[0].length;
  }

  private skipWhitespace(): void {
    while (/\s/u.test(this.text[this.index] ?? '') && this.text[this.index] !== '\ufeff') {
      this.index++;
    }
  }

  private fail(expected: string): never {
    throw new SyntaxError(`framework-json-v1 expected ${expected} at offset ${this.index}.`);
  }
}
