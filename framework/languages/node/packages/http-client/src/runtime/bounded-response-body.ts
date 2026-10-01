/* SPDX-License-Identifier: Apache-2.0 */

import { responseBodySizeExceeded } from './http-client-errors';

/** Owns the received-byte limit for buffered and streaming responses on both transports. */
export async function readResponseBody<T extends Uint8Array>(
  stream: AsyncIterable<T>,
  maximumSize: number,
  consume: (chunk: T) => void
): Promise<void> {
  let total = 0;
  for await (const chunk of stream) {
    total += chunk.length;
    if (total > maximumSize) {
      throw responseBodySizeExceeded();
    }
    consume(chunk);
  }
}
