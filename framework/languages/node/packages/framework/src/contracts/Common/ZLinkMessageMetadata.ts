export interface ZLinkMessageMetadata {
  readonly values: ReadonlyMap<string, string>;
  find(key: string): string | undefined;
}

export interface ZLinkMessageMetadataPolicy {
  canForwardSessionToActor(key: string): boolean;
  canForwardActorToSession(key: string): boolean;
}

class ImmutableZLinkMessageMetadata implements ZLinkMessageMetadata {
  readonly values: ReadonlyMap<string, string>;

  constructor(values: Iterable<readonly [string, string]> = []) {
    this.values = Object.freeze(new ImmutableMetadataMap(values));
  }

  find(key: string): string | undefined {
    return this.values.get(key);
  }
}

class ImmutableMetadataMap implements ReadonlyMap<string, string> {
  readonly #values: Map<string, string>;

  constructor(values: Iterable<readonly [string, string]>) {
    this.#values = new Map(values);
  }

  get size(): number {
    return this.#values.size;
  }

  get(key: string): string | undefined {
    return this.#values.get(key);
  }

  has(key: string): boolean {
    return this.#values.has(key);
  }

  forEach(
    callbackfn: (value: string, key: string, map: ReadonlyMap<string, string>) => void,
    thisArg?: unknown
  ): void {
    this.#values.forEach((value, key) => callbackfn.call(thisArg, value, key, this));
  }

  entries(): MapIterator<[string, string]> {
    return this.#values.entries();
  }

  keys(): MapIterator<string> {
    return this.#values.keys();
  }

  values(): MapIterator<string> {
    return this.#values.values();
  }

  [Symbol.iterator](): MapIterator<[string, string]> {
    return this.#values[Symbol.iterator]();
  }
}

export const ZLinkMessageMetadataEmpty: ZLinkMessageMetadata = Object.freeze(
  new ImmutableZLinkMessageMetadata()
);

export function zlinkMessageMetadata(
  values: ReadonlyMap<string, string> | Readonly<Record<string, string>>
): ZLinkMessageMetadata {
  const entries =
    typeof (values as ReadonlyMap<string, string>)[Symbol.iterator] === 'function'
      ? (values as ReadonlyMap<string, string>)
      : Object.entries(values);
  const size = Array.isArray(entries)
    ? entries.length
    : (entries as ReadonlyMap<string, string>).size;
  return size === 0
    ? ZLinkMessageMetadataEmpty
    : Object.freeze(new ImmutableZLinkMessageMetadata(entries));
}
