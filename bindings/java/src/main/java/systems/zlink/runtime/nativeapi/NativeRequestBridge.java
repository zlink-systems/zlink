/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.nativeapi;

import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;

/** Optional binding-private bridge for common two-part request submission. */
public final class NativeRequestBridge {
    private static final FunctionDescriptor DEALER_TWO_PART_REQUEST =
        FunctionDescriptor.of(ValueLayout.JAVA_LONG,
            ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.ADDRESS,
            ValueLayout.JAVA_INT, ValueLayout.JAVA_INT,
            ValueLayout.ADDRESS, ValueLayout.JAVA_LONG);
    private static final FunctionDescriptor TWO_PART_REQUEST =
        FunctionDescriptor.of(ValueLayout.JAVA_LONG,
            ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.ADDRESS,
            ValueLayout.ADDRESS, ValueLayout.JAVA_INT, ValueLayout.JAVA_INT,
            ValueLayout.ADDRESS, ValueLayout.JAVA_LONG);
    private static final MethodHandle DEALER =
        NativeSymbols.downcallOptional("zlink_java_dealer_request_two",
            DEALER_TWO_PART_REQUEST);
    private static final MethodHandle DEALER_TWO =
        NativeSymbols.downcallOptional(
            "zlink_java_dealer_request_transport_pair_two",
            TWO_PART_REQUEST);
    private static final MethodHandle ROUTER_TWO =
        NativeSymbols.downcallOptional(
            "zlink_java_router_request_transport_pair_two",
            TWO_PART_REQUEST);
    private static final MethodHandle SNAPSHOT_PAIR =
        NativeSymbols.downcallOptional("zlink_java_snapshot_message_pair",
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
                ValueLayout.ADDRESS));
    private static final MethodHandle ROUTER_REPLY_TWO =
        NativeSymbols.downcallOptional("zlink_java_router_reply_two",
            FunctionDescriptor.of(ValueLayout.JAVA_LONG,
                ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                ValueLayout.JAVA_LONG, ValueLayout.ADDRESS,
                ValueLayout.ADDRESS));

    private NativeRequestBridge() {
    }

    public static boolean available() {
        return DEALER != null && DEALER_TWO != null && ROUTER_TWO != null;
    }

    public static boolean snapshotPairAvailable() {
        return SNAPSHOT_PAIR != null;
    }

    public static boolean replyTwoAvailable() {
        return ROUTER_REPLY_TWO != null;
    }

    /**
     * Returns the native result in the low 32 bits and the number of source
     * messages consumed by Core in the high 32 bits.
     */
    public static long replyTwo(MemorySegment socket,
                                MemorySegment routingId,
                                long requestSequence,
                                MemorySegment first,
                                MemorySegment second) {
        if (ROUTER_REPLY_TWO == null) {
            throw new IllegalStateException(
                "Java router reply bridge is unavailable");
        }
        try {
            return (long) ROUTER_REPLY_TWO.invokeExact(socket, routingId,
                requestSequence, first, second);
        } catch (RuntimeException | Error failure) {
            throw failure;
        } catch (Throwable failure) {
            throw new RuntimeException("two-part router reply failed",
                failure);
        }
    }

    public static int snapshotPair(MemorySegment parts,
                                   MemorySegment snapshot) {
        if (SNAPSHOT_PAIR == null) {
            throw new IllegalStateException(
                "Java reply snapshot bridge is unavailable");
        }
        try {
            return (int) SNAPSHOT_PAIR.invokeExact(parts, snapshot);
        } catch (RuntimeException | Error failure) {
            throw failure;
        } catch (Throwable failure) {
            throw new RuntimeException("two-part reply snapshot failed",
                failure);
        }
    }

    public static long submitTwo(boolean dealer,
                                MemorySegment socket,
                                MemorySegment target,
                                MemorySegment first,
                                MemorySegment second,
                                int flags,
                                int timeoutMs,
                                MemorySegment callback,
                                long userdata) {
        if (dealer && target.equals(MemorySegment.NULL)) {
            if (DEALER == null) {
                throw new IllegalStateException(
                    "Java dealer request bridge is unavailable");
            }
            try {
                return (long) DEALER.invokeExact(socket, first, second, flags,
                    timeoutMs, callback, userdata);
            } catch (RuntimeException | Error failure) {
                throw failure;
            } catch (Throwable failure) {
                throw new RuntimeException("two-part dealer request bridge failed",
                    failure);
            }
        }
        MethodHandle handle = dealer ? DEALER_TWO : ROUTER_TWO;
        if (handle == null) {
            throw new IllegalStateException(
                "Java request bridge is unavailable");
        }
        try {
            return (long) handle.invokeExact(socket, target, first, second,
                flags, timeoutMs, callback, userdata);
        } catch (RuntimeException | Error failure) {
            throw failure;
        } catch (Throwable failure) {
            throw new RuntimeException("two-part request bridge failed",
                failure);
        }
    }
}
