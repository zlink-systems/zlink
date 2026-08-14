/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.core;

import systems.zlink.contracts.sockets.AutoHwmProfile;
import systems.zlink.internal.ContractAccess;
import systems.zlink.internal.DurationConversions;
import java.time.Duration;
import java.util.Objects;

/**
 * Typed facade for context options.
 */
public final class ContextOptions {
    private final Context context;
    private String threadNamePrefix = "";

    public ContextOptions(Context context) {
        this.context = context;
    }

    public int ioThreads() {
        return ContractAccess.contextGetOption(context, ContextOption.IO_THREADS);
    }

    public void ioThreads(int count) {
        ContractAccess.contextSetOption(context, ContextOption.IO_THREADS, count);
    }

    public int maxSockets() {
        return ContractAccess.contextGetOption(context, ContextOption.MAX_SOCKETS);
    }

    public void maxSockets(int count) {
        ContractAccess.contextSetOption(context, ContextOption.MAX_SOCKETS, count);
    }

    public int socketLimit() {
        return ContractAccess.contextGetOption(context, ContextOption.SOCKET_LIMIT);
    }

    public int threadPriority() {
        return ContractAccess.contextGetOption(context, ContextOption.THREAD_PRIORITY);
    }

    public void threadPriority(int priority) {
        ContractAccess.contextSetOption(context, ContextOption.THREAD_PRIORITY, priority);
    }

    public int threadSchedulingPolicy() {
        return ContractAccess.contextGetOption(context, ContextOption.THREAD_SCHED_POLICY);
    }

    public void threadSchedulingPolicy(int policy) {
        ContractAccess.contextSetOption(context, ContextOption.THREAD_SCHED_POLICY, policy);
    }

    public String threadNamePrefix() {
        return threadNamePrefix;
    }

    public void threadNamePrefix(String prefix) {
        Objects.requireNonNull(prefix, "prefix");
        ContractAccess.contextSetOptionData(context,
            ContextOption.THREAD_NAME_PREFIX, prefix);
        threadNamePrefix = prefix;
    }

    public int maxMessageSize() {
        return ContractAccess.contextGetOption(context, ContextOption.MAX_MSGSZ);
    }

    public void maxMessageSize(int bytes) {
        ContractAccess.contextSetOption(context, ContextOption.MAX_MSGSZ, bytes);
    }

    public int messageThreadSize() {
        return ContractAccess.contextGetOption(context, ContextOption.MSG_T_SIZE);
    }

    public boolean blocky() {
        return ContractAccess.contextGetOption(context, ContextOption.BLOCKY) != 0;
    }

    public void blocky(boolean enabled) {
        ContractAccess.contextSetOption(context, ContextOption.BLOCKY,
            enabled ? 1 : 0);
    }

    public boolean autoHwmEnabled() {
        return ContractAccess.contextGetOption(context,
            ContextOption.AUTO_HWM_ENABLE) != 0;
    }

    public void autoHwmEnabled(boolean enabled) {
        ContractAccess.contextSetOption(context, ContextOption.AUTO_HWM_ENABLE,
            enabled ? 1 : 0);
    }

    public Duration autoHwmRecalcDebounce() {
        return Duration.ofMillis(
          ContractAccess.contextGetOption(context,
              ContextOption.AUTO_HWM_RECALC_DEBOUNCE_MS));
    }

    public void autoHwmRecalcDebounce(Duration value) {
        Objects.requireNonNull(value, "value");
        ContractAccess.contextSetOption(context,
          ContextOption.AUTO_HWM_RECALC_DEBOUNCE_MS,
          DurationConversions.toIntMillis(value, "value"));
    }

    public AutoHwmProfile coreHwmProfile() {
        return AutoHwmProfile.fromValue(
          ContractAccess.contextGetOption(context, ContextOption.AUTO_HWM_PROFILE));
    }

    public void coreHwmProfile(AutoHwmProfile profile) {
        ContractAccess.contextSetOption(context, ContextOption.AUTO_HWM_PROFILE,
            profile.value());
    }

    /**
     * Returns the explicit context memory limit in bytes.
     */
    public long coreHwmMemoryLimitBytes() {
        return ContractAccess.contextGetUInt64Option(context,
            ContextOption.AUTO_HWM_MEMORY_LIMIT_BYTES);
    }

    /**
     * Sets the explicit context memory limit in the positive Java long range.
     */
    public void coreHwmMemoryLimitBytes(long value) {
        requireNonNegativeLong(value);
        ContractAccess.contextSetUInt64Option(context,
            ContextOption.AUTO_HWM_MEMORY_LIMIT_BYTES, value);
    }

    /** Returns the manually configured context-wide Core budget in bytes. */
    public long coreHwmBudgetBytes() {
        return ContractAccess.contextGetUInt64Option(context,
            ContextOption.AUTO_HWM_CORE_BUDGET_BYTES);
    }

    /** Sets the context-wide Core budget in the positive Java long range. */
    public void coreHwmBudgetBytes(long value) {
        requireNonNegativeLong(value);
        ContractAccess.contextSetUInt64Option(context,
            ContextOption.AUTO_HWM_CORE_BUDGET_BYTES, value);
    }

    public void addThreadAffinityCpu(int cpu) {
        ContractAccess.contextSetOption(context,
            ContextOption.THREAD_AFFINITY_CPU_ADD, cpu);
    }

    public void removeThreadAffinityCpu(int cpu) {
        ContractAccess.contextSetOption(context,
            ContextOption.THREAD_AFFINITY_CPU_REMOVE, cpu);
    }

    private static void requireNonNegativeLong(long value) {
        if (value < 0) {
            throw new IllegalArgumentException(
                "byte value exceeds Java's positive long range");
        }
    }

}
