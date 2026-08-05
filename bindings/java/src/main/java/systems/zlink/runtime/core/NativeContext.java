/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.core;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.ContextOption;
import systems.zlink.contracts.core.ContextOptions;
import systems.zlink.internal.ContractAccess;
import systems.zlink.contracts.errors.ZlinkConfigException;
import systems.zlink.contracts.errors.ConfigResult;
import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.PairSocket;
import systems.zlink.contracts.sockets.PubSocket;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.StreamSocket;
import systems.zlink.contracts.sockets.SubSocket;
import systems.zlink.contracts.sockets.XPubSocket;
import systems.zlink.contracts.sockets.XSubSocket;
import systems.zlink.runtime.nativeapi.InternalAccess;
import systems.zlink.runtime.nativeapi.Native;
import systems.zlink.runtime.nativeapi.NativeErrno;
import systems.zlink.runtime.nativeapi.NativeHelpers;
import systems.zlink.runtime.sockets.NativeSockets;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;

final class NativeContext implements Context {
    private static final boolean DEBUG_REQREP =
      Boolean.getBoolean("zlink.reqrep.debug");
        private final ContextOptions options;
    private MemorySegment handle;

    static {
        InternalAccess.register(new InternalAccess.ContextAccess() {
            @Override
            public MemorySegment handle(Context context) {
                return ((NativeContext) context).handle();
            }

            @Override
            public void setOption(Context context, ContextOption option,
                                  int value) {
                ((NativeContext) context).setOption(option, value);
            }

            @Override
            public void setOptionData(Context context, ContextOption option,
                                      String value) {
                ((NativeContext) context).setOptionData(option, value);
            }

            @Override
            public int getOption(Context context, ContextOption option) {
                return ((NativeContext) context).getOption(option);
            }

        });
        ContractAccess.register(new ContractAccess.ContextOptionsAccess() {
            @Override
            public void setOption(Context context, ContextOption option,
                                  int value) {
                ((NativeContext) context).setOption(option, value);
            }

            @Override
            public void setUInt64Option(Context context, ContextOption option,
                                        long value) {
                ((NativeContext) context).setUInt64Option(option, value);
            }

            @Override
            public void setOptionData(Context context, ContextOption option,
                                      String value) {
                ((NativeContext) context).setOptionData(option, value);
            }

            @Override
            public int getOption(Context context, ContextOption option) {
                return ((NativeContext) context).getOption(option);
            }

            @Override
            public long getUInt64Option(Context context,
                                        ContextOption option) {
                return ((NativeContext) context).getUInt64Option(option);
            }
        });
    }

    NativeContext() {
        this.handle = Native.ctxNew();
        if (handle == null || handle.address() == 0) {
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
        }
        this.options = new ContextOptions(this);
    }

    @Override
    public ContextOptions options() {
        return options;
    }

    @Override
    public PairSocket createPairSocket() {
        return NativeSockets.pair(this);
    }

    @Override
    public DealerSocket createDealerSocket() {
        return NativeSockets.dealer(this);
    }

    @Override
    public RouterSocket createRouterSocket() {
        return NativeSockets.router(this);
    }

    @Override
    public PubSocket createPubSocket() {
        return NativeSockets.pub(this);
    }

    @Override
    public SubSocket createSubSocket() {
        return NativeSockets.sub(this);
    }

    @Override
    public XPubSocket createXPubSocket() {
        return NativeSockets.xpub(this);
    }

    @Override
    public XSubSocket createXSubSocket() {
        return NativeSockets.xsub(this);
    }

    @Override
    public StreamSocket createStreamSocket() {
        return NativeSockets.stream(this);
    }

    MemorySegment handle() {
        return handle;
    }

    @Override
    public void shutdown() {
        ensureOpen();
        int rc = Native.ctxShutdown(handle);
        if (rc != 0) {
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
        }
    }

    @Override
    public void recalculateAutoHwm() {
        ensureOpen();
        int rc = Native.ctxAutoHwmRecalculate(handle);
        if (rc != 0) {
            throw new ZlinkConfigException(ConfigResult.fromValue(rc));
        }
    }

    @Override
    public void close() {
        if (handle == null || handle.address() == 0) {
            return;
        }
        debug("ctxTerm begin");
        NativeErrno.retryWhileInterrupted(() -> Native.ctxShutdown(handle),
            rc -> rc != 0);
        NativeErrno.retryWhileInterrupted(() -> Native.ctxTerm(handle),
            rc -> rc != 0);
        debug("ctxTerm end");
        handle = MemorySegment.NULL;
    }

    private void setOption(ContextOption option, int value) {
        ensureOpen();
        int rc = Native.ctxSet(handle, option.getValue(), value);
        if (rc != 0) {
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
        }
    }

    private void setOptionData(ContextOption option, String value) {
        ensureOpen();
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment bytes = NativeHelpers.toCString(arena, value);
            int byteLength = bytes.getString(0).getBytes(
                java.nio.charset.StandardCharsets.UTF_8).length;
            int rc = Native.ctxSetData(handle, option.getValue(), bytes,
                byteLength);
            if (rc != 0) {
                throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
            }
        }
    }

    private void setUInt64Option(ContextOption option, long value) {
        ensureOpen();
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment bytes = arena.allocate(ValueLayout.JAVA_LONG);
            bytes.set(ValueLayout.JAVA_LONG, 0, value);
            int rc = Native.ctxSetData(handle, option.getValue(), bytes,
                Long.BYTES);
            if (rc != 0) {
                throw ZlinkException.fromLastError(
                    systems.zlink.contracts.errors.ErrorCategory.CONFIG);
            }
        }
    }

    private int getOption(ContextOption option) {
        ensureOpen();
        int rc = Native.ctxGet(handle, option.getValue());
        if (rc < 0 && option != ContextOption.THREAD_PRIORITY
            && option != ContextOption.THREAD_SCHED_POLICY) {
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
        }
        return rc;
    }

    private long getUInt64Option(ContextOption option) {
        ensureOpen();
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment value = arena.allocate(ValueLayout.JAVA_LONG);
            MemorySegment size = arena.allocate(ValueLayout.JAVA_LONG);
            size.set(ValueLayout.JAVA_LONG, 0, Long.BYTES);
            int rc = Native.ctxGetData(handle, option.getValue(), value, size);
            if (rc != 0) {
                throw ZlinkException.fromLastError(
                    systems.zlink.contracts.errors.ErrorCategory.CONFIG);
            }
            if (size.get(ValueLayout.JAVA_LONG, 0) != Long.BYTES) {
                throw new IllegalStateException(
                    "native context option returned an invalid value size");
            }
            return value.get(ValueLayout.JAVA_LONG, 0);
        }
    }

    private void ensureOpen() {
        if (handle == null || handle.address() == 0) {
            throw new IllegalStateException("context is closed");
        }
    }

    private static void debug(String message) {
        if (DEBUG_REQREP) {
            try {
                Files.writeString(Path.of("/tmp/zlink-reqrep.log"),
                    "[context] " + message + System.lineSeparator(),
                    StandardOpenOption.CREATE, StandardOpenOption.APPEND);
            } catch (Exception ignored) {
            }
        }
    }
}
