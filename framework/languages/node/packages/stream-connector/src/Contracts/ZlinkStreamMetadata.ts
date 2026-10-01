import { ZlinkStreamErrorCode } from './ZlinkStreamEnums';
import { ZlinkStreamException } from './ZlinkStreamModels';

export interface ZlinkStreamMetadata {
  readonly count: number;
  readonly values: ReadonlyMap<string, string>;
  get(key: string): string | undefined;
  with(key: string, value: string): ZlinkStreamMetadata;
  withMany(values: Iterable<readonly [string, string]>): ZlinkStreamMetadata;
}

export class ZlinkStreamMetadataMap implements ZlinkStreamMetadata {
  static readonly empty: ZlinkStreamMetadata = new ZlinkStreamMetadataMap(new Map());

  private constructor(readonly values: ReadonlyMap<string, string>) {}

  get count(): number {
    return this.values.size;
  }

  get(key: string): string | undefined {
    return this.values.get(key);
  }

  with(key: string, value: string): ZlinkStreamMetadata {
    validateMetadataKey(key);
    const next = new Map(this.values);
    next.set(key, value);
    return new ZlinkStreamMetadataMap(next);
  }

  withMany(values: Iterable<readonly [string, string]>): ZlinkStreamMetadata {
    const next = new Map(this.values);
    for (const [key, value] of values) {
      validateMetadataKey(key);
      next.set(key, value);
    }
    return new ZlinkStreamMetadataMap(next);
  }

  static from(values: Iterable<readonly [string, string]>): ZlinkStreamMetadata {
    return ZlinkStreamMetadataMap.empty.withMany(values);
  }
}

export function validateMetadataKey(key: string): void {
  if (key.length === 0) {
    throw new ZlinkStreamException({
      code: ZlinkStreamErrorCode.ValidationFailed,
      message: 'Metadata key must not be empty.'
    });
  }
}
