import type { ZLinkAuthoritySnapshot } from '../locations/internal-location-contracts';
import type { ServiceRelocationEnvelope } from './service-relocation-runtime';

export interface ServiceRelocationStaging {
  readonly id: string;
}

export interface ServiceRelocationRestoreOwner<TStaging extends ServiceRelocationStaging> {
  prepare(envelope: ServiceRelocationEnvelope, signal?: AbortSignal): Promise<TStaging>;
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

/**
 * A relocation whose settled authority leaves the host in `Error` with
 * `Blocked/RelocationFailed` (spec 30 §13): the source owner lease ended
 * before its Preserve fence succeeded, or units settled on both the source
 * and the target.
 */
export class ServiceRelocationAuthorityError extends Error {
  constructor(message: string, options?: ErrorOptions) {
    super(message, options);
    this.name = 'ServiceRelocationAuthorityError';
  }
}
