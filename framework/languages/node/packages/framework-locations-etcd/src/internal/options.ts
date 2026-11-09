import { validateNamespace } from './encoding';
import { createSecureContext } from 'node:tls';
import type { ZLinkEtcdLocationOptions } from '../options';

export const DEFAULT_OPERATION_TIMEOUT_MS = 5000;

export type EtcdOptions = ZLinkEtcdLocationOptions;

function nonempty(value: unknown): value is string {
  return typeof value === 'string' && value.length > 0;
}
function object(value: unknown): boolean {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

export function validateOptions(options: EtcdOptions): URL[] {
  validateNamespace(options.namespace);
  if (!Array.isArray(options.endpoints) || options.endpoints.length === 0)
    throw new TypeError('Endpoints are required');
  const urls = options.endpoints.map((endpoint) => {
    if (!nonempty(endpoint)) throw new TypeError('Invalid endpoint');
    const url = new URL(endpoint);
    if (
      !['http:', 'https:'].includes(url.protocol) ||
      url.hostname.length === 0 ||
      url.username !== '' ||
      url.password !== '' ||
      url.search !== '' ||
      url.hash !== '' ||
      url.pathname !== '/'
    )
      throw new TypeError('Invalid endpoint URL');
    return url;
  });
  if (urls.some((url) => url.protocol !== urls[0].protocol))
    throw new TypeError('Endpoint schemes must match');
  if (options.tls !== undefined) {
    if (!object(options.tls) || urls[0].protocol !== 'https:')
      throw new TypeError('TLS requires HTTPS');
    if (options.tls.trustedCaPem !== undefined && !nonempty(options.tls.trustedCaPem))
      throw new TypeError('Invalid CA');
    const identity = options.tls.clientIdentity;
    if (
      identity !== undefined &&
      (!object(identity) || !nonempty(identity.certificatePem) || !nonempty(identity.keyPem))
    )
      throw new TypeError('Invalid client identity');
    try {
      createSecureContext({
        ca: options.tls.trustedCaPem,
        cert: identity?.certificatePem,
        key: identity?.keyPem
      });
    } catch {
      throw new TypeError('Invalid TLS material');
    }
  }
  if (
    options.credentials !== undefined &&
    (!object(options.credentials) ||
      !nonempty(options.credentials.user) ||
      !nonempty(options.credentials.password))
  )
    throw new TypeError('Invalid credentials');
  let timeout = DEFAULT_OPERATION_TIMEOUT_MS;
  if (options.operationTimeoutMs !== undefined) timeout = options.operationTimeoutMs;
  if (!Number.isFinite(timeout) || timeout <= 0) throw new RangeError('Invalid operation timeout');
  return urls;
}
