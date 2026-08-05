/* SPDX-License-Identifier: Apache-2.0 */

import { ZLinkFrameworkException, ZLinkFrameworkErrorKind } from '@zlink-systems/framework';
import type { ZLinkHttpClient, ZLinkHttpClientBuilder } from './client';
import type {
  BodyChunkProvider,
  DownloadSink,
  HttpResponse,
  RawHttpResponse,
  ZLinkHttpCallback,
  ZLinkHttpExecutionScheduler,
  ZLinkHttpExecutionTurn,
  ZLinkHttpMethod
} from './types';
import type { HttpRequestSpec } from './runtime/request-performer';
import { makeMultipartBoundary, percentEncode, requireNonBlank, requirePositiveTimeout } from './runtime/text';

interface MultipartPart {
  name: string;
  filename: string;
  content: string;
  contentType: string;
}

/**
 * Fluent builder for a single request. Mirrors the C++ `request_builder_t`. Submission returns a
 * `Promise`; the event loop is never blocked while the request is in flight.
 */
export class ZLinkHttpRequestBuilder {
  private bodyValue: string | undefined;
  private bodyProviderValue: BodyChunkProvider | undefined;
  private readonly headersValue: Record<string, string> = {};
  private timeoutMsValue: number | undefined;
  private readonly queryValue: Array<[string, string]> = [];
  private readonly formValue: Array<[string, string]> = [];
  private readonly multipartValue: MultipartPart[] = [];

  private clientInstance: ZLinkHttpClient | undefined;
  private readonly clientFactory: ZLinkHttpClientBuilder | undefined;
  private readonly ownsClient: boolean;
  protected readonly executionTurn: ZLinkHttpExecutionTurn | undefined;
  protected readonly executionScheduler: ZLinkHttpExecutionScheduler | undefined;
  private consumed = false;

  constructor(
    client: ZLinkHttpClient | undefined,
    private readonly method: ZLinkHttpMethod,
    private readonly path: string,
    clientFactory?: ZLinkHttpClientBuilder,
  ) {
    this.clientInstance = client;
    this.clientFactory = clientFactory;
    // A one-shot request (builder verb shortcut) owns the lazily-built client and closes it once the
    // request completes.
    this.ownsClient = clientFactory !== undefined;
    this.executionScheduler = client?.executionScheduler;
    this.executionTurn = client?.executionScheduler?.capture()
      ?? clientFactory?.captureExecutionTurn();
    if (path.length === 0 || path[0] !== '/') {
      throw new ZLinkFrameworkException(
        ZLinkFrameworkErrorKind.ProtocolError,
        'HTTP request path must start with /',
      );
    }
  }

  // Resolves the client to run on, building a one-shot client lazily. A one-shot request builder is
  // single-use so its lazily-built client is closed exactly once.
  private resolveClient(): ZLinkHttpClient {
    if (this.ownsClient && this.consumed) {
      throw new ZLinkFrameworkException(
        ZLinkFrameworkErrorKind.ProtocolError,
        'A one-shot HTTP request can only be submitted once',
      );
    }
    this.consumed = true;
    if (this.clientInstance === undefined) {
      if (this.clientFactory === undefined) {
        throw new ZLinkFrameworkException(
          ZLinkFrameworkErrorKind.ProtocolError,
          'HTTP request has no client',
        );
      }
      this.clientInstance = this.clientFactory.build();
    }
    return this.clientInstance;
  }

  private async closeIfOwned(): Promise<void> {
    if (this.ownsClient && this.clientInstance !== undefined) {
      // A one-shot cleanup failure must not mask the request result.
      await this.clientInstance.close().catch(() => undefined);
    }
  }

  header(name: string, value: string): this {
    requireNonBlank(name, 'HTTP request header name is required');
    this.headersValue[name.toLowerCase()] = value;
    return this;
  }

  query(name: string, value: string): this {
    requireNonBlank(name, 'HTTP request query name is required');
    this.queryValue.push([name, value]);
    return this;
  }

  timeout(milliseconds: number): this {
    requirePositiveTimeout(milliseconds);
    this.timeoutMsValue = milliseconds;
    return this;
  }

