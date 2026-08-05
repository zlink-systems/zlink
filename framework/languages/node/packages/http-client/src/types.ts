/* SPDX-License-Identifier: Apache-2.0 */

/** HTTP methods supported by the ZLink HTTP client. */
export type ZLinkHttpMethod =
  | 'GET'
  | 'POST'
  | 'PUT'
  | 'DELETE'
  | 'PATCH'
  | 'HEAD'
  | 'OPTIONS';

/**
 * Raw HTTP response with status, headers, and the buffered body as a string. Response header
 * names are lowercase. Mirrors the C++ `raw_http_response_t`.
 */
export interface RawHttpResponse {
  readonly status: number;
  readonly headers: Readonly<Record<string, string>>;
  readonly body: string;
}

/**
 * Typed HTTP response. `body` is the JSON-decoded payload; `rawBody` keeps the original response
 * text. Mirrors the C++ `http_response_t<T>`.
 */
export interface HttpResponse<T> {
  readonly status: number;
  readonly headers: Readonly<Record<string, string>>;
  readonly body: T;
  readonly rawBody: string;
}

/** Provider for a streamed request body; returns `null` when the body is complete. */
export type BodyChunkProvider = () => Uint8Array | null;

/** Sink for a streamed response download; receives chunks as they arrive (no decompression). */
export type DownloadSink = (chunk: Uint8Array) => void;

/** Completion scheduling seam supplied by a framework server integration. */
export interface ZLinkHttpExecutionTurn {
  yieldPromise<T>(pending: Promise<T>): Promise<T>;
  post(callback: () => void): void;
}

/** Captures the current framework execution turn when an HTTP call is built. */
export interface ZLinkHttpExecutionScheduler {
  capture(): ZLinkHttpExecutionTurn | undefined;
  reportError(error: unknown): void;
}

/** Callback completion path for callers that do not use an awaitable. */
export type ZLinkHttpCallback<T> = (
  error: unknown | undefined,
  response: HttpResponse<T> | undefined
) => void;
