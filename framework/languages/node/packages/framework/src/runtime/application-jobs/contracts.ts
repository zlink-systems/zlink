export type ApplicationJobOrigin = 'local' | 'remote';

export interface ApplicationJobPermitPort {
  readonly origin: ApplicationJobOrigin;
  readonly detachedForHandlerTurn: boolean;
  markApplicationQueued(): void;
  /** Transfers ingress-record cleanup to a detached exact-target handler turn. */
  detachForHandlerTurn(): void;
  releaseBeforeHandler(): void;
  releaseAfterInternalProcessing(): void;
  /** Returns reusable reservation storage only after its ingress lifetime ends. */
  releaseStorage?(): void;
}

export interface ApplicationJobReceiveRotationPort {
  reserve(): void;
  take(): ApplicationJobPermitPort | undefined;
  releaseUnused(): void;
}

export interface ApplicationJobQueuePort {
  createReceiveRotation(
    capacity: number,
    origin: ApplicationJobOrigin
  ): ApplicationJobReceiveRotationPort;
  acquire(
    signal: AbortSignal | undefined,
    origin: ApplicationJobOrigin
  ): Promise<ApplicationJobPermitPort>;
  /** Returns a grant now, or registers one FIFO waiter that later calls resume. */
  acquireOrResume(
    resume: (permit: ApplicationJobPermitPort) => void,
    reject: (reason: unknown) => void,
    signal: AbortSignal | undefined,
    origin: ApplicationJobOrigin
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
