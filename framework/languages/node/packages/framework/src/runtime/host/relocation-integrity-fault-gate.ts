/** Internal E2E-only fault seam for direct relocation transfer integrity cases. */
export const ZLINK_INTERNAL_RELOCATION_INTEGRITY_FAULT_GATE =
  Symbol.for('zlink.internal.relocation-integrity-fault-gate');

export interface ZLinkInternalRelocationIntegrityFaultGate {
  consumeChecksumMismatch(): boolean;
  consumeIdentityConflict(): boolean;
}
