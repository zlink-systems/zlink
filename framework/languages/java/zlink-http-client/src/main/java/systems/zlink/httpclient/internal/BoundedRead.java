/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient.internal;

import java.io.IOException;
import java.io.InputStream;
import java.util.function.Supplier;
import systems.zlink.framework.errors.ZLinkFrameworkException;

/**
 * The single bounded read loop shared by buffered reads, sink streaming, and decompression.
 * Closes the stream, enforces the byte limit via the supplied failure, and hands each chunk to
 * the consumer without retaining it.
 */
final class BoundedRead {

    interface ChunkConsumer {
        void accept(byte[] buffer, int length) throws IOException;
    }

    private BoundedRead() {
    }

    static void copy(
        InputStream stream,
        long maxBytes,
        Supplier<ZLinkFrameworkException> onLimit,
        ChunkConsumer consumer) throws IOException {
        try (stream) {
            byte[] buffer = new byte[16384];
            long total = 0;
            int read;
            while ((read = stream.read(buffer)) > 0) {
                total += read;
                if (total > maxBytes) {
                    throw onLimit.get();
                }
                consumer.accept(buffer, read);
            }
        }
    }
}
