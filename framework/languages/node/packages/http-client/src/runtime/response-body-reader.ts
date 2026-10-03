/* SPDX-License-Identifier: Apache-2.0 */

import { HttpHeaderName } from './text';

import type { Readable } from 'node:stream';
import type { DownloadSink } from '../types';
import type { HttpClientOptions } from './options';
import { CONTENT_ENCODING, gunzip, inflateDeflate } from './compression';
import { readResponseBody } from './bounded-response-body';

/**
 * Reads and decodes undici response bodies for the wrapper: buffered read with the configured size
 * limit, streaming delivery to a sink, header collection, and wrapper-controlled gzip/deflate
 * decompression. Separated from the request/redirect flow so response decoding is independent.
 */
export class ResponseBodyReader {
  constructor(private readonly options: HttpClientOptions) {}

  async streamToSink(stream: Readable, sink: DownloadSink): Promise<void> {
    await readResponseBody<Buffer>(stream, this.options.maxResponseBodySize, (buffer) => {
      sink(new Uint8Array(buffer));
    });
  }

  async readBuffered(stream: Readable): Promise<Buffer> {
    const chunks: Buffer[] = [];
    await readResponseBody<Buffer>(stream, this.options.maxResponseBodySize, (buffer) => {
      chunks.push(buffer);
    });
    return Buffer.concat(chunks);
  }

  async decompress(
    bytes: Buffer,
    headers: Record<string, string>
  ): Promise<{ body: Buffer; headers: Record<string, string> }> {
    const encoding = Object.prototype.hasOwnProperty.call(headers, HttpHeaderName.ContentEncoding)
      ? headers[HttpHeaderName.ContentEncoding]
      : undefined;
    // An empty body (HEAD / 204 / 304) carries no payload to decode even with Content-Encoding.
    if (encoding === undefined || bytes.length === 0) {
      return { body: bytes, headers };
    }
    const normalizedEncoding = encoding.toLowerCase();
    if (normalizedEncoding === CONTENT_ENCODING.Gzip) {
      return {
        body: await gunzip(bytes, this.options.maxResponseBodySize),
        headers: stripEncodingHeaders(headers)
      };
    }
    if (normalizedEncoding === CONTENT_ENCODING.Deflate) {
      return {
        body: await inflateDeflate(bytes, this.options.maxResponseBodySize),
        headers: stripEncodingHeaders(headers)
      };
    }
    return { body: bytes, headers };
  }

  static collectHeaders(
    headers: Record<string, string | string[] | undefined>
  ): Record<string, string> {
    const result: Record<string, string> = {};
    for (const [name, value] of Object.entries(headers)) {
      if (value === undefined) {
        continue;
      }
      result[name.toLowerCase()] = Array.isArray(value) ? value.join(', ') : value;
    }
    return result;
  }
}

// After decoding, drop Content-Encoding and the now-stale Content-Length (it described the
// compressed body, not the decoded one).
function stripEncodingHeaders(headers: Record<string, string>): Record<string, string> {
  const copy: Record<string, string> = {};
  for (const [key, value] of Object.entries(headers)) {
    const lower = key.toLowerCase();
    if (lower !== HttpHeaderName.ContentEncoding && lower !== HttpHeaderName.ContentLength) {
      copy[key] = value;
    }
  }
  return copy;
}
