/* SPDX-License-Identifier: Apache-2.0 */

import { ZLinkFrameworkException, ZLinkFrameworkErrorKind } from '@zlink-systems/framework';
import { HttpClientRuntime } from './runtime/runtime';
import type { HttpClientOptions } from './runtime/options';
import { basicAuthorization, requireNonBlank, requirePositiveTimeout } from './runtime/text';
import { createZLinkHttpRequestBuilder, ZLinkHttpRequestBuilder } from './request-builder';
import type { ZLinkHttpExecutionScheduler } from './types';

/**
 * ZLink-style fluent HTTP client. Wraps undici behind a builder so transport types never leak into
 * application code. A general HTTP client; the typed-JSON path (`body(dto)` / `async<T>()`) is a
 * convenience layer on top. Mirrors the C++ `zlink::http_client::client_t`.
 */
export class ZLinkHttpClient {
  constructor(
    private readonly runtimeInstance: HttpClientRuntime,
    readonly executionScheduler?: ZLinkHttpExecutionScheduler
  ) {}

  /** @internal */
  get runtime(): HttpClientRuntime {
    return this.runtimeInstance;
  }

  static create(baseUrl?: string): ZLinkHttpClientBuilder {
    const builder = new ZLinkHttpClientBuilder();
    return baseUrl === undefined ? builder : builder.baseUrl(baseUrl);
  }

  get(path: string): ZLinkHttpRequestBuilder {
    return createZLinkHttpRequestBuilder(this, 'GET', path);
  }

  post(path: string): ZLinkHttpRequestBuilder {
    return createZLinkHttpRequestBuilder(this, 'POST', path);
  }

  put(path: string): ZLinkHttpRequestBuilder {
    return createZLinkHttpRequestBuilder(this, 'PUT', path);
  }

  delete(path: string): ZLinkHttpRequestBuilder {
    return createZLinkHttpRequestBuilder(this, 'DELETE', path);
  }

  patch(path: string): ZLinkHttpRequestBuilder {
    return createZLinkHttpRequestBuilder(this, 'PATCH', path);
  }

  head(path: string): ZLinkHttpRequestBuilder {
    return createZLinkHttpRequestBuilder(this, 'HEAD', path);
  }

  options(path: string): ZLinkHttpRequestBuilder {
    return createZLinkHttpRequestBuilder(this, 'OPTIONS', path);
  }

  /** Releases the underlying dispatcher (connection pool). */
  async close(): Promise<void> {
    await this.runtimeInstance.close();
  }
}

/** Fluent builder for {@link ZLinkHttpClient}. Mirrors the C++ `client_builder_t`. */
export class ZLinkHttpClientBuilder {
  private baseUrlValue = '';
  private timeoutMsValue = 3000;
  private maxResponseBodySizeValue = 16 * 1024 * 1024;
  private readonly headersValue: Record<string, string> = {};
  private trustCertificateFileValue: string | undefined;
  private clientCertificateValue: { certificatePath: string; keyPath: string } | undefined;
  private followRedirectsValue = 0;
  private retryAttemptsValue = 0;
  private cookiesValue = false;
  private proxyValue: string | undefined;
  private proxyAuthorizationValue: string | undefined;
  private compressionValue = false;
  private executionSchedulerValue: ZLinkHttpExecutionScheduler | undefined;

  baseUrl(value: string): this {
    requireNonBlank(value, 'HTTP client base_url is required');
    this.baseUrlValue = value;
    return this;
  }

  timeout(milliseconds: number): this {
    requirePositiveTimeout(milliseconds);
    this.timeoutMsValue = milliseconds;
    return this;
  }

  defaultHeader(name: string, value: string): this {
    requireNonBlank(name, 'HTTP client default header name is required');
    this.headersValue[name.toLowerCase()] = value;
    return this;
  }

  basicAuth(user: string, password: string): this {
    requireNonBlank(user, 'HTTP client basic auth user is required');
    this.headersValue['authorization'] = basicAuthorization(user, password);
    return this;
  }

  bearerToken(token: string): this {
    requireNonBlank(token, 'HTTP client bearer token is required');
    this.headersValue['authorization'] = `Bearer ${token}`;
    return this;
  }

  maxResponseBodySize(bytes: number): this {
    if (!(bytes > 0)) {
      throw new ZLinkFrameworkException(
        ZLinkFrameworkErrorKind.ProtocolError,
        'HTTP client max response body size must be greater than zero',
      );
    }
    this.maxResponseBodySizeValue = bytes;
    return this;
  }

  trustCertificateFile(path: string): this {
    requireNonBlank(path, 'HTTP client trust certificate file is required');
    this.trustCertificateFileValue = path;
    return this;
  }

