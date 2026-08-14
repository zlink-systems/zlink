import type { ZLinkAuthoritySnapshot } from '../../contracts/Locations';
import type { ServiceRelocationEnvelope } from './service-relocation-runtime';

export interface ServiceRelocationStaging {
  readonly id: string;
}

export interface ServiceRelocationRestoreOwner<TStaging extends ServiceRelocationStaging> {
  prepare(
    envelope: ServiceRelocationEnvelope,
    signal?: AbortSignal
  ): Promise<TStaging>;
  publish(
    staging: TStaging,
    authority: ZLinkAuthoritySnapshot,
    signal?: AbortSignal
  ): Promise<void>;
  restoreSavedWork(staging: TStaging, signal?: AbortSignal): Promise<void>;
  normalize(
    staging: TStaging,
    authority: ZLinkAuthoritySnapshot,
    signal?: AbortSignal
  ): Promise<void>;
  openAdmission(staging: TStaging, signal?: AbortSignal): Promise<void>;
  abort(staging: TStaging): Promise<void> | void;
}

export class ServiceRelocationPostCommitError extends Error {
  constructor(
    readonly authority: ZLinkAuthoritySnapshot,
    readonly staging: ServiceRelocationStaging,
    readonly cause: unknown
  ) {
    super('Relocation owner committed, but target publication failed.');
    this.name = 'ServiceRelocationPostCommitError';
  }
}
