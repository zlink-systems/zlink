export interface ApplicationJobPermitPort {
  markApplicationQueued(): void;
  /** Transfers ingress-record cleanup to a detached exact-target handler turn. */
  detachForHandlerTurn?(): void;
  releaseBeforeHandler(): void;
  releaseAfterInternalProcessing(): void;
}

export interface ApplicationJobQueuePort {
  acquire(signal?: AbortSignal): Promise<ApplicationJobPermitPort>;
  /** Returns a grant now, or registers one FIFO waiter that later calls resume. */
  acquireOrResume(
    resume: (permit: ApplicationJobPermitPort) => void,
    reject: (reason: unknown) => void,
    signal?: AbortSignal
  ): ApplicationJobPermitPort | undefined;
  registerReceiveFlowTarget?(
    identity: object,
    applyState: (state: 'running' | 'paused') => void,
    failureSink?: (error: unknown) => void
  ): boolean;
  unregisterReceiveFlowTarget?(identity: object): void;
  pressureState?(): 'running' | 'paused';
  pressureTransition?(): {
    readonly state: 'running' | 'paused';
    readonly sequence: bigint;
  };
  onPressureStateChange?(
    listener: (state: 'running' | 'paused', sequence: bigint) => void
  ): () => void;
  recordFlowStateConfigFailure?(): void;
}
