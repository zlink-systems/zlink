/* SPDX-License-Identifier: Apache-2.0 */

import { readFileSync } from 'node:fs';
import { rootCertificates } from 'node:tls';
import { Agent, ProxyAgent, type Dispatcher } from 'undici';
import type { HttpClientOptions } from './options';

/**
 * Builds the undici dispatcher for the wrapper: a `ProxyAgent` when a proxy is configured (proxy
 * authentication carried as a token), otherwise a TLS `Agent` (trust certificate + mTLS client
 * certificate) or the global dispatcher when no transport customisation is needed. Separated from
 * the runtime so dispatcher construction is independent of request orchestration.
 */
export function createDispatcher(options: HttpClientOptions): Dispatcher | undefined {
  const connect: Record<string, unknown> = {};
  let hasTls = false;
  if (options.trustCertificateFile !== undefined) {
    // Add the custom certificate to the default roots rather than replacing them, so public-CA
    // HTTPS targets keep working when a test/internal trust certificate is configured.
    connect['ca'] = [...rootCertificates, readFileSync(options.trustCertificateFile, 'utf8')];
    hasTls = true;
  }
  if (options.clientCertificate !== undefined) {
    connect['cert'] = readFileSync(options.clientCertificate.certificatePath);
    connect['key'] = readFileSync(options.clientCertificate.keyPath);
    hasTls = true;
  }

  if (options.proxy !== undefined) {
    return new ProxyAgent({
      uri: options.proxy,
      ...(options.proxyAuthorization !== undefined ? { token: options.proxyAuthorization } : {}),
      ...(hasTls ? { requestTls: connect } : {}),
    });
  }
  if (hasTls) {
    return new Agent({ connect });
  }
  return undefined;
}
