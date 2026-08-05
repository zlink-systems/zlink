export interface ZLinkTimer {
  readonly isDisposed: boolean;
  cancel(signal?: AbortSignal): Promise<void>;
  dispose(): Promise<void>;
}
