/* SPDX-License-Identifier: Apache-2.0 */

import { ZLinkFrameworkException, ZLinkFrameworkErrorKind } from '@zlink-systems/framework';
import type { HttpClientOptions } from './options';
import type { HttpRequestSpec, RawResult } from './request-performer';

// Exponential backoff with full jitter: base 50ms, doubling per attempt, capped at 1s.
// Fixed delays synchronize retries from many clients against an ailing server.
function delayMsFor(attempt: number): number {
  const ceiling = Math.min(1000, 50 << Math.min(attempt, 5));
  return Math.floor(Math.random() * (ceiling + 1));
}

/**
 * Applies the wrapper's retry policy around a single request attempt: streaming requests are never
 * retried (they cannot be rewound), each attempt is bounded by the effective timeout via an
 * `AbortController`, and only retriable transport failures (timeout, connection errors) are retried
 * at a fixed delay. Separated from the runtime so the retry/timeout policy is independent of
 * dispatcher construction.
 */
export class RetryPolicy {
  constructor(private readonly options: HttpClientOptions) {}

  async execute(
    spec: HttpRequestSpec,
    perform: (spec: HttpRequestSpec, signal: AbortSignal) => Promise<RawResult>,
  ): Promise<RawResult> {
    const maxRetries =
      spec.sink !== undefined || spec.bodyProvider !== undefined ? 0 : this.options.retryAttempts;
    const timeoutMs = spec.timeoutMs ?? this.options.timeoutMs;

    for (let attempt = 0; ; attempt++) {
      const controller = new AbortController();
      const timer = setTimeout(() => {
        controller.abort();
      }, timeoutMs);
      try {
        return await perform(spec, controller.signal);
      } catch (error) {
        const failure = mapFailure(error, controller.signal.aborted);
        if (isRetriableHttpFailure(failure) && attempt < maxRetries) {
          await delay(delayMsFor(attempt));
          continue;
        }
        throw failure;
      } finally {
        clearTimeout(timer);
      }
    }
  }
}

function mapFailure(error: unknown, aborted: boolean): ZLinkFrameworkException {
  if (error instanceof ZLinkFrameworkException) {
    return error;
  }
  if (aborted || (error instanceof Error && error.name === 'AbortError')) {
    return new ZLinkFrameworkException(
      ZLinkFrameworkErrorKind.DeadlineExceeded,
      'HTTP request exceeded timeout',
      error,
    );
  }
  // Transport failures (connection refused/reset, undici errors) are retriable.
  const message = error instanceof Error ? error.message : 'HTTP transport failure';
  return new ZLinkFrameworkException(ZLinkFrameworkErrorKind.Unavailable, message, error);
}

function isRetriableHttpFailure(error: ZLinkFrameworkException): boolean {
  return error.kind === ZLinkFrameworkErrorKind.Unavailable
    || error.kind === ZLinkFrameworkErrorKind.DeadlineExceeded;
}

function delay(ms: number): Promise<void> {
  return new Promise((resolve) => {
    setTimeout(resolve, ms);
  });
}
