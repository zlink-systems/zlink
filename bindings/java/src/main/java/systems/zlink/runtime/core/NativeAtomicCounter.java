/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.core;

import systems.zlink.contracts.core.AtomicCounter;

import systems.zlink.contracts.errors.ZlinkConfigException;
import systems.zlink.contracts.errors.ConfigResult;
import systems.zlink.runtime.nativeapi.Native;
import java.lang.foreign.MemorySegment;

final class NativeAtomicCounter implements AtomicCounter {
    private volatile MemorySegment handle;
    // Core releases a handle once; one owner at a time runs the native close
    // and later callers observe its outcome.
    private final Object closeLock = new Object();

    NativeAtomicCounter() {
        this.handle = Native.atomicCounterNew();
        if (handle == null || handle.address() == 0) {
            throw new ZlinkConfigException(ConfigResult.INVALID_HANDLE);
        }
    }

    public void set(int value) {
        ensureOpen();
        Native.atomicCounterSet(handle, value);
    }

    public int increment() {
        ensureOpen();
        return Native.atomicCounterInc(handle);
    }

    public int decrement() {
        ensureOpen();
        return Native.atomicCounterDec(handle);
    }

    public int value() {
        ensureOpen();
        return Native.atomicCounterValue(handle);
    }

    @Override
    public void close() {
        synchronized (closeLock) {
            MemorySegment current = handle;
            if (current == null || current.address() == 0) {
                return;
            }
            Native.atomicCounterDestroy(current);
            handle = MemorySegment.NULL;
        }
    }

    private void ensureOpen() {
        if (handle == null || handle.address() == 0) {
            throw new IllegalStateException("atomic counter is closed");
        }
    }
}