  clientCertificateFile(certificatePath: string, keyPath: string): this {
    requireNonBlank(certificatePath, 'HTTP client certificate file is required');
    requireNonBlank(keyPath, 'HTTP client certificate key file is required');
    this.clientCertificateValue = { certificatePath, keyPath };
    return this;
  }

  followRedirects(maxRedirects = 5): this {
    if (!(maxRedirects > 0)) {
      throw new ZLinkFrameworkException(
        ZLinkFrameworkErrorKind.ProtocolError,
        'HTTP client follow_redirects must be greater than zero',
      );
    }
    this.followRedirectsValue = maxRedirects;
    return this;
  }

  retry(attempts: number): this {
    if (!(attempts > 0)) {
      throw new ZLinkFrameworkException(
        ZLinkFrameworkErrorKind.ProtocolError,
        'HTTP client retry attempts must be greater than zero',
      );
    }
    this.retryAttemptsValue = attempts;
    return this;
  }

  cookies(): this {
    this.cookiesValue = true;
    return this;
  }

  proxy(url: string): this {
    requireNonBlank(url, 'HTTP client proxy url is required');
    if (!url.startsWith('http://')) {
      throw new ZLinkFrameworkException(
        ZLinkFrameworkErrorKind.ProtocolError,
        'HTTP client proxy url must start with http://',
      );
    }
    this.proxyValue = url;
    return this;
  }

  proxyBasicAuth(user: string, password: string): this {
    requireNonBlank(user, 'HTTP client proxy auth user is required');
    this.proxyAuthorizationValue = basicAuthorization(user, password);
    return this;
  }

  compression(): this {
    this.compressionValue = true;
    return this;
  }

  /** Supplies the framework turn scheduler used by server-side terminators. */
  executionScheduler(scheduler: ZLinkHttpExecutionScheduler): this {
    this.executionSchedulerValue = scheduler;
    return this;
  }

  /** @internal Captures a turn for one-shot requests before the client is built. */
  captureExecutionTurn() {
    return this.executionSchedulerValue?.capture();
  }

  build(): ZLinkHttpClient {
    requireNonBlank(this.baseUrlValue, 'HTTP client base_url is required');
    requirePositiveTimeout(this.timeoutMsValue);
    const lower = this.baseUrlValue.toLowerCase();
    if (!lower.startsWith('http://') && !lower.startsWith('https://')) {
      throw new ZLinkFrameworkException(
        ZLinkFrameworkErrorKind.ProtocolError,
        'HTTP client base_url must start with http:// or https://',
      );
    }

    const options: HttpClientOptions = {
      baseUrl: this.baseUrlValue,
      timeoutMs: this.timeoutMsValue,
      maxResponseBodySize: this.maxResponseBodySizeValue,
      headers: { ...this.headersValue },
      ...(this.trustCertificateFileValue !== undefined ? { trustCertificateFile: this.trustCertificateFileValue } : {}),
      ...(this.clientCertificateValue !== undefined ? { clientCertificate: this.clientCertificateValue } : {}),
      followRedirects: this.followRedirectsValue,
      retryAttempts: this.retryAttemptsValue,
      cookies: this.cookiesValue,
      ...(this.proxyValue !== undefined ? { proxy: this.proxyValue } : {}),
      ...(this.proxyAuthorizationValue !== undefined ? { proxyAuthorization: this.proxyAuthorizationValue } : {}),
      compression: this.compressionValue,
    };

    return new ZLinkHttpClient(new HttpClientRuntime(options), this.executionSchedulerValue);
  }

  get(path: string): ZLinkHttpRequestBuilder {
    return new ZLinkHttpRequestBuilder(undefined, 'GET', path, this);
  }

  post(path: string): ZLinkHttpRequestBuilder {
    return new ZLinkHttpRequestBuilder(undefined, 'POST', path, this);
  }

  put(path: string): ZLinkHttpRequestBuilder {
    return new ZLinkHttpRequestBuilder(undefined, 'PUT', path, this);
  }

  delete(path: string): ZLinkHttpRequestBuilder {
    return new ZLinkHttpRequestBuilder(undefined, 'DELETE', path, this);
  }

  patch(path: string): ZLinkHttpRequestBuilder {
    return new ZLinkHttpRequestBuilder(undefined, 'PATCH', path, this);
  }

  head(path: string): ZLinkHttpRequestBuilder {
    return new ZLinkHttpRequestBuilder(undefined, 'HEAD', path, this);
  }

  options(path: string): ZLinkHttpRequestBuilder {
    return new ZLinkHttpRequestBuilder(undefined, 'OPTIONS', path, this);
  }
}
