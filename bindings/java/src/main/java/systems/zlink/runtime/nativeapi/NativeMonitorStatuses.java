/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.nativeapi;

import systems.zlink.contracts.eventing.MonitorStatus;
import systems.zlink.contracts.eventing.MonitorStateFlags;
import systems.zlink.contracts.eventing.MonitorStatusDetailFlags;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;

public final class NativeMonitorStatuses {
    private static final int MONITOR_STATUS_ABI_VERSION = 2;

    private NativeMonitorStatuses() {
    }

    public static MonitorStatus fromNative(MemorySegment segment) {
        int abiVersion = segment.get(ValueLayout.JAVA_INT,
            NativeLayouts.MONITOR_SNAPSHOT_ABI_VERSION_OFFSET);
        int structSize = segment.get(ValueLayout.JAVA_INT,
            NativeLayouts.MONITOR_SNAPSHOT_STRUCT_SIZE_OFFSET);
        if (abiVersion != MONITOR_STATUS_ABI_VERSION
            || Integer.toUnsignedLong(structSize)
                != NativeLayouts.MONITOR_SNAPSHOT_LAYOUT.byteSize()) {
            throw new UnsupportedOperationException(
                "unsupported monitor status ABI " + abiVersion
                    + " with structure size "
                    + Integer.toUnsignedString(structSize));
        }
        return new MonitorStatus(
          abiVersion,
          structSize,
          EnumCodecs.monitorSourceKindFromValue(segment.get(ValueLayout.JAVA_INT,
            NativeLayouts.MONITOR_SNAPSHOT_SOURCE_KIND_OFFSET)),
          MonitorStateFlags.fromMask(segment.get(ValueLayout.JAVA_INT,
            NativeLayouts.MONITOR_SNAPSHOT_STATE_FLAGS_OFFSET)),
          MonitorStatusDetailFlags.fromMask(segment.get(ValueLayout.JAVA_INT,
            NativeLayouts.MONITOR_SNAPSHOT_DETAIL_FLAGS_OFFSET)),
          segment.get(ValueLayout.JAVA_LONG_UNALIGNED,
            NativeLayouts.MONITOR_SNAPSHOT_SND_PENDING_MSGS_OFFSET),
          segment.get(ValueLayout.JAVA_LONG_UNALIGNED,
            NativeLayouts.MONITOR_SNAPSHOT_RCV_PENDING_MSGS_OFFSET),
          segment.get(ValueLayout.JAVA_INT,
            NativeLayouts.MONITOR_SNAPSHOT_AUTO_HWM_ENABLED_OFFSET) != 0,
          EnumCodecs.autoHwmProfileFromValue(segment.get(ValueLayout.JAVA_INT,
            NativeLayouts.MONITOR_SNAPSHOT_AUTO_HWM_PROFILE_OFFSET)),
          segment.get(ValueLayout.JAVA_INT,
            NativeLayouts.MONITOR_SNAPSHOT_AUTO_HWM_ROLE_OFFSET),
          segment.get(ValueLayout.JAVA_INT,
            NativeLayouts.MONITOR_SNAPSHOT_AUTO_HWM_POLICY_CLASS_OFFSET),
          segment.get(ValueLayout.JAVA_LONG_UNALIGNED,
            NativeLayouts.MONITOR_SNAPSHOT_AUTO_HWM_UNIT_BUDGET_BYTES_OFFSET),
          segment.get(ValueLayout.JAVA_INT,
            NativeLayouts.MONITOR_SNAPSHOT_AUTO_HWM_SIZE_CAP_OFFSET),
          segment.get(ValueLayout.JAVA_LONG_UNALIGNED,
            NativeLayouts.MONITOR_SNAPSHOT_AUTO_HWM_SOCKET_MESSAGE_SLOTS_OFFSET),
          segment.get(ValueLayout.JAVA_INT,
            NativeLayouts.MONITOR_SNAPSHOT_AUTO_HWM_CONNECTION_BUCKET_ENABLED_OFFSET) != 0,
          segment.get(ValueLayout.JAVA_INT,
            NativeLayouts.MONITOR_SNAPSHOT_AUTO_HWM_CONNECTION_BUCKET_COUNT_OFFSET),
          segment.get(ValueLayout.JAVA_INT,
            NativeLayouts.MONITOR_SNAPSHOT_AUTO_HWM_CONNECTION_BUCKET_INDEX_OFFSET),
          segment.get(ValueLayout.JAVA_INT,
            NativeLayouts.MONITOR_SNAPSHOT_AUTO_HWM_CONNECTION_BUCKET_HWM_4K_OFFSET),
          segment.get(ValueLayout.JAVA_INT,
            NativeLayouts.MONITOR_SNAPSHOT_AUTO_HWM_CONNECTION_BUCKET_HYSTERESIS_RETAINED_OFFSET) != 0,
          segment.get(ValueLayout.JAVA_LONG_UNALIGNED,
            NativeLayouts.MONITOR_SNAPSHOT_AUTO_HWM_EFFECTIVE_MESSAGE_BYTES_OFFSET),
          segment.get(ValueLayout.JAVA_LONG_UNALIGNED,
            NativeLayouts.MONITOR_SNAPSHOT_AUTO_HWM_PLANNED_SNDHWM_BYTES_OFFSET),
          segment.get(ValueLayout.JAVA_LONG_UNALIGNED,
            NativeLayouts.MONITOR_SNAPSHOT_AUTO_HWM_PLANNED_RCVHWM_BYTES_OFFSET),
          segment.get(ValueLayout.JAVA_LONG_UNALIGNED,
            NativeLayouts.MONITOR_SNAPSHOT_AUTO_HWM_APPLIED_SNDHWM_BYTES_OFFSET),
          segment.get(ValueLayout.JAVA_LONG_UNALIGNED,
            NativeLayouts.MONITOR_SNAPSHOT_AUTO_HWM_APPLIED_RCVHWM_BYTES_OFFSET),
          segment.get(ValueLayout.JAVA_INT,
            NativeLayouts.MONITOR_SNAPSHOT_AUTO_HWM_EFFECTIVE_SNDBUF_OFFSET),
          segment.get(ValueLayout.JAVA_INT,
            NativeLayouts.MONITOR_SNAPSHOT_AUTO_HWM_EFFECTIVE_RCVBUF_OFFSET),
          segment.get(ValueLayout.JAVA_LONG_UNALIGNED,
            NativeLayouts.MONITOR_SNAPSHOT_AUTO_HWM_LAST_RECALC_MS_OFFSET),
          EnumCodecs.autoHwmRecalcReasonFromValue(segment.get(ValueLayout.JAVA_INT,
            NativeLayouts.MONITOR_SNAPSHOT_AUTO_HWM_LAST_RECALC_REASON_OFFSET)),
          segment.get(ValueLayout.JAVA_INT,
            NativeLayouts.MONITOR_SNAPSHOT_AUTO_HWM_SEND_BLOCKED_RATIO_PPM_OFFSET),
          segment.get(ValueLayout.JAVA_LONG_UNALIGNED,
            NativeLayouts.MONITOR_SNAPSHOT_AUTO_HWM_DEFERRED_SNDHWM_BYTES_OFFSET),
          segment.get(ValueLayout.JAVA_LONG_UNALIGNED,
            NativeLayouts.MONITOR_SNAPSHOT_AUTO_HWM_DEFERRED_RCVHWM_BYTES_OFFSET),
          segment.get(ValueLayout.JAVA_INT,
            NativeLayouts.MONITOR_SNAPSHOT_AUTO_HWM_DEFERRED_SNDHWM_VALID_OFFSET) != 0,
          segment.get(ValueLayout.JAVA_INT,
            NativeLayouts.MONITOR_SNAPSHOT_AUTO_HWM_DEFERRED_RCVHWM_VALID_OFFSET) != 0,
          segment.get(ValueLayout.JAVA_LONG_UNALIGNED,
            NativeLayouts.MONITOR_SNAPSHOT_SND_BYTES_IN_FLIGHT_OFFSET),
          segment.get(ValueLayout.JAVA_LONG_UNALIGNED,
            NativeLayouts.MONITOR_SNAPSHOT_RCV_BYTES_IN_FLIGHT_OFFSET),
          segment.get(ValueLayout.JAVA_LONG_UNALIGNED,
            NativeLayouts.MONITOR_SNAPSHOT_MINIMUM_CORE_MESSAGE_CHARGE_BYTES_OFFSET),
          segment.get(ValueLayout.JAVA_LONG_UNALIGNED,
            NativeLayouts.MONITOR_SNAPSHOT_OVERSIZE_MESSAGE_ADMISSION_COUNT_OFFSET),
          segment.get(ValueLayout.JAVA_LONG_UNALIGNED,
            NativeLayouts.MONITOR_SNAPSHOT_OVERSIZE_MESSAGE_ADMISSION_MAX_BYTES_OFFSET));
    }
}
