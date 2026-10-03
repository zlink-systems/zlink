/* SPDX-License-Identifier: Apache-2.0 */

import { ZLinkFrameworkException, ZLinkFrameworkErrorKind } from '@zlink-systems/framework';
import type { HttpClientOptions } from './options';
import type { HttpRequestSpec, RawResult } from './request-performer';

const INITIAL_RETRY_DELAY_MS = 50;
const MAX_RETRY_DELAY_MS = 1000;
const MAX_RETRY_DELAY_SHIFT = Math.ceil(Math.log2(MAX_RETRY_DELAY_MS / INITIAL_RETRY_DELAY_MS));

// Exponential backoff with full jitter: base 50ms, doubling per attempt, capped at 1s.
// Fixed delays synchronize retries from many clients against an ailing server.
function delayMsFor(attempt: number): number {
  const ceiling = Math.min(
    MAX_RETRY_DELAY_MS,
    INITIAL_RETRY_DELAY_MS << Math.min(attempt, MAX_RETRY_DELAY_SHIFT)
  );
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
    perform: (spec: HttpRequestSpec, controller: AbortController) => Promise<RawResult>
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
        return await perform(spec, controller);
      } catch (error) {
        const failure = mapFailure(error, controller.signal.aborted, HttpFailureStage.Application);
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

export enum HttpFailureStage {
  Transport,
  Application
}

export function mapFailure(
  error: unknown,
  aborted: boolean,
  stage: HttpFailureStage
): ZLinkFrameworkException {
  if (error instanceof ZLinkFrameworkException) {
    return error;
  }
  if (stage === HttpFailureStage.Transport && aborted) {
    const cause = new DOMException('HTTP request exceeded timeout', 'TimeoutError');
    return new ZLinkFrameworkException(
      ZLinkFrameworkErrorKind.DeadlineExceeded,
      cause.message,
      cause
    );
  }
  const message = error instanceof Error ? error.message : 'HTTP execution failure';
  return new ZLinkFrameworkException(
    stage === HttpFailureStage.Transport
      ? ZLinkFrameworkErrorKind.Unavailable
      : ZLinkFrameworkErrorKind.InternalFailure,
    message,
    error
  );
}

function isRetriableHttpFailure(error: ZLinkFrameworkException): boolean {
  return (
    error.kind === ZLinkFrameworkErrorKind.Unavailable ||
    error.kind === ZLinkFrameworkErrorKind.DeadlineExceeded
  );
}

function delay(ms: number): Promise<void> {
  return new Promise((resolve) => {
    setTimeout(resolve, ms);
  });
}
