export interface ApplicationJobPermitPort {
  markApplicationQueued(): void;
  /** Transfers retained-ingress cleanup to a detached exact-target handler turn. */
  detachForHandlerTurn?(): void;
  releaseBeforeHandler(): void;
  releaseAfterInternalProcessing(): void;
}

export interface ApplicationJobQueuePort {
  acquire(signal?: AbortSignal): Promise<ApplicationJobPermitPort>;
}
