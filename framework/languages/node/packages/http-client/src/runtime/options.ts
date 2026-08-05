/* SPDX-License-Identifier: Apache-2.0 */

/**
 * Immutable snapshot of the client configuration produced by the builder. Mirrors the C++
 * `http_client_options_t`. Transport gaps (cookie jar, redirect loop, compression, retry) are
 * implemented by the wrapper; only matching semantics are delegated to undici.
 */
export interface HttpClientOptions {
  readonly baseUrl: string;
  readonly timeoutMs: number;
  readonly maxResponseBodySize: number;
  readonly headers: Readonly<Record<string, string>>;
  readonly trustCertificateFile?: string;
  readonly clientCertificate?: { readonly certificatePath: string; readonly keyPath: string };
  readonly followRedirects: number;
  readonly retryAttempts: number;
  readonly cookies: boolean;
  readonly proxy?: string;
  readonly proxyAuthorization?: string;
  readonly compression: boolean;
}
