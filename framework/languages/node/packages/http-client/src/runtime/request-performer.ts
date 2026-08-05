/* SPDX-License-Identifier: Apache-2.0 */

import { Readable } from 'node:stream';
import { request, type Dispatcher } from 'undici';
import { ZLinkFrameworkException, ZLinkFrameworkErrorKind } from '@zlink-systems/framework';
import type { BodyChunkProvider, DownloadSink, ZLinkHttpMethod } from '../types';
import type { HttpClientOptions } from './options';
import { CookieJar } from './cookie-jar';
import { isRedirectStatus, makeTarget, resolveLocation, rewriteForRedirect } from './redirect-policy';
import { ResponseBodyReader } from './response-body-reader';
import { httpClientUserAgent } from './version';

export interface HttpRequestSpec {
  readonly method: ZLinkHttpMethod;
  readonly target: string;
  readonly body?: string;
  readonly bodyProvider?: BodyChunkProvider;
  readonly headers: Readonly<Record<string, string>>;
  readonly timeoutMs?: number;
  readonly sink?: DownloadSink;
}

export interface RawResult {
  readonly status: number;
  readonly headers: Record<string, string>;
  readonly body: string;
}

/**
 * Performs a single logical request, driving the wrapper-owned redirect loop, cookie jar
 * integration, and same-origin `Authorization` scrubbing. Redirect/URL rules live in
 * `redirect-policy` and response decoding in {@link ResponseBodyReader}. Mirrors the C++
 * `request_performer.cpp`. undici does not auto-redirect, auto-decompress, or keep cookies, so these
 * semantics match the ZLink contract.
 */
export class RequestPerformer {
  private readonly bodyReader: ResponseBodyReader;

  constructor(
    private readonly options: HttpClientOptions,
    private readonly cookieJar: CookieJar,
    private readonly dispatcher: Dispatcher | undefined,
  ) {
    this.bodyReader = new ResponseBodyReader(options);
  }

  async perform(spec: HttpRequestSpec, signal: AbortSignal): Promise<RawResult> {
    const baseUri = new URL(this.options.baseUrl);
    const origin = baseUri.origin;
    let current = new URL(origin + makeTarget(baseUri.pathname, spec.target));
    let method = spec.method;
    let body = spec.body;
    // A streamed body provider cannot be rewound, so it is dropped once a redirect is followed.
    let bodyProvider = spec.bodyProvider;
    let redirectsLeft = this.options.followRedirects;

    for (;;) {
      const keepAuthorization = current.origin === origin;
      const hasBody = body !== undefined || bodyProvider !== undefined;
      const headers = this.buildHeaders(spec, current, keepAuthorization, hasBody);
      const response = await request(current.href, {
        method,
        headers,
        body: this.buildBody(body, bodyProvider),
        dispatcher: this.dispatcher,
        maxRedirections: 0,
        signal,
      });

      const status = response.statusCode;
      if (this.options.cookies) {
        const setCookie = response.headers['set-cookie'];
        const values = Array.isArray(setCookie) ? setCookie : setCookie === undefined ? [] : [setCookie];
        for (const value of values) {
          this.cookieJar.store(current.hostname, value);
        }
      }

      const location = headerValue(response.headers, 'location');
      if (this.options.followRedirects > 0 && isRedirectStatus(status) && location !== undefined && location.length > 0) {
        if (redirectsLeft === 0) {
          await drain(response.body);
          throw requestError('HTTP request exceeded the redirect limit');
        }
        redirectsLeft--;
        ({ method, body } = rewriteForRedirect(status, method, body));
        bodyProvider = undefined; // consumed; never replay a non-rewindable stream on a redirect
        await drain(response.body);
        current = resolveLocation(current, location);
        continue;
      }

      const collectedHeaders = ResponseBodyReader.collectHeaders(response.headers);
      if (spec.sink !== undefined) {
        await this.bodyReader.streamToSink(response.body, spec.sink);
        return { status, headers: collectedHeaders, body: '' };
      }

      let bytes = await this.bodyReader.readBuffered(response.body);
      let finalHeaders = collectedHeaders;
      if (this.options.compression) {
        const result = await this.bodyReader.decompress(bytes, finalHeaders);
        bytes = result.body;
        finalHeaders = result.headers;
      }
      // Decode lazily: the UTF-8 conversion of a large body is a main-thread cost that
      // callers who never read the text body (binary consumers) should not pay.
      let text: string | undefined;
      return {
        status,
        headers: finalHeaders,
        get body(): string {
          text ??= bytes.toString('utf8');
          return text;
        },
      };
    }
  }

  private buildHeaders(
    spec: HttpRequestSpec,
    target: URL,
    keepAuthorization: boolean,
    hasBody: boolean,
  ): Record<string, string> {
    const headers: Record<string, string> = {
      'user-agent': httpClientUserAgent,
      accept: 'application/json',
    };
    if (this.options.compression) {
      headers['accept-encoding'] = 'gzip, deflate';
    }
    // Proxy authentication is carried by the ProxyAgent dispatcher (see runtime.ts), not a header,
    // so it is not leaked to the target over a CONNECT tunnel.

    applyHeaders(headers, this.options.headers, keepAuthorization);
    applyHeaders(headers, spec.headers, keepAuthorization);

    if (!hasBody) {
      // A body source dropped by a redirect must not leave stale content-type metadata behind.
      delete headers['content-type'];
    }

    if (this.options.cookies) {
      const cookieHeader = this.cookieJar.headerFor(target.hostname, target.pathname, target.protocol === 'https:');
      if (cookieHeader.length > 0) {
        headers['cookie'] = cookieHeader;
      }
    }
    return headers;
  }

  private buildBody(
    body: string | undefined,
    bodyProvider: BodyChunkProvider | undefined,
  ): string | Readable | undefined {
    if (bodyProvider !== undefined) {
      const provider = bodyProvider;
      return Readable.from(
        (function* () {
          for (let chunk = provider(); chunk !== null; chunk = provider()) {
            if (chunk.length > 0) {
              yield chunk;
            }
          }
        })(),
      );
    }
    return body;
  }
}

function applyHeaders(
  target: Record<string, string>,
  headers: Readonly<Record<string, string>>,
  keepAuthorization: boolean,
): void {
  for (const [name, value] of Object.entries(headers)) {
    const lower = name.toLowerCase();
    if (!keepAuthorization && lower === 'authorization') {
      continue;
    }
    target[lower] = value;
  }
}

function headerValue(
  headers: Record<string, string | string[] | undefined>,
  name: string,
): string | undefined {
  const value = headers[name];
  if (value === undefined) {
    return undefined;
  }
  return Array.isArray(value) ? value[0] : value;
}

async function drain(stream: Readable): Promise<void> {
  for await (const _chunk of stream) {
    // discard
  }
}

function requestError(message: string): ZLinkFrameworkException {
  return new ZLinkFrameworkException(ZLinkFrameworkErrorKind.Unavailable, message);
}
