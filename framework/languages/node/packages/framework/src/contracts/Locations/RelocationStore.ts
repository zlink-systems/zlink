declare const zlinkBlobReferenceBrand: unique symbol;

export interface ZLinkBlobReference {
  readonly value: string;
  readonly [zlinkBlobReferenceBrand]: true;
}

export type ZLinkBlobPutResult =
  | {
      readonly kind: 'stored' | 'alreadyStored';
      readonly expiresAt: Date;
      readonly storeNow: Date;
    }
  | { readonly kind: 'conflict'; readonly storeNow: Date };

export type ZLinkBlobReadResult =
  | { readonly kind: 'missing'; readonly storeNow: Date }
  | {
      readonly kind: 'found';
      readonly bytes: Uint8Array;
      readonly expiresAt: Date;
      readonly storeNow: Date;
    };

export type ZLinkBlobRenewResult =
  | { readonly kind: 'missing'; readonly storeNow: Date }
  | {
      readonly kind: 'renewed';
      readonly expiresAt: Date;
      readonly storeNow: Date;
    };

export interface ZLinkRelocationStore {
  put(
    reference: ZLinkBlobReference,
    payload: Uint8Array,
    retentionMs: number,
    signal?: AbortSignal
  ): Promise<ZLinkBlobPutResult>;
  read(
    reference: ZLinkBlobReference,
    signal?: AbortSignal
  ): Promise<ZLinkBlobReadResult>;
  renew(
    reference: ZLinkBlobReference,
    retentionMs: number,
    signal?: AbortSignal
  ): Promise<ZLinkBlobRenewResult>;
  delete(reference: ZLinkBlobReference, signal?: AbortSignal): Promise<void>;
  dispose?(): void | Promise<void>;
}
