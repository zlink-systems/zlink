/* SPDX-License-Identifier: Apache-2.0 */

import { ZLinkFrameworkException, ZLinkFrameworkErrorKind } from '@zlink-systems/framework';

export const HttpHeaderName = Object.freeze({
  Accept: 'accept',
  Authorization: 'authorization',
  ContentType: 'content-type',
  Location: 'location',
  SetCookie: 'set-cookie',
  UserAgent: 'user-agent',
  AcceptEncoding: 'accept-encoding',
  Cookie: 'cookie',
  ContentEncoding: 'content-encoding',
  ContentLength: 'content-length'
});
export const HttpFormContentType = Object.freeze({
  UrlEncoded: 'application/x-www-form-urlencoded',
  Multipart: 'multipart/form-data'
});

/** Shared text helpers mirroring the C++ `client.cpp` anonymous-namespace utilities. */

function isBlank(value: string): boolean {
  return value.length === 0 || /^[\s]*$/u.test(value);
}

export function requireNonBlank(value: string, message: string): void {
  if (isBlank(value)) {
    throw new ZLinkFrameworkException(ZLinkFrameworkErrorKind.ProtocolError, message);
  }
}

export function requirePositiveTimeout(value: number): void {
  if (!(value > 0)) {
    throw new ZLinkFrameworkException(
      ZLinkFrameworkErrorKind.ProtocolError,
      'HTTP client timeout must be greater than zero'
    );
  }
}

const unreserved = /[A-Za-z0-9\-_.~]/u;

export function percentEncode(value: string): string {
  const bytes = new TextEncoder().encode(value);
  let encoded = '';
  for (const byte of bytes) {
    const char = String.fromCharCode(byte);
    if (unreserved.test(char)) {
      encoded += char;
    } else {
      encoded += '%' + byte.toString(16).toUpperCase().padStart(2, '0');
    }
  }
  return encoded;
}

export function basicAuthorization(user: string, password: string): string {
  return 'Basic ' + Buffer.from(`${user}:${password}`, 'utf8').toString('base64');
}

const MULTIPART_BOUNDARY_PREFIX = 'zlink-boundary-';
const MULTIPART_BOUNDARY_ENTROPY_CHARACTERS = 16;

export function makeMultipartBoundary(): string {
  return (
    MULTIPART_BOUNDARY_PREFIX +
    Math.random()
      .toString(16)
      .slice(2)
      .padEnd(MULTIPART_BOUNDARY_ENTROPY_CHARACTERS, '0')
      .slice(0, MULTIPART_BOUNDARY_ENTROPY_CHARACTERS)
  );
}