  /** Sets a typed JSON body (1 arg) or a raw body with explicit content type (2 args). */
  body<T>(value: T): this;
  body(content: string, contentType: string): this;
  body(value: unknown, contentType?: string): this {
    if (contentType !== undefined) {
      if (typeof value !== 'string') {
        throw new ZLinkFrameworkException(
          ZLinkFrameworkErrorKind.ProtocolError,
          'HTTP request raw body content is required',
        );
      }
      requireNonBlank(contentType, 'HTTP request body content type is required');
      this.bodyValue = value;
      this.headersValue['content-type'] = contentType;
      return this;
    }
    this.bodyValue = JSON.stringify(value);
    this.headersValue['content-type'] ??= 'application/json';
    return this;
  }

  /**
   * Streams the request body chunk by chunk with chunked transfer-encoding; the provider returns
   * `null` when the body is complete. Streamed requests are excluded from retry.
   */
  bodyStream(provider: BodyChunkProvider, contentType: string): this {
    if (typeof provider !== 'function') {
      throw new ZLinkFrameworkException(
        ZLinkFrameworkErrorKind.ProtocolError,
        'HTTP request body stream provider is required',
      );
    }
    requireNonBlank(contentType, 'HTTP request body content type is required');
    this.bodyProviderValue = provider;
    this.headersValue['content-type'] = contentType;
    return this;
  }

  form(name: string, value: string): this {
    requireNonBlank(name, 'HTTP request form field name is required');
    this.formValue.push([name, value]);
    return this;
  }

  multipart(name: string, value: string): this {
    requireNonBlank(name, 'HTTP request multipart field name is required');
    this.multipartValue.push({ name, filename: '', content: value, contentType: '' });
    return this;
  }

  multipartFile(name: string, filename: string, content: string, contentType: string): this {
    requireNonBlank(name, 'HTTP request multipart field name is required');
    requireNonBlank(filename, 'HTTP request multipart filename is required');
    requireNonBlank(contentType, 'HTTP request multipart content type is required');
    this.multipartValue.push({ name, filename, content, contentType });
    return this;
  }

  /** Executes the request and returns the raw response while retaining the current turn. */
  async submitRaw(): Promise<RawHttpResponse> {
    const request = this.makeRequest(undefined);
    const client = this.resolveClient();
    try {
      return await client.runtime.executeAsync(request);
    } finally {
      await this.closeIfOwned();
    }
  }

  /**
   * Streams the response body to `sink` chunk by chunk instead of buffering it; the returned
   * response carries status and headers with an empty body (no decompression of chunks).
   */
  async download(sink: DownloadSink): Promise<RawHttpResponse> {
    if (typeof sink !== 'function') {
      throw new ZLinkFrameworkException(
        ZLinkFrameworkErrorKind.ProtocolError,
        'HTTP request download sink is required',
      );
    }
    const request = this.makeRequest(sink);
    const client = this.resolveClient();
    try {
      return await client.runtime.executeAsync(request);
    } finally {
      await this.closeIfOwned();
    }
  }

  async async<T>(): Promise<HttpResponse<T>>;
  async<T>(callback: ZLinkHttpCallback<T>): void;
  async<T>(callback?: ZLinkHttpCallback<T>): Promise<HttpResponse<T>> | void {
    const pending = this.executeTyped<T>();
    if (callback === undefined) {
      return pending;
    }
    void pending.then(
      (response) => this.completeCallback(() => callback(undefined, response)),
      (error) => this.completeCallback(() => callback(error, undefined))
    );
  }

  protected async executeTyped<T>(): Promise<HttpResponse<T>> {
    const raw = await this.submitRaw();
    if (raw.status >= 400) {
      throw new ZLinkFrameworkException(
        ZLinkFrameworkErrorKind.Unavailable,
        `HTTP request failed with status ${raw.status}`,
      );
    }
    let body: T;
    if (raw.body.length === 0) {
      body = null as T;
    } else {
      try {
        body = JSON.parse(raw.body, safeJsonReviver) as T;
      } catch (cause) {
        throw new ZLinkFrameworkException(
          ZLinkFrameworkErrorKind.ProtocolError,
          cause instanceof Error ? cause.message : 'HTTP response body decode failed',
          cause,
        );
      }
    }
    return { status: raw.status, headers: raw.headers, body, rawBody: raw.body };
  }

  /** Returns only the decoded body for client-side scenarios that do not need the HTTP envelope. */
  async fetch<T>(): Promise<T> {
    return (await this.executeTyped<T>()).body;
  }

  private completeCallback(callback: () => void): void {
    if (this.executionTurn === undefined) {
      callback();
      return;
    }
    this.executionTurn.post(callback);
  }

