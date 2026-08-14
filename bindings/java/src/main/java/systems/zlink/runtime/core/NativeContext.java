/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.core;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.ContextOption;
import systems.zlink.contracts.core.ContextOptions;
import systems.zlink.contracts.core.CoreHwmBudgetSnapshot;
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
import systems.zlink.runtime.nativeapi.NativeLayouts;
import systems.zlink.runtime.sockets.NativeSockets;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.util.ArrayList;
import java.util.List;

final class NativeContext implements Context {
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
        long runtimeMemoryLimit = Runtime.getRuntime().maxMemory();
        if (runtimeMemoryLimit > 0) {
            setUInt64Option(
                ContextOption.AUTO_HWM_RUNTIME_MEMORY_LIMIT_BYTES,
                runtimeMemoryLimit);
        }
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
    public CoreHwmBudgetSnapshot coreHwmBudgetSnapshot() {
        ensureOpen();
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment snapshot = arena.allocate(
                NativeLayouts.CORE_HWM_BUDGET_SNAPSHOT_LAYOUT);
            snapshot.set(ValueLayout.JAVA_INT,
                NativeLayouts.CORE_HWM_BUDGET_ABI_VERSION_OFFSET, 1);
            snapshot.set(ValueLayout.JAVA_INT,
                NativeLayouts.CORE_HWM_BUDGET_STRUCT_SIZE_OFFSET,
                Math.toIntExact(
                    NativeLayouts.CORE_HWM_BUDGET_SNAPSHOT_LAYOUT.byteSize()));
            int rc = Native.ctxGetAutoHwmBudgetSnapshot(handle, snapshot);
            if (rc != 0) {
                throw new ZlinkConfigException(ConfigResult.fromValue(rc));
            }

            List<Long> reserved = new ArrayList<>(8);
            for (int index = 0; index < 8; ++index) {
                reserved.add(readPositiveLong(snapshot,
                    NativeLayouts.CORE_HWM_BUDGET_RESERVED_OFFSET
                        + (long) index * Long.BYTES,
                    "reservedUInt64[" + index + "]"));
            }
            return new CoreHwmBudgetSnapshot(
                snapshot.get(ValueLayout.JAVA_INT,
                    NativeLayouts.CORE_HWM_BUDGET_ABI_VERSION_OFFSET),
                snapshot.get(ValueLayout.JAVA_INT,
                    NativeLayouts.CORE_HWM_BUDGET_STRUCT_SIZE_OFFSET),
                readBudgetValue(snapshot, 0, "budgetGeneration"),
                readBudgetValue(snapshot, 1, "measurementEpoch"),
                readBudgetValue(snapshot, 2, "configuredMemoryLimitBytes"),
                readBudgetValue(snapshot, 3, "runtimeMemoryLimitBytes"),
                readBudgetValue(snapshot, 4, "resolvedMemoryLimitBytes"),
                readBudgetValue(snapshot, 5, "configuredCoreBudgetBytes"),
                readBudgetValue(snapshot, 6, "effectiveCoreBudgetBytes"),
                readBudgetValue(snapshot, 7, "totalPlannedHwmBytes"),
                readBudgetValue(snapshot, 8, "totalAppliedHwmBytes"),
                readBudgetValue(snapshot, 9, "manualReservedHwmBytes"),
                readBudgetValue(snapshot, 10, "coreQueueAccountedBytes"),
                readBudgetValue(snapshot, 11, "applicationAccountedBytes"),
                readBudgetValue(snapshot, 12, "currentAccountedBytes"),
                readBudgetValue(snapshot, 13, "provisionalAccountedBytes"),
                readBudgetValue(snapshot, 14, "peakAccountedBytes"),
                readBudgetValue(snapshot, 15,
                    "completionCurrentAccountedBytes"),
                readBudgetValue(snapshot, 16,
                    "completionPeakAccountedBytes"),
                readBudgetValue(snapshot, 17,
                    "completionPendingMessageCount"),
                readBudgetValue(snapshot, 18,
                    "totalMessagingAccountedBytes"),
                readBudgetValue(snapshot, 19,
                    "monitorQueueAppliedHwmBytes"),
                readBudgetValue(snapshot, 20,
                    "monitorQueueAccountedBytes"),
                readBudgetValue(snapshot, 21,
                    "totalInstanceAppliedHwmBytes"),
                readBudgetValue(snapshot, 22,
                    "totalInstanceAccountedBytes"),
                readBudgetValue(snapshot, 23, "oversizeAdmissionCount"),
                readBudgetValue(snapshot, 24,
                    "largestOversizeMessageBytes"),
                readBudgetValue(snapshot, 25,
                    "activeDirectionalQueueCount"),
                readBudgetValue(snapshot, 26,
                    "activeCompletionDirectionalQueueCount"),
                readBudgetValue(snapshot, 27, "activeSendQueueCount"),
                readBudgetValue(snapshot, 28, "activeReceiveQueueCount"),
                readBudgetValue(snapshot, 29,
                    "outstandingApplicationLeaseCount"),
                readBudgetValue(snapshot, 30, "retiredQueueCount"),
                readBudgetValue(snapshot, 31, "deferredOriginCreditBytes"),
                readBudgetValue(snapshot, 32, "unlimitedManualQueueCount"),
                snapshot.get(ValueLayout.JAVA_INT,
                    NativeLayouts.CORE_HWM_BUDGET_BLOCKED_RATIO_OFFSET),
                snapshot.get(ValueLayout.JAVA_INT,
                    NativeLayouts.CORE_HWM_BUDGET_FLAGS_OFFSET),
                reserved);
        }
    }

    @Override
    public void resetCoreHwmBudgetMetrics() {
        ensureOpen();
        int rc = Native.ctxResetAutoHwmBudgetMetrics(handle);
        if (rc != 0) {
            throw new ZlinkConfigException(ConfigResult.fromValue(rc));
        }
    }

    @Override
    public void close() {
        if (handle == null || handle.address() == 0) {
            return;
        }
        NativeErrno.retryWhileInterrupted(() -> Native.ctxShutdown(handle),
            rc -> rc != 0);
        NativeErrno.retryWhileInterrupted(() -> Native.ctxTerm(handle),
            rc -> rc != 0);
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
                throw new ZlinkConfigException(ConfigResult.fromValue(rc));
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
                throw new ZlinkConfigException(ConfigResult.fromValue(rc));
            }
            if (size.get(ValueLayout.JAVA_LONG, 0) != Long.BYTES) {
                throw new IllegalStateException(
                    "native context option returned an invalid value size");
            }
            return requirePositiveLong(
                value.get(ValueLayout.JAVA_LONG, 0), option.name());
        }
    }

    private static long readBudgetValue(MemorySegment snapshot, int index,
                                        String name) {
        return readPositiveLong(snapshot,
            NativeLayouts.CORE_HWM_BUDGET_VALUES_OFFSET
                + (long) index * Long.BYTES, name);
    }

    private static long readPositiveLong(MemorySegment snapshot, long offset,
                                         String name) {
        return requirePositiveLong(
            snapshot.get(ValueLayout.JAVA_LONG, offset), name);
    }

    private static long requirePositiveLong(long value, String name) {
        if (value < 0) {
            throw new ArithmeticException(
                name + " exceeds Java's positive long range");
        }
        return value;
    }

    private void ensureOpen() {
        if (handle == null || handle.address() == 0) {
            throw new IllegalStateException("context is closed");
        }
    }
}
