/**
 * Internal E2E seam for holding one acquired application-job permit until the
 * handler turn completes. Production hosts do not register this provider.
 */
export const ZLINK_INTERNAL_APPLICATION_JOB_QUEUE_HANDLER_START_GATE =
  Symbol.for('zlink.internal.application-job-queue-handler-start-gate');

export interface ZLinkInternalApplicationJobQueueHandlerStartGate {
  shouldHoldPermitBeforeHandler(): boolean;
}
