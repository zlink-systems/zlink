/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.core;

import systems.zlink.contracts.core.ZlinkStopwatch;

import systems.zlink.contracts.errors.ZlinkConfigException;
import systems.zlink.contracts.errors.ConfigResult;
import systems.zlink.runtime.nativeapi.Native;
import java.lang.foreign.MemorySegment;
import java.time.Duration;

final class NativeStopwatch implements ZlinkStopwatch {
    private volatile MemorySegment handle;
    // Core releases a handle once; one owner at a time runs the native close
    // and later callers observe its outcome.
    private final Object closeLock = new Object();
    private boolean stopped;

    NativeStopwatch() {
        this.handle = Native.stopwatchStart();
        if (handle == null || handle.address() == 0) {
            throw new ZlinkConfigException(ConfigResult.INVALID_HANDLE);
        }
    }

    public Duration intermediate() {
        ensureOpen();
        return Duration.ofNanos(Native.stopwatchIntermediate(handle));
    }

    public Duration stop() {
        synchronized (closeLock) {
            ensureOpen();
            long elapsed = Native.stopwatchStop(handle);
            handle = MemorySegment.NULL;
            stopped = true;
            return Duration.ofNanos(elapsed);
        }
    }

    @Override
    public void close() {
        synchronized (closeLock) {
            if (handle == null || handle.address() == 0) {
                return;
            }
            if (!stopped) {
                stop();
            } else {
                handle = MemorySegment.NULL;
            }
        }
    }

    private void ensureOpen() {
        if (handle == null || handle.address() == 0) {
            throw new IllegalStateException("stopwatch is closed");
        }
    }
}
