import type { ZLinkJsonSchema } from './json-contract';

export const FRAMEWORK_JSON_V1_PROFILE = 'framework-json-v1';

const SIGNED_64_MIN = -(1n << 63n);
const UNSIGNED_64_MAX = (1n << 64n) - 1n;

export interface FrameworkJsonParseOptions {
  readonly rejectPropertyName?: (name: string) => boolean;
}

export function stringifyFrameworkJsonV1(
  value: unknown,
  schema?: ZLinkJsonSchema,
  replacer?: (this: unknown, key: string, value: unknown) => unknown
): string {
  if (value === undefined || typeof value === 'function' || typeof value === 'symbol') {
    throw new TypeError('framework-json-v1 payload is not a JSON value.');
  }
  const encoded =
    replacer === undefined
      ? JSON.stringify(value, frameworkJsonV1Replacer)
      : stringifyWithReplacer(value, replacer);
  if (schema !== undefined) {
    validateFrameworkJsonV1Value(JSON.parse(encoded), schema, '$', false);
  }
  return encoded;
}

// Keep custom callback captures off the default encoding path.
function stringifyWithReplacer(
  value: unknown,
  replacer: (this: unknown, key: string, value: unknown) => unknown
): string {
  return JSON.stringify(value, function (key, item) {
    const transformed = replacer.call(this, key, item);
    return frameworkJsonV1Replacer.call({ [key]: transformed }, key, transformed);
  });
}

//  Module-level so JSON.stringify does not allocate a fresh closure per
//  encoded message. On the root call `this` is the {'': value} wrapper, so
//  this[key] resolves the raw (pre-toJSON) source for every key including ''.
function frameworkJsonV1Replacer(
  this: Record<string, unknown>,
  key: string,
  item: unknown
): unknown {
  const source = this[key];
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
    return encodeBase64(source);
  }
  if (source instanceof Date) {
    throw new TypeError('framework-json-v1 does not implicitly encode Date values.');
  }
  if (
    typeof source === 'object' &&
    source !== null &&
    typeof (source as { readonly toJSON?: unknown }).toJSON === 'function'
  ) {
    throw new TypeError('framework-json-v1 does not implicitly invoke custom toJSON methods.');
  }
  return item;
}

export function parseFrameworkJsonV1(
  text: string,
  options: FrameworkJsonParseOptions = {},
  schema?: ZLinkJsonSchema,
  reviver?: (this: unknown, key: string, value: unknown) => unknown
): unknown {
  if (text.charCodeAt(0) === 0xfeff) {
    throw new SyntaxError('framework-json-v1 does not allow a UTF-8 BOM.');
  }
  new JsonPropertyScanner(text, options).scan();
  const parsed = JSON.parse(text, reviver);
  return schema === undefined ? parsed : validateFrameworkJsonV1Value(parsed, schema, '$', true);
}

//  Sticky pattern shared across scans; matching happens synchronously so the
//  mutable lastIndex never crosses calls.
const NUMBER_TOKEN_PATTERN = /-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?/uy;

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
      if (typeof value !== 'number' || !Number.isFinite(value))
        schemaFailure(path, 'a finite number');
      return value;
    case 'int32':
      if (
        !Number.isInteger(value) ||
        (value as number) < INT32_MIN ||
        (value as number) > INT32_MAX
      ) {
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
      if (!hasCanonicalBase64Padding(value as string))
        schemaFailure(path, 'canonical padded RFC 4648 base64');
      // decodeBase64 returns fresh storage owned by this value.
      return decode ? decodeBase64(value as string) : value;
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
        validateFrameworkJsonV1Value(item, schema.items, `${path}[${index}]`, decode)
      );
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
        result[name] = validateFrameworkJsonV1Value(
          item,
          propertySchema as ZLinkJsonSchema,
          `${path}.${name}`,
          decode
        );
      }
      return decode ? result : value;
    }
  }
}

