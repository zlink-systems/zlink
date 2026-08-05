export enum ZLinkLocationWriteIntent {
  NewClaim = 1,
  Renew = 2,
  Takeover = 3
}

export enum ZLinkLocationWriteStatus {
  Stored = 'stored',
  IgnoredStale = 'ignoredStale',
  RejectedConflict = 'rejectedConflict'
}

export interface ZLinkLocationWriteResult {
  readonly status: ZLinkLocationWriteStatus;
  readonly generation: bigint;
  readonly updatedAt: Date;
}

export interface ZLinkLocationOwnerToken {
  readonly ownerId: string;
  readonly leaseGeneration: bigint;
}

export type ZLinkOwnerLeaseClaimResult =
  | {
      readonly kind: 'claimed';
      readonly token: ZLinkLocationOwnerToken;
      readonly leaseExpiresAt: Date;
      readonly storeNow: Date;
    }
  | { readonly kind: 'conflict' }
  | { readonly kind: 'generationExhausted' };

export type ZLinkOwnerLeaseRenewResult =
  | {
      readonly kind: 'renewed';
      readonly leaseExpiresAt: Date;
      readonly storeNow: Date;
    }
  | { readonly kind: 'stale' };

export type ZLinkOwnerLeaseReleaseResult = 'released' | 'stale';

export type ZLinkOwnerLeaseReadResult =
  | {
      readonly kind: 'found';
      readonly token: ZLinkLocationOwnerToken;
      readonly leaseExpiresAt: Date;
      readonly storeNow: Date;
    }
  | { readonly kind: 'missing' };

export type ZLinkOwnerLeaseRenewed = Extract<
  ZLinkOwnerLeaseRenewResult,
  { readonly kind: 'renewed' }
>;
