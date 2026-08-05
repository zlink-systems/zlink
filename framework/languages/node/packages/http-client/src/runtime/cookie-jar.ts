/* SPDX-License-Identifier: Apache-2.0 */

/**
 * Wrapper-owned cookie jar mirroring the deliberately narrow C++ `cookie_jar.cpp` semantics so
 * behaviour matches across languages: host-exact storage (no `Domain`), default `Path=/`, only
 * `Path`/`Secure`/`Max-Age<=0` honoured (`Domain`/`Expires` ignored), at most 128 cookies per host,
 * and secure cookies sent only on secure requests. `fetch`/undici provide no server-side persistent
 * cookie jar, so this is implemented in the wrapper.
 */

interface Cookie {
  host: string;
  name: string;
  value: string;
  path: string;
  secure: boolean;
}

const MAX_COOKIES_PER_HOST = 128;

export class CookieJar {
  private readonly cookies: Cookie[] = [];

  store(host: string, setCookieHeader: string): void {
    const segments = setCookieHeader.split(';');
    const pair = (segments.shift() ?? '').trim();
    const equals = pair.indexOf('=');
    if (equals < 0) {
      return;
    }

    const name = pair.slice(0, equals).trim();
    const value = pair.slice(equals + 1).trim();
    if (name.length === 0) {
      return;
    }

    let path = '/';
    let secure = false;
    let expired = false;
    for (const raw of segments) {
      const attribute = raw.trim();
      const attrEquals = attribute.indexOf('=');
      const attrName = (attrEquals < 0 ? attribute : attribute.slice(0, attrEquals)).trim();
      const attrValue = attrEquals < 0 ? '' : attribute.slice(attrEquals + 1).trim();
      if (attrName.toLowerCase() === 'path' && attrValue.length > 0) {
        path = attrValue;
      } else if (attrName.toLowerCase() === 'secure') {
        secure = true;
      } else if (attrName.toLowerCase() === 'max-age') {
        const maxAge = Number.parseInt(attrValue, 10);
        if (!Number.isNaN(maxAge)) {
          expired = maxAge <= 0;
        }
      }
    }

    for (let i = this.cookies.length - 1; i >= 0; i--) {
      const existing = this.cookies[i];
      if (existing.host === host && existing.name === name && existing.path === path) {
        this.cookies.splice(i, 1);
      }
    }

    if (expired) {
      return;
    }

    this.cookies.push({ host, name, value, path, secure });
    let countForHost = this.cookies.filter((stored) => stored.host === host).length;
    while (countForHost > MAX_COOKIES_PER_HOST) {
      const oldest = this.cookies.findIndex((stored) => stored.host === host);
      if (oldest < 0) {
        break;
      }
      this.cookies.splice(oldest, 1);
      countForHost--;
    }
  }

  headerFor(host: string, path: string, secure: boolean): string {
    const parts: string[] = [];
    for (const cookie of this.cookies) {
      if (cookie.host !== host || (cookie.secure && !secure) || !pathMatches(path, cookie.path)) {
        continue;
      }
      parts.push(`${cookie.name}=${cookie.value}`);
    }
    return parts.join('; ');
  }
}

function pathMatches(requestPath: string, cookiePath: string): boolean {
  if (cookiePath.length === 0 || cookiePath === '/') {
    return true;
  }
  if (requestPath === cookiePath) {
    return true;
  }
  if (!requestPath.startsWith(cookiePath)) {
    return false;
  }
  return cookiePath.endsWith('/') || requestPath[cookiePath.length] === '/';
}
