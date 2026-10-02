export const ZLINK_PROVIDER_MAX_PAGE_SIZE = 1000;
export const ZLINK_PROVIDER_MAX_KEY_BYTES = 1024;
export const ZLINK_PROVIDER_MAX_VALUE_BYTES = 1024 * 1024;
export const ZLINK_PROVIDER_MAX_VERSION_BYTES = 4096;
export const ZLINK_PROVIDER_MAX_WRITE_KEYS = 2048;
export const ZLINK_PROVIDER_MAX_WRITE_BYTES = 4 * 1024 * 1024;
export const ZLINK_PROVIDER_MAX_ENCODED_PAGE_BYTES = 4 * 1024 * 1024;
export const ZLINK_PROVIDER_MAX_SCAN_CURSOR_BYTES = 4096;

declare const zlinkStoreKeyBrand: unique symbol;
declare const zlinkStoreVersionBrand: unique symbol;
declare const zlinkStoreScanCursorBrand: unique symbol;

export interface ZLinkStoreKey {
  readonly value: string;
  readonly [zlinkStoreKeyBrand]: true;
}

export interface ZLinkStoreVersion {
  readonly value: string;
  readonly [zlinkStoreVersionBrand]: true;
}

export interface ZLinkStoreScanCursor {
  readonly value: string;
  readonly [zlinkStoreScanCursorBrand]: true;
}

export interface ZLinkStoreValue {
  readonly bytes: Uint8Array;
  readonly version: ZLinkStoreVersion;
  readonly expiresAt?: Date;
  readonly storeNow: Date;
}

export type ZLinkStoreReadResult =
  | { readonly kind: 'missing'; readonly storeNow: Date }
  | { readonly kind: 'found'; readonly value: ZLinkStoreValue };

export type ZLinkStoreCondition =
  | { readonly kind: 'missing'; readonly key: ZLinkStoreKey }
  | {
      readonly kind: 'version';
      readonly key: ZLinkStoreKey;
      readonly expected: ZLinkStoreVersion;
    }
  | {
      readonly kind: 'value';
      readonly key: ZLinkStoreKey;
      readonly expected: Uint8Array;
    };

export type ZLinkStoreMutation =
  | {
      readonly kind: 'put';
      readonly key: ZLinkStoreKey;
      readonly bytes: Uint8Array;
      readonly retentionMs?: number;
    }
  | { readonly kind: 'delete'; readonly key: ZLinkStoreKey };

export interface ZLinkStoreWriteRequest {
  readonly conditions: readonly ZLinkStoreCondition[];
  readonly mutations: readonly ZLinkStoreMutation[];
}

export interface ZLinkStorePutVersion {
  readonly key: ZLinkStoreKey;
  readonly version: ZLinkStoreVersion;
}

export type ZLinkStoreWriteResult =
  | {
      readonly kind: 'applied';
      readonly putVersions: readonly ZLinkStorePutVersion[];
      readonly storeNow: Date;
    }
  | { readonly kind: 'conflict'; readonly storeNow: Date };

export interface ZLinkStoreScanRequest {
  readonly prefix: string;
  readonly cursor?: ZLinkStoreScanCursor;
  readonly limit: number;
}

export interface ZLinkStoreScanItem {
  readonly key: ZLinkStoreKey;
  readonly value: ZLinkStoreValue;
}

export interface ZLinkStoreScanPage {
  readonly items: readonly ZLinkStoreScanItem[];
  readonly nextCursor?: ZLinkStoreScanCursor;
  readonly storeNow: Date;
}

export type ZLinkStoreScanResult =
  { readonly kind: 'page'; readonly value: ZLinkStoreScanPage } | { readonly kind: 'expired' };

export interface ZLinkLocationStore {
  read(key: ZLinkStoreKey, signal?: AbortSignal): Promise<ZLinkStoreReadResult>;
  write(request: ZLinkStoreWriteRequest, signal?: AbortSignal): Promise<ZLinkStoreWriteResult>;
  scan(request: ZLinkStoreScanRequest, signal?: AbortSignal): Promise<ZLinkStoreScanResult>;
  dispose?(): void | Promise<void>;
}
