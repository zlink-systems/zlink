/**
 * Runtime-owned port for reporting exceptions raised after dispatch admission.
 * Keeping the port in diagnostics prevents the channel reporter from becoming
 * a dependency of the message-flow tracer.
 */
export interface ZLinkDispatchErrorSink {
  reportRuntimeTaskException(taskName: string, error: unknown): void;
}
