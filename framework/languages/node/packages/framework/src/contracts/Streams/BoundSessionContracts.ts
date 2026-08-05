export interface ZLinkBoundSession {
  send(message: unknown): ZLinkBoundSessionSendCall;
  disconnect(signal?: AbortSignal): Promise<void>;
}

export interface ZLinkBoundSessionSendCall {
  metadata(key: string, value: string): this;
  submit(signal?: AbortSignal): Promise<void>;
}
