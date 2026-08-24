/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.core;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.Socket;
import systems.zlink.runtime.nativeapi.InternalAccess;
import systems.zlink.runtime.nativeapi.Native;
import systems.zlink.runtime.nativeapi.NativeHelpers;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.time.Duration;
import java.util.Objects;

final class NativeCoreRuntime {
    private NativeCoreRuntime() {
    }

    static int errno() {
        return Native.errno();
    }

    static String strerror(int errnum) {
        return Native.strerror(errnum);
    }

    static boolean has(String capability) {
        Objects.requireNonNull(capability, "capability");
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment cap = NativeHelpers.toCString(arena, capability);
            return Native.has(cap) != 0;
        }
    }

    static int[] version() {
        return Native.version();
    }

    static void proxy(Socket frontend, Socket backend, Socket capture) {
        Objects.requireNonNull(frontend, "frontend");
        Objects.requireNonNull(backend, "backend");
        Native.proxy(InternalAccess.socketHandle(frontend),
            InternalAccess.socketHandle(backend),
            capture == null ? MemorySegment.NULL
                : InternalAccess.socketHandle(capture));
    }

    static void sleep(int seconds) {
        if (seconds < 0)
            throw new IllegalArgumentException("seconds must be >= 0");
        Native.sleep(seconds);
    }

    static void sleep(Duration duration) {
        Objects.requireNonNull(duration, "duration");
        long seconds = duration.toSeconds();
        if (seconds < 0 || seconds > Integer.MAX_VALUE) {
            throw new IllegalArgumentException(
              "duration seconds out of int range: " + seconds);
        }
        Native.sleep((int) seconds);
    }

    static void multipartClose(Message[] parts) {
        Message.closeAll(parts);
    }
}
