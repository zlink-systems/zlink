/**
 * Deterministic endpoint-notation normalization (endpoint 표기 정책,
 * doc/plan/endpoint-notation-policy.ko.md).
 *
 * `new URL()` cannot represent this domain: it throws on bracketed IPv6
 * literals that carry a zone id (`tcp://[fe80::1%eth0]:80`, even with the
 * zone id percent-escaped as `%25eth0`), and for non-special schemes like
 * `tcp://`/`tls://` it does not lowercase the host and it re-encodes
 * userinfo/query, which would violate the losslessness requirement below.
 * This module parses the `scheme://[userinfo@]host[:port][/path][?query][#fragment]`
 * grammar directly, bracket-aware, so it never resorts to `lastIndexOf(':')`
 * to split host from port (that breaks on IPv6 literals).
 *
 * Normalization is applied once, at the point an endpoint is constructed or
 * accepted from outside the process (config, wire, Location Store row,
 * public connect/disconnect APIs). Everything downstream compares the
 * resulting strings with plain `===`.
 *
 * Rules (policy 2.2), applied only to authority-bearing schemes
 * (`tcp`, `tls`, `ws`, `wss` -- schemes whose grammar after `scheme://` is
 * `host:port`):
 *  - scheme: lowercased
 *  - host: lowercased (IPv6 zone id is preserved verbatim, not lowercased --
 *    interface names are case sensitive on Linux)
 *  - IPv6 host: unified to bracketed form, zone id preserved
 *  - port: decimal, leading zeros stripped
 *  - path: trailing slash(es) stripped
 *  - surrounding whitespace stripped
 *
 * Schemes without an authority -- `ipc://` is the one in use here, where
 * what follows `scheme://` is a filesystem path, not a host -- only get
 * scheme lowercasing and whitespace trimming. A path can be case sensitive
 * and a trailing slash can be meaningful, so applying the host rules would
 * change what the endpoint points at. This matches the C++ implementation
 * (framework/languages/cpp/framework/src/runtime/transport/endpoint_notation.hpp).
 *
 * Normalization is lossless: userInfo, query, fragment and IPv6 zone id are
 * preserved exactly as given. It is also idempotent:
 * `normalizeEndpoint(normalizeEndpoint(x)) === normalizeEndpoint(x)`.
 *
 * DNS names are never resolved. `localhost` and `127.0.0.1` remain distinct
 * endpoints (policy 2.1).
 */

const SCHEME_PATTERN = /^([A-Za-z][A-Za-z0-9+.-]*):\/\/(.*)$/s;

const AUTHORITY_SCHEMES = new Set(['tcp', 'tls', 'ws', 'wss']);

function isAuthorityScheme(schemeLower: string): boolean {
  return AUTHORITY_SCHEMES.has(schemeLower);
}

export function normalizeEndpoint(endpoint: string): string {
  const trimmed = endpoint.trim();
  const schemeMatch = SCHEME_PATTERN.exec(trimmed);
  if (schemeMatch === null) {
    // Not a `scheme://...` endpoint (e.g. malformed input). Nothing in the
    // policy defines normalization outside this grammar; leave it as-is
    // (whitespace already stripped) rather than guessing.
    return trimmed;
  }
  const scheme = schemeMatch[1].toLowerCase();
  const rest = schemeMatch[2];

  if (!isAuthorityScheme(scheme)) {
    // Non-authority scheme (e.g. "ipc" -- a filesystem path follows, not a
    // host) or an unknown scheme: only the scheme casing is a safe,
    // lossless normalization. The remainder is opaque and returned
    // byte-for-byte.
    return `${scheme}://${rest}`;
  }

  const authorityEnd = findAuthorityEnd(rest);
  const authority = rest.slice(0, authorityEnd);
  const suffix = rest.slice(authorityEnd);

  const atIndex = authority.lastIndexOf('@');
  const userInfo = atIndex >= 0 ? authority.slice(0, atIndex + 1) : '';
  const hostPort = atIndex >= 0 ? authority.slice(atIndex + 1) : authority;

  const { host, port } = splitHostPort(hostPort);
  const normalizedHost = normalizeHost(host);
  const normalizedPort = normalizePort(port);

  return `${scheme}://${userInfo}${normalizedHost}`
    + (normalizedPort.length > 0 ? `:${normalizedPort}` : '')
    + normalizeSuffix(suffix);
}

