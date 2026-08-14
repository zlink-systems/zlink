import type { ZLinkLocationOptionValues } from '../RouteMesh';

export interface ZLinkLocationOptions {
  ownerLeaseRenewIntervalMs(value: number): this;
  ownerLeaseTtlMs(value: number): this;
  pollingIntervalMs(value: number): this;
  storeFailureGraceMs(value: number): this;
  ownerLeaseFencingMarginMs(value: number): this;
  ownerLeaseRenewTimeoutMs(value: number): this;
  routeCacheMaxAgeMs(value: number): this;
  messageFollowDurationMs(value: number): this;
  sessionRelocationSealTimeoutMs(value: number): this;
}

export type ZLinkLocationOptionOverrides =
  Partial<ZLinkLocationOptionValues> & { readonly listPageSize?: number };

export const zlinkRuntimeDefaultLocationOptions: Readonly<
  ZLinkLocationOptionValues & { readonly listPageSize: number }
> = {
  ownerLeaseRenewIntervalMs: 5000,
  ownerLeaseTtlMs: 15000,
  pollingIntervalMs: 1000,
  listPageSize: 1000,
  storeFailureGraceMs: 30000,
  ownerLeaseFencingMarginMs: 5000,
  ownerLeaseRenewTimeoutMs: 3000,
  routeCacheMaxAgeMs: 15000,
  messageFollowDurationMs: 30000,
  sessionRelocationSealTimeoutMs: 3000
};

export const zlinkDefaultLocationOptions: Readonly<ZLinkLocationOptionValues> =
  zlinkRuntimeDefaultLocationOptions;
