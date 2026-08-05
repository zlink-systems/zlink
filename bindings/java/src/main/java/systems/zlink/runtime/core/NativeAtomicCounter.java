/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.core;

import systems.zlink.contracts.core.AtomicCounter;

import systems.zlink.contracts.errors.ZlinkConfigException;
import systems.zlink.contracts.errors.ConfigResult;
import systems.zlink.runtime.nativeapi.Native;
import java.lang.foreign.MemorySegment;

final class NativeAtomicCounter implements AtomicCounter {
    private MemorySegment handle;

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
        if (handle == null || handle.address() == 0) {
            return;
        }
        Native.atomicCounterDestroy(handle);
        handle = MemorySegment.NULL;
    }

    private void ensureOpen() {
        if (handle == null || handle.address() == 0) {
            throw new IllegalStateException("atomic counter is closed");
        }
    }
}
