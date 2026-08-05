/* SPDX-License-Identifier: Apache-2.0 */

import { type Dispatcher } from 'undici';
import type { HttpClientOptions } from './options';
import { CookieJar } from './cookie-jar';
import { RequestPerformer, type HttpRequestSpec, type RawResult } from './request-performer';
import { createDispatcher } from './transport-factory';
import { RetryPolicy } from './retry-policy';

/**
 * Wires the undici dispatcher, request performer, and retry policy. Dispatcher construction lives in
 * `transport-factory` and the retry/timeout loop in {@link RetryPolicy}. Submission is the caller's
 * native `Promise`; the event loop is never blocked. Mirrors the C++ `http_client_runtime.cpp`.
 */
export class HttpClientRuntime {
  private readonly cookieJar = new CookieJar();
  private readonly dispatcher: Dispatcher | undefined;
  private readonly performer: RequestPerformer;
  private readonly retryPolicy: RetryPolicy;

  constructor(options: HttpClientOptions) {
    this.dispatcher = createDispatcher(options);
    this.performer = new RequestPerformer(options, this.cookieJar, this.dispatcher);
    this.retryPolicy = new RetryPolicy(options);
  }

  async executeAsync(spec: HttpRequestSpec): Promise<RawResult> {
    return this.retryPolicy.execute(spec, (request, signal) => this.performer.perform(request, signal));
  }

  async close(): Promise<void> {
    if (this.dispatcher !== undefined) {
      await this.dispatcher.close();
    }
  }
}