function findAuthorityEnd(rest: string): number {
  if (rest.startsWith('[')) {
    const closeBracket = rest.indexOf(']');
    if (closeBracket === -1) {
      // Malformed (unterminated bracket); treat the remainder as authority.
      return rest.length;
    }
    const tail = rest.slice(closeBracket);
    const delimiter = /[/?#]/.exec(tail);
    return delimiter === null ? rest.length : closeBracket + delimiter.index;
  }
  const delimiter = /[/?#]/.exec(rest);
  return delimiter === null ? rest.length : delimiter.index;
}

function splitHostPort(hostPort: string): { host: string; port: string } {
  if (hostPort.startsWith('[')) {
    const closeBracket = hostPort.indexOf(']');
    if (closeBracket === -1) {
      return { host: hostPort, port: '' };
    }
    const host = hostPort.slice(0, closeBracket + 1);
    const afterBracket = hostPort.slice(closeBracket + 1);
    const port = afterBracket.startsWith(':') ? afterBracket.slice(1) : '';
    return { host, port };
  }
  const colonCount = (hostPort.match(/:/g) ?? []).length;
  if (colonCount >= 2) {
    // Unbracketed IPv6 literal (no port present, since a bracketed form is
    // otherwise required to attach a port unambiguously). Unify to bracketed
    // form -- never split on the "last colon" here, that corrupts IPv6.
    return { host: `[${hostPort}]`, port: '' };
  }
  const colonIndex = hostPort.indexOf(':');
  if (colonIndex === -1) {
    return { host: hostPort, port: '' };
  }
  return { host: hostPort.slice(0, colonIndex), port: hostPort.slice(colonIndex + 1) };
}

function normalizeHost(host: string): string {
  if (host.startsWith('[') && host.endsWith(']')) {
    const inner = host.slice(1, -1);
    const zoneIndex = inner.indexOf('%');
    if (zoneIndex === -1) {
      return `[${inner.toLowerCase()}]`;
    }
    const address = inner.slice(0, zoneIndex).toLowerCase();
    const zone = inner.slice(zoneIndex);
    return `[${address}${zone}]`;
  }
  return host.toLowerCase();
}

function normalizePort(port: string): string {
  if (!/^\d+$/.test(port)) {
    // Not a plain decimal port (empty, or something we shouldn't guess
    // about); leave verbatim rather than risk corrupting it.
    return port;
  }
  const stripped = port.replace(/^0+(?=\d)/, '');
  return stripped;
}

/**
 * Extracts `{ host, port }` from a `scheme://[userinfo@]host[:port]...`
 * endpoint whose scheme (case-insensitively) matches `expectedScheme`.
 * `host` has IPv6 brackets stripped (zone id, if any, stays attached).
 * `port` is `0` when absent. Returns `undefined` when the scheme doesn't
 * match or the endpoint isn't parseable.
 *
 * IPv6-safe by construction (bracket-aware, never `lastIndexOf(':')`) --
 * this replaces `new URL()`-based host/port extraction, which throws on a
 * zone id (even percent-escaped) and so silently produced `undefined`/`0`
 * for such endpoints.
 */
export function parseEndpointHostPort(
  endpoint: string,
  expectedScheme: string
): { host: string; port: number } | undefined {
  const schemeMatch = SCHEME_PATTERN.exec(endpoint.trim());
  if (schemeMatch === null) return undefined;
  if (schemeMatch[1].toLowerCase() !== expectedScheme) return undefined;
  const rest = schemeMatch[2];
  const authorityEnd = findAuthorityEnd(rest);
  const authority = rest.slice(0, authorityEnd);
  const atIndex = authority.lastIndexOf('@');
  const hostPort = atIndex >= 0 ? authority.slice(atIndex + 1) : authority;
  const { host, port } = splitHostPort(hostPort);
  const bareHost = host.startsWith('[') && host.endsWith(']') ? host.slice(1, -1) : host;
  return {
    host: bareHost,
    port: /^\d+$/.test(port) ? Number(port) : 0
  };
}

const ADVERTISE_SOURCE_PATTERN = /^([A-Za-z][A-Za-z0-9+.-]*):\/\/(?:\[[^\]]+\]|[^:]+):(\d+)$/;

/**
 * Builds a normalized `scheme://advertiseHost:port` endpoint from a bound
 * endpoint, replacing only the host. Consolidates what used to be three
 * near-identical, independently drifted implementations
 * (channel-socket-registry, fanout-location-runtime, spot-node-runtime-manager)
 * -- all matched via the same regex shape, but only one of the three
 * accepted non-`tcp` schemes or was scheme-case-insensitive.
 *
 * Returns `undefined` when `boundEndpoint` is not a `scheme://host:port`
 * endpoint, or (when `restrictToScheme` is given) when its scheme doesn't
 * match after lowercasing -- callers translate that into their own
 * capability-specific `ZLinkConfigurationException` message. Returns
 * `boundEndpoint` normalized (unrestricted) when `advertiseHost` is
 * undefined -- the common case, and the actual bound endpoint that ends up
 * on the wire and in descriptors compared elsewhere.
 */
export function buildAdvertisedEndpoint(
  boundEndpoint: string,
  advertiseHost: string | undefined,
  restrictToScheme?: string
): string | undefined {
  if (advertiseHost === undefined) return normalizeEndpoint(boundEndpoint);
  const match = ADVERTISE_SOURCE_PATTERN.exec(boundEndpoint);
  if (match === null) return undefined;
  const scheme = match[1].toLowerCase();
  if (restrictToScheme !== undefined && scheme !== restrictToScheme) return undefined;
  const host = advertiseHost.includes(':') && !advertiseHost.startsWith('[')
    ? `[${advertiseHost}]`
    : advertiseHost;
  return normalizeEndpoint(`${scheme}://${host}:${match[2]}`);
}

function normalizeSuffix(suffix: string): string {
  if (suffix.length === 0) {
    return suffix;
  }
  const hashIndex = suffix.indexOf('#');
  const beforeFragment = hashIndex >= 0 ? suffix.slice(0, hashIndex) : suffix;
  const fragment = hashIndex >= 0 ? suffix.slice(hashIndex) : '';
  const queryIndex = beforeFragment.indexOf('?');
  const path = queryIndex >= 0 ? beforeFragment.slice(0, queryIndex) : beforeFragment;
  const query = queryIndex >= 0 ? beforeFragment.slice(queryIndex) : '';
  const normalizedPath = path.replace(/\/+$/, '');
  return `${normalizedPath}${query}${fragment}`;
}
