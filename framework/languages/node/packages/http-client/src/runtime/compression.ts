/* SPDX-License-Identifier: Apache-2.0 */

import { gunzip as gunzipCallback, inflate as inflateCallback, inflateRaw as inflateRawCallback } from 'node:zlib';
import { promisify } from 'node:util';
import { ZLinkFrameworkException, ZLinkFrameworkErrorKind } from '@zlink-systems/framework';

const gunzipAsync = promisify(gunzipCallback);
const inflateAsync = promisify(inflateCallback);
const inflateRawAsync = promisify(inflateRawCallback);

/**
 * Wrapper-controlled response decompression mirroring the C++ `compression.cpp`: gzip and deflate
 * are decoded, the decoded size is bounded by the configured body limit (enforced by zlib's
 * `maxOutputLength` so a malicious response cannot allocate past the limit before the check), and the
 * caller removes the `Content-Encoding` header afterwards. undici's `request` does not auto-decompress,
 * so streaming downloads are never transparently decoded. A malformed body raises `payloadDecodeFailed`;
 * exceeding the limit raises `requestFailed`. Decoding runs on zlib's async worker pool — the
 * synchronous variants would block the event loop for the whole decode of a large body.
 */
export function gunzip(input: Buffer, maxBytes: number): Promise<Buffer> {
  return decode(() => gunzipAsync(input, { maxOutputLength: maxBytes, chunkSize: 1 << 20 }));
}

export function inflateDeflate(input: Buffer, maxBytes: number): Promise<Buffer> {
  // Detect a zlib-wrapped stream (CMF/FLG: method deflate, header a multiple of 31) vs raw deflate.
  const zlibWrapped =
    input.length >= 2 && (input[0] & 0x0f) === 8 && (((input[0] << 8) | input[1]) % 31) === 0;
  return decode(() =>
    zlibWrapped
      ? inflateAsync(input, { maxOutputLength: maxBytes, chunkSize: 1 << 20 })
      : inflateRawAsync(input, { maxOutputLength: maxBytes, chunkSize: 1 << 20 }),
  );
}

async function decode(run: () => Promise<Buffer>): Promise<Buffer> {
  try {
    return await run();
  } catch (cause) {
    // zlib rejects with a RangeError (ERR_BUFFER_TOO_LARGE) when output exceeds maxOutputLength.
    if (cause instanceof RangeError) {
      throw new ZLinkFrameworkException(
        ZLinkFrameworkErrorKind.Unavailable,
        'HTTP response compressed body exceeds maxResponseBodySize',
      );
    }
    throw new ZLinkFrameworkException(
      ZLinkFrameworkErrorKind.ProtocolError,
      'HTTP response compressed body is malformed',
      cause,
    );
  }
}
