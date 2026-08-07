export const FRAMEWORK_JSON_V1_PROFILE = 'framework-json-v1';

const SIGNED_64_MIN = -(1n << 63n);
const UNSIGNED_64_MAX = (1n << 64n) - 1n;

export interface FrameworkJsonParseOptions {
  readonly rejectPropertyName?: (name: string) => boolean;
}

export function stringifyFrameworkJsonV1(value: unknown): string {
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
  if (encoded === undefined) {
    throw new TypeError('framework-json-v1 payload is not a JSON value.');
  }
  return encoded;
}

export function parseFrameworkJsonV1(
  text: string,
  options: FrameworkJsonParseOptions = {}
): unknown {
  if (text.charCodeAt(0) === 0xfeff) {
    throw new SyntaxError('framework-json-v1 does not allow a UTF-8 BOM.');
  }
  new JsonPropertyScanner(text, options).scan();
  return JSON.parse(text);
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
      const character = this.text[this.index++];
      if (character === undefined) this.fail('unterminated string');
      if (character === '"') {
        return JSON.parse(this.text.slice(start, this.index)) as string;
      }
      if (character === '\\') {
        const escaped = this.text[this.index++];
        if (escaped === 'u') this.index += 4;
        else if (escaped === undefined) this.fail('unterminated escape');
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