  private makeRequest(sink: DownloadSink | undefined): HttpRequestSpec {
    const { body, headers } = this.resolveBodyAndHeaders();
    return {
      method: this.method,
      target: this.resolveTarget(),
      ...(body !== undefined ? { body } : {}),
      ...(this.bodyProviderValue !== undefined ? { bodyProvider: this.bodyProviderValue } : {}),
      headers,
      ...(this.timeoutMsValue !== undefined ? { timeoutMs: this.timeoutMsValue } : {}),
      ...(sink !== undefined ? { sink } : {}),
    };
  }

  private resolveTarget(): string {
    if (this.queryValue.length === 0) {
      return this.path;
    }
    let target = this.path;
    let separator = this.path.includes('?') ? '&' : '?';
    for (const [name, value] of this.queryValue) {
      target += `${separator}${percentEncode(name)}=${percentEncode(value)}`;
      separator = '&';
    }
    return target;
  }

  private resolveBodyAndHeaders(): { body: string | undefined; headers: Record<string, string> } {
    if (this.countBodySources() > 1) {
      throw new ZLinkFrameworkException(
        ZLinkFrameworkErrorKind.ProtocolError,
        'HTTP request accepts a single body source: body, body_stream, form, or multipart',
      );
    }

    const headers: Record<string, string> = { ...this.headersValue };
    if (this.bodyValue !== undefined) {
      return { body: this.bodyValue, headers };
    }

    if (this.formValue.length > 0) {
      headers['content-type'] = 'application/x-www-form-urlencoded';
      return { body: this.encodeFormBody(), headers };
    }

    if (this.multipartValue.length > 0) {
      const boundary = makeMultipartBoundary();
      headers['content-type'] = `multipart/form-data; boundary=${boundary}`;
      return { body: this.encodeMultipartBody(boundary), headers };
    }

    return { body: undefined, headers };
  }

  private countBodySources(): number {
    return (
      (this.bodyValue !== undefined ? 1 : 0) +
      (this.bodyProviderValue !== undefined ? 1 : 0) +
      (this.formValue.length > 0 ? 1 : 0) +
      (this.multipartValue.length > 0 ? 1 : 0)
    );
  }

  private encodeFormBody(): string {
    return this.formValue
      .map(([name, value]) => `${percentEncode(name)}=${percentEncode(value)}`)
      .join('&');
  }

  private encodeMultipartBody(boundary: string): string {
    let encoded = '';
    for (const part of this.multipartValue) {
      encoded += `--${boundary}\r\n`;
      encoded += `Content-Disposition: form-data; name="${part.name}"`;
      if (part.filename.length > 0) {
        encoded += `; filename="${part.filename}"`;
      }
      encoded += '\r\n';
      if (part.contentType.length > 0) {
        encoded += `Content-Type: ${part.contentType}\r\n`;
      }
      encoded += '\r\n';
      encoded += part.content;
      encoded += '\r\n';
    }
    encoded += `--${boundary}--\r\n`;
    return encoded;
  }
}

class ZLinkFrameworkHttpRequestBuilder extends ZLinkHttpRequestBuilder {
  /** Starts a server-side one-way request and ignores its response body. */
  async submit(): Promise<void> {
    if (this.executionScheduler === undefined) {
      throw new ZLinkFrameworkException(
        ZLinkFrameworkErrorKind.ProtocolError,
        'HTTP submit requires a framework server client'
      );
    }
    await this.submitRaw();
  }

  /** Executes a typed request while yielding the current Spot turn. */
  yield<T>(): Promise<HttpResponse<T>> {
    if (this.executionTurn === undefined) {
      return Promise.reject(new ZLinkFrameworkException(
        ZLinkFrameworkErrorKind.ProtocolError,
        'HTTP yield requires a framework Spot turn'
      ));
    }
    return this.executionTurn.yieldPromise(this.executeTyped<T>());
  }
}

export function createZLinkHttpRequestBuilder(
  client: ZLinkHttpClient,
  method: ZLinkHttpMethod,
  path: string
): ZLinkHttpRequestBuilder {
  return client.executionScheduler === undefined
    ? new ZLinkHttpRequestBuilder(client, method, path)
    : new ZLinkFrameworkHttpRequestBuilder(client, method, path);
}

// Prototype-pollution guard, mirroring the framework's stream JSON codec.
const prototypeKeys = new Set(['__proto__', 'constructor', 'prototype']);

function safeJsonReviver(key: string, value: unknown): unknown {
  if (prototypeKeys.has(key)) {
    return undefined;
  }
  return value;
}
