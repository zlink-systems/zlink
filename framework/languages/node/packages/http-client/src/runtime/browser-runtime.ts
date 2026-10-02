/* SPDX-License-Identifier: Apache-2.0 */

import { HttpContentType, HttpHeaderName } from './text';

import { ZLinkFrameworkException, ZLinkFrameworkErrorKind } from '@zlink-systems/framework';
import type { HttpClientOptions } from './options';
import type { HttpRequestSpec, RawResult } from './request-performer';
import {
  isRedirectStatus,
  makeTarget,
  resolveLocation,
  rewriteForRedirect
} from './redirect-policy';
import { HttpFailureStage, mapFailure, RetryPolicy } from './retry-policy';
import { redirectLimitExceeded } from './http-client-errors';
import { readResponseBody } from './bounded-response-body';

/** Browser transport for the same public client surface used by the Node runtime. */
export class HttpClientRuntime {
  private readonly retryPolicy: RetryPolicy;

  constructor(private readonly options: HttpClientOptions) {
    rejectUnsupportedTransportOptions(options);
    this.retryPolicy = new RetryPolicy(options);
  }

  async executeAsync(spec: HttpRequestSpec): Promise<RawResult> {
    return await this.retryPolicy.execute(spec, (request, controller) =>
      this.perform(request, controller)
    );
  }

  async close(): Promise<void> {}

  private async perform(spec: HttpRequestSpec, controller: AbortController): Promise<RawResult> {
    const signal = controller.signal;
    const baseUri = new URL(this.options.baseUrl);
    const origin = baseUri.origin;
    let current = new URL(origin + makeTarget(baseUri.pathname, spec.target));
    let method = spec.method;
    let body = spec.body;
    let bodyProvider = spec.bodyProvider;
    let redirectsLeft = this.options.followRedirects;

    for (;;) {
      const headers = this.buildHeaders(
        spec,
        current.origin === origin,
        body !== undefined || bodyProvider !== undefined
      );
      let response: Response;
      try {
        const init: RequestInit & { duplex: 'half' } = {
          method,
          headers,
          body: body ?? bodyStream(bodyProvider, controller),
          duplex: 'half',
          credentials: this.options.cookies ? 'include' : 'same-origin',
          redirect: 'manual',
          signal
        };
        response = await fetch(current, init);
      } catch (error) {
        throw mapFailure(error, signal.aborted, HttpFailureStage.Transport);
      }

      const location = response.headers.get(HttpHeaderName.Location);
      if (
        this.options.followRedirects > 0 &&
        isRedirectStatus(response.status) &&
        location !== null
      ) {
        if (redirectsLeft === 0) {
          try {
            await response.body?.cancel();
          } catch (error) {
            throw mapFailure(error, signal.aborted, HttpFailureStage.Transport);
          }
          throw redirectLimitExceeded();
        }
        redirectsLeft--;
        ({ method, body } = rewriteForRedirect(response.status, method, body));
        bodyProvider = undefined;
        try {
          await response.body?.cancel();
        } catch (error) {
          throw mapFailure(error, signal.aborted, HttpFailureStage.Transport);
        }
        current = resolveLocation(current, location);
        continue;
      }

      const headersResult = collectHeaders(response.headers);
      if (spec.sink !== undefined) {
        const sink = spec.sink;
        await readResponseBody(
          responseChunks(response, signal),
          this.options.maxResponseBodySize,
          (chunk) => {
            try {
              sink(chunk);
            } catch (error) {
              throw mapFailure(error, signal.aborted, HttpFailureStage.Application);
            }
          }
        );
        return { status: response.status, headers: headersResult, body: '' };
      }
      const decoder = new TextDecoder();
      const text: string[] = [];
      await readResponseBody(
        responseChunks(response, signal),
        this.options.maxResponseBodySize,
        (chunk) => {
          text.push(decoder.decode(chunk, { stream: true }));
        }
      );
      text.push(decoder.decode());
      return { status: response.status, headers: headersResult, body: text.join('') };
    }
  }

  private buildHeaders(
    spec: HttpRequestSpec,
    keepAuthorization: boolean,
    hasBody: boolean
  ): Record<string, string> {
    const headers: Record<string, string> = {
      [HttpHeaderName.Accept]: HttpContentType.Json
    };
    applyHeaders(headers, this.options.headers, keepAuthorization);
    applyHeaders(headers, spec.headers, keepAuthorization);
    if (!hasBody) delete headers[HttpHeaderName.ContentType];
    return headers;
  }
}

function rejectUnsupportedTransportOptions(options: HttpClientOptions): void {
  if (
    options.trustCertificateFile !== undefined ||
    options.clientCertificate !== undefined ||
    options.proxy !== undefined
  ) {
    throw new ZLinkFrameworkException(
      ZLinkFrameworkErrorKind.ProtocolError,
      'Browser HTTP clients cannot configure certificate files or a transport proxy.'
    );
  }
}

function bodyStream(
  provider: HttpRequestSpec['bodyProvider'],
  abortController: AbortController
): ReadableStream<Uint8Array> | undefined {
  if (provider === undefined) return undefined;
  return new ReadableStream<Uint8Array>({
    pull(controller) {
      let chunk: Uint8Array | null;
      try {
        chunk = provider();
      } catch (error) {
        const failure = mapFailure(
          error,
          abortController.signal.aborted,
          HttpFailureStage.Application
        );
        abortController.abort(failure);
        throw failure;
      }
      if (chunk === null) controller.close();
      else controller.enqueue(chunk);
    }
  });
}

async function* responseChunks(response: Response, signal: AbortSignal): AsyncIterable<Uint8Array> {
  if (response.body === null) return;
  const reader = response.body.getReader();
  try {
    for (;;) {
      let result: ReadableStreamReadResult<Uint8Array>;
      try {
        result = await reader.read();
      } catch (error) {
        throw mapFailure(error, signal.aborted, HttpFailureStage.Transport);
      }
      if (result.done) return;
      yield result.value;
    }
  } finally {
    try {
      try {
        await reader.cancel();
      } catch (error) {
        throw mapFailure(error, signal.aborted, HttpFailureStage.Transport);
      }
    } finally {
      reader.releaseLock();
    }
  }
}

function collectHeaders(headers: Headers): Record<string, string> {
  const result: Record<string, string> = {};
  headers.forEach((value, name) => {
    result[name.toLowerCase()] = value;
  });
  return result;
}

function applyHeaders(
  target: Record<string, string>,
  source: Readonly<Record<string, string>>,
  keepAuthorization: boolean
): void {
  for (const [name, value] of Object.entries(source)) {
    const lower = name.toLowerCase();
    if (keepAuthorization || lower !== HttpHeaderName.Authorization) target[lower] = value;
  }
}
