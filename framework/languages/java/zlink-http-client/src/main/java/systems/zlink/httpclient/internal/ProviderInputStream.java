/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient.internal;

import java.io.InputStream;
import java.util.function.Supplier;

/**
 * {@link InputStream} that pulls request body chunks from a provider. Wrapped in a
 * {@code BodyPublishers.ofInputStream} so the runtime streams the body with chunked transfer
 * encoding. The provider returns {@code null} when the body is complete. Mirrors the C++
 * {@code body_stream} path; such requests are excluded from automatic retry because the provider
 * cannot be rewound.
 */
final class ProviderInputStream extends InputStream {

    private final Supplier<byte[]> provider;
    private byte[] current = new byte[0];
    private int position;
    private boolean completed;

    ProviderInputStream(Supplier<byte[]> provider) {
        this.provider = provider;
    }

    @Override
    public int read() {
        byte[] single = new byte[1];
        int n = read(single, 0, 1);
        return n < 0 ? -1 : single[0] & 0xff;
    }

    @Override
    public int read(byte[] buffer, int offset, int length) {
        if (!ensureCurrent()) {
            return -1;
        }
        int take = Math.min(length, current.length - position);
        System.arraycopy(current, position, buffer, offset, take);
        position += take;
        return take;
    }

    private boolean ensureCurrent() {
        while (position >= current.length) {
            if (completed) {
                return false;
            }
            byte[] next = provider.get();
            if (next == null) {
                completed = true;
                return false;
            }
            current = next;
            position = 0;
        }
        return true;
    }
}
