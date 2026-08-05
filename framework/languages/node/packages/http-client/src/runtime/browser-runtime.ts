/* SPDX-License-Identifier: Apache-2.0 */

import { ZLinkFrameworkException, ZLinkFrameworkErrorKind } from '@zlink-systems/framework';
import type { HttpClientOptions } from './options';
import type { HttpRequestSpec, RawResult } from './request-performer';
import { isRedirectStatus, makeTarget, resolveLocation, rewriteForRedirect } from './redirect-policy';
import { RetryPolicy } from './retry-policy';

/** Browser transport for the same public client surface used by the Node runtime. */
export class HttpClientRuntime {
  private readonly retryPolicy: RetryPolicy;

  constructor(private readonly options: HttpClientOptions) {
    rejectUnsupportedTransportOptions(options);
    this.retryPolicy = new RetryPolicy(options);
  }

  async executeAsync(spec: HttpRequestSpec): Promise<RawResult> {
    return await this.retryPolicy.execute(spec, (request, signal) => this.perform(request, signal));
  }

  async close(): Promise<void> {}

  private async perform(spec: HttpRequestSpec, signal: AbortSignal): Promise<RawResult> {
    const baseUri = new URL(this.options.baseUrl);
    const origin = baseUri.origin;
    let current = new URL(origin + makeTarget(baseUri.pathname, spec.target));
    let method = spec.method;
    let body = spec.body;
    let bodyProvider = spec.bodyProvider;
    let redirectsLeft = this.options.followRedirects;

    for (;;) {
      const headers = this.buildHeaders(spec, current.origin === origin, body !== undefined || bodyProvider !== undefined);
      const response = await fetch(current, {
        method,
        headers,
        body: body ?? bodyStream(bodyProvider),
        credentials: this.options.cookies ? 'include' : 'same-origin',
        redirect: 'manual',
        signal,
      });

      const location = response.headers.get('location');
      if (this.options.followRedirects > 0 && isRedirectStatus(response.status) && location !== null) {
        if (redirectsLeft === 0) {
          await response.body?.cancel();
          throw requestError('HTTP request exceeded the redirect limit');
        }
        redirectsLeft--;
        ({ method, body } = rewriteForRedirect(response.status, method, body));
        bodyProvider = undefined;
        await response.body?.cancel();
        current = resolveLocation(current, location);
        continue;
      }

      const headersResult = collectHeaders(response.headers);
      if (spec.sink !== undefined) {
        await streamResponse(response, spec.sink, this.options.maxResponseBodySize);
        return { status: response.status, headers: headersResult, body: '' };
      }
      const text = await response.text();
      if (new TextEncoder().encode(text).length > this.options.maxResponseBodySize) {
        throw requestError('HTTP response exceeded the maximum body size');
      }
      return { status: response.status, headers: headersResult, body: text };
    }
  }

  private buildHeaders(
    spec: HttpRequestSpec,
    keepAuthorization: boolean,
    hasBody: boolean,
  ): Record<string, string> {
    const headers: Record<string, string> = { accept: 'application/json' };
    applyHeaders(headers, this.options.headers, keepAuthorization);
    applyHeaders(headers, spec.headers, keepAuthorization);
    if (!hasBody) delete headers['content-type'];
    return headers;
  }
}

function rejectUnsupportedTransportOptions(options: HttpClientOptions): void {
  if (options.trustCertificateFile !== undefined || options.clientCertificate !== undefined || options.proxy !== undefined) {
    throw new ZLinkFrameworkException(
      ZLinkFrameworkErrorKind.ProtocolError,
      'Browser HTTP clients cannot configure certificate files or a transport proxy.',
    );
  }
}

function bodyStream(provider: HttpRequestSpec['bodyProvider']): ReadableStream<Uint8Array> | undefined {
  if (provider === undefined) return undefined;
  return new ReadableStream<Uint8Array>({
    pull(controller) {
      const chunk = provider();
      if (chunk === null) controller.close();
      else controller.enqueue(chunk);
    },
  });
}

async function streamResponse(
  response: Response,
  sink: NonNullable<HttpRequestSpec['sink']>,
  maximumSize: number,
): Promise<void> {
  if (response.body === null) return;
  const reader = response.body.getReader();
  let total = 0;
  for (;;) {
    const result = await reader.read();
    if (result.done) return;
    total += result.value.length;
    if (total > maximumSize) {
      await reader.cancel();
      throw requestError('HTTP response exceeded the maximum body size');
    }
    sink(result.value);
  }
}

function collectHeaders(headers: Headers): Record<string, string> {
  const result: Record<string, string> = {};
  headers.forEach((value, name) => { result[name.toLowerCase()] = value; });
  return result;
}

function applyHeaders(
  target: Record<string, string>,
  source: Readonly<Record<string, string>>,
  keepAuthorization: boolean,
): void {
  for (const [name, value] of Object.entries(source)) {
    const lower = name.toLowerCase();
    if (keepAuthorization || lower !== 'authorization') target[lower] = value;
  }
}

function requestError(message: string): ZLinkFrameworkException {
  return new ZLinkFrameworkException(ZLinkFrameworkErrorKind.Unavailable, message);
}
