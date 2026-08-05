/* SPDX-License-Identifier: Apache-2.0 */

import type { Readable } from 'node:stream';
import { ZLinkFrameworkException, ZLinkFrameworkErrorKind } from '@zlink-systems/framework';
import type { DownloadSink } from '../types';
import type { HttpClientOptions } from './options';
import { gunzip, inflateDeflate } from './compression';

/**
 * Reads and decodes undici response bodies for the wrapper: buffered read with the configured size
 * limit, streaming delivery to a sink, header collection, and wrapper-controlled gzip/deflate
 * decompression. Separated from the request/redirect flow so response decoding is independent.
 */
export class ResponseBodyReader {
  constructor(private readonly options: HttpClientOptions) {}

  async streamToSink(stream: Readable, sink: DownloadSink): Promise<void> {
    let total = 0;
    for await (const chunk of stream) {
      const buffer = chunk as Buffer;
      total += buffer.length;
      if (total > this.options.maxResponseBodySize) {
        throw exceededBodySize();
      }
      sink(new Uint8Array(buffer));
    }
  }

  async readBuffered(stream: Readable): Promise<Buffer> {
    const chunks: Buffer[] = [];
    let total = 0;
    for await (const chunk of stream) {
      const buffer = chunk as Buffer;
      total += buffer.length;
      if (total > this.options.maxResponseBodySize) {
        throw exceededBodySize();
      }
      chunks.push(buffer);
    }
    return Buffer.concat(chunks);
  }

  async decompress(
    bytes: Buffer,
    headers: Record<string, string>,
  ): Promise<{ body: Buffer; headers: Record<string, string> }> {
    const encoding = Object.prototype.hasOwnProperty.call(headers, 'content-encoding')
      ? headers['content-encoding']
      : undefined;
    // An empty body (HEAD / 204 / 304) carries no payload to decode even with Content-Encoding.
    if (encoding === undefined || bytes.length === 0) {
      return { body: bytes, headers };
    }
    if (encoding.toLowerCase() === 'gzip') {
      return {
        body: await gunzip(bytes, this.options.maxResponseBodySize),
        headers: stripEncodingHeaders(headers),
      };
    }
    if (encoding.toLowerCase() === 'deflate') {
      return {
        body: await inflateDeflate(bytes, this.options.maxResponseBodySize),
        headers: stripEncodingHeaders(headers),
      };
    }
    return { body: bytes, headers };
  }

  static collectHeaders(headers: Record<string, string | string[] | undefined>): Record<string, string> {
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
    if (lower !== 'content-encoding' && lower !== 'content-length') {
      copy[key] = value;
    }
  }
  return copy;
}

function exceededBodySize(): ZLinkFrameworkException {
  return new ZLinkFrameworkException(
    ZLinkFrameworkErrorKind.Unavailable,
    'HTTP response exceeded the maximum body size',
  );
}
