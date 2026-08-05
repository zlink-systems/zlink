/* SPDX-License-Identifier: Apache-2.0 */

import { ZLinkFrameworkException, ZLinkFrameworkErrorKind } from '@zlink-systems/framework';
import type { ZLinkHttpMethod } from '../types';

/**
 * Stateless redirect and target-URL rules for the wrapper-owned redirect loop. Isolates the redirect
 * contract (allowed statuses, location resolution, method rewrite) from the request flow.
 */

const REDIRECT_STATUSES = new Set([301, 302, 303, 307, 308]);

export function isRedirectStatus(status: number): boolean {
  return REDIRECT_STATUSES.has(status);
}

/** Combines the base URL path prefix with the request target. */
export function makeTarget(prefix: string, path: string): string {
  if (prefix.length === 0 || prefix === '/') {
    return path;
  }
  return prefix.endsWith('/') ? prefix.slice(0, -1) + path : prefix + path;
}

/**
 * Applies the method/body rewrite for a followed redirect: 303, and 301/302 on POST, become a
 * bodyless GET; all other redirects preserve the method and body.
 */
export function rewriteForRedirect(
  status: number,
  method: ZLinkHttpMethod,
  body: string | undefined,
): { method: ZLinkHttpMethod; body: string | undefined } {
  if (status === 303 || ((status === 301 || status === 302) && method === 'POST')) {
    return { method: 'GET', body: undefined };
  }
  return { method, body };
}

export function resolveLocation(current: URL, location: string): URL {
  try {
    if (location.startsWith('http://') || location.startsWith('https://')) {
      return new URL(location);
    }
    if (location.startsWith('//')) {
      // Protocol-relative location: inherit the current scheme (current.protocol includes the ':').
      return new URL(current.protocol + location);
    }
    if (location.startsWith('/')) {
      return new URL(current.origin + location);
    }
  } catch {
    // A malformed Location is a redirect-protocol error, not a transport failure: throw a
    // non-retriable framework exception so retry does not resend the original request.
    throw new ZLinkFrameworkException(
      ZLinkFrameworkErrorKind.Unavailable,
      `HTTP redirect location is malformed: ${location}`,
    );
  }
  throw new ZLinkFrameworkException(
    ZLinkFrameworkErrorKind.Unavailable,
    `HTTP redirect location is not supported: ${location}`,
  );
}
