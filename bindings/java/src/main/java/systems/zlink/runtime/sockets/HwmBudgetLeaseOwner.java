/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.util.Arrays;
import systems.zlink.runtime.nativeapi.Native;

/** Single private owner for the Core leases attached to one receive result. */
final class HwmBudgetLeaseOwner implements AutoCloseable, Runnable {
    private final Arena releaseArena = Arena.ofShared();
    private final MemorySegment releaseSlot =
        releaseArena.allocate(ValueLayout.ADDRESS);
    private long[] leaseAddresses = new long[4];
    private int leaseCount;
    private boolean closed;

    /** Moves the lease in {@code leaseOut} into this owner. */
    synchronized void adopt(MemorySegment leaseOut) {
        MemorySegment lease = leaseOut.get(ValueLayout.ADDRESS, 0);
        if (lease.address() == 0)
            return;
        if (closed) {
            Native.hwmBudgetLeaseRelease(leaseOut);
            return;
        }
        if (leaseCount == leaseAddresses.length) {
            try {
                leaseAddresses = Arrays.copyOf(leaseAddresses,
                    leaseAddresses.length * 2);
            } catch (RuntimeException | Error ex) {
                Native.hwmBudgetLeaseRelease(leaseOut);
                throw ex;
            }
        }
        leaseAddresses[leaseCount++] = lease.address();
        leaseOut.set(ValueLayout.ADDRESS, 0, MemorySegment.NULL);
    }

    /** Releases a lease that could not be adopted by a result owner. */
    static void releaseNative(MemorySegment leaseOut) {
        if (leaseOut.get(ValueLayout.ADDRESS, 0).address() != 0)
            Native.hwmBudgetLeaseRelease(leaseOut);
    }

    @Override
    public void close() {
        run();
    }

    @Override
    public synchronized void run() {
        if (closed)
            return;
        closed = true;
        for (int index = 0; index < leaseCount; index++) {
            long address = leaseAddresses[index];
            leaseAddresses[index] = 0L;
            if (address == 0L)
                continue;
            releaseSlot.set(ValueLayout.ADDRESS, 0,
                MemorySegment.ofAddress(address));
            try {
                Native.hwmBudgetLeaseRelease(releaseSlot);
            } catch (RuntimeException ignored) {
            }
        }
        leaseCount = 0;
        releaseArena.close();
    }
}