function validateDecimalInteger(
  value: unknown,
  path: string,
  signed: boolean,
  decode: boolean
): unknown {
  if (typeof value !== 'string' || !/^(?:0|[1-9][0-9]*|-[1-9][0-9]*)$/u.test(value)) {
    schemaFailure(
      path,
      signed ? 'a canonical int64 decimal string' : 'a canonical uint64 decimal string'
    );
  }
  const parsed = BigInt(value as string);
  const minimum = signed ? SIGNED_64_MIN : 0n;
  const maximum = signed ? (1n << 63n) - 1n : UNSIGNED_64_MAX;
  if (parsed < minimum || parsed > maximum)
    schemaFailure(path, signed ? 'an int64 value' : 'a uint64 value');
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
  private readonly firstEscape: number;

  constructor(
    private readonly text: string,
    private readonly options: FrameworkJsonParseOptions
  ) {
    const escape = text.indexOf('\\');
    this.firstEscape = escape === -1 ? text.length : escape;
  }

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
    const text = this.text;
    const start = this.index;
    let index = start + 1;
    //  Before the first backslash of the document every token ends at the
    //  next quote, so native search finds it; later tokens walk the escapes.
    const end = text.indexOf('"', index);
    if (end !== -1 && end < this.firstEscape) {
      this.index = end + 1;
      return text.slice(index, end);
    }
    let sawEscape = false;
    for (;;) {
      if (index >= text.length) {
        this.index = index;
        this.fail('unterminated string');
      }
      const character = text[index++];
      if (character === '"') {
        this.index = index;
        //  Without escapes the token content is verbatim; the outer
        //  JSON.parse pass still rejects any raw control characters.
        return sawEscape
          ? (JSON.parse(text.slice(start, index)) as string)
          : text.slice(start + 1, index - 1);
      }
      if (character === '\\') {
        sawEscape = true;
        if (index >= text.length) {
          this.index = index;
          this.fail('unterminated escape');
        }
        if (text[index++] === 'u') index += 4;
      }
    }
  }

  private scanLiteral(value: 'true' | 'false' | 'null'): void {
    if (!this.text.startsWith(value, this.index)) this.fail(value);
    this.index += value.length;
  }

  private scanNumber(): void {
    NUMBER_TOKEN_PATTERN.lastIndex = this.index;
    const match = NUMBER_TOKEN_PATTERN.exec(this.text);
    if (match === null) this.fail('JSON value');
    if (!Number.isFinite(Number(match[0]))) {
      throw new TypeError('framework-json-v1 only accepts finite JSON numbers.');
    }
    this.index += match[0].length;
  }

  private skipWhitespace(): void {
    for (;;) {
      const code = this.text.charCodeAt(this.index);
      if (code === 0x20 || code === 0x09 || code === 0x0a || code === 0x0d) {
        this.index += 1;
        continue;
      }
      return;
    }
  }

  private fail(expected: string): never {
    throw new SyntaxError(`framework-json-v1 expected ${expected} at offset ${this.index}.`);
  }
}

const BASE64_STRING_CHUNK_BYTES = 8192;
const BASE64_ALPHABET = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

/**
 * Padded RFC 4648 text that matches BASE64_PATTERN is canonical exactly when
 * the bits after the last encoded byte are zero: the low four bits of the
 * symbol before `==`, or the low two bits of the symbol before `=`. Checking
 * those bits replaces re-encoding the decoded bytes for comparison.
 */
function hasCanonicalBase64Padding(value: string): boolean {
  if (value.endsWith('=='))
    return (BASE64_ALPHABET.indexOf(value.charAt(value.length - 3)) & 0x0f) === 0;
  if (value.endsWith('='))
    return (BASE64_ALPHABET.indexOf(value.charAt(value.length - 2)) & 0x03) === 0;
  return true;
}

function encodeBase64(bytes: Uint8Array): string {
  let binary = '';
  for (let offset = 0; offset < bytes.length; offset += BASE64_STRING_CHUNK_BYTES) {
    binary += String.fromCharCode(...bytes.subarray(offset, offset + BASE64_STRING_CHUNK_BYTES));
  }
  return btoa(binary);
}

function decodeBase64(value: string): Uint8Array {
  const binary = atob(value);
  const bytes = new Uint8Array(binary.length);
  for (let index = 0; index < binary.length; index++) bytes[index] = binary.charCodeAt(index);
  return bytes;
}
