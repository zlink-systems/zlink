package systems.zlink.runtime.nativeapi;

import java.lang.foreign.MemoryLayout;
import java.lang.foreign.MemoryLayout.PathElement;
import java.lang.foreign.ValueLayout;

public final class NativeLayouts {
    private NativeLayouts() {}

    public static final MemoryLayout MESSAGE_LAYOUT = MemoryLayout
            .sequenceLayout(64, ValueLayout.JAVA_BYTE)
            .withByteAlignment(ValueLayout.ADDRESS.byteAlignment());

    public static final MemoryLayout ROUTING_ID_LAYOUT = MemoryLayout.structLayout(
            ValueLayout.JAVA_BYTE.withName("size"),
            MemoryLayout.sequenceLayout(255, ValueLayout.JAVA_BYTE).withName("data"));
    public static final long ROUTING_ID_SIZE_OFFSET = ROUTING_ID_LAYOUT.byteOffset(
            PathElement.groupElement("size"));
    public static final long ROUTING_ID_DATA_OFFSET = ROUTING_ID_LAYOUT.byteOffset(
            PathElement.groupElement("data"));

    public static final MemoryLayout SOCKET_MONITOR_OPEN_OPTIONS_LAYOUT =
            MemoryLayout.structLayout(
                    ValueLayout.JAVA_INT.withName("events"));
    public static final long SOCKET_MONITOR_OPEN_EVENTS_OFFSET =
            SOCKET_MONITOR_OPEN_OPTIONS_LAYOUT.byteOffset(
                    PathElement.groupElement("events"));

    public static final MemoryLayout MONITOR_SNAPSHOT_LAYOUT =
            MemoryLayout.structLayout(
                    ValueLayout.JAVA_INT.withName("abi_version"),
                    ValueLayout.JAVA_INT.withName("struct_size"),
                    ValueLayout.JAVA_INT.withName("source_kind"),
                    ValueLayout.JAVA_INT.withName("state_flags"),
                    ValueLayout.JAVA_INT.withName("detail_flags"),
                    MemoryLayout.paddingLayout(4),
                    ValueLayout.JAVA_LONG_UNALIGNED.withName("snd_pending_msgs"),
                    ValueLayout.JAVA_LONG_UNALIGNED.withName("rcv_pending_msgs"),
                    ValueLayout.JAVA_INT.withName("auto_hwm_enabled"),
                    ValueLayout.JAVA_INT.withName("auto_hwm_profile"),
                    ValueLayout.JAVA_INT.withName("auto_hwm_role"),
                    ValueLayout.JAVA_INT.withName("auto_hwm_policy_class"),
                    ValueLayout.JAVA_LONG_UNALIGNED.withName("auto_hwm_unit_budget_bytes"),
                    ValueLayout.JAVA_INT.withName("auto_hwm_size_cap"),
                    MemoryLayout.paddingLayout(4),
                    ValueLayout.JAVA_LONG_UNALIGNED.withName("auto_hwm_socket_message_slots"),
                    ValueLayout.JAVA_INT.withName("auto_hwm_connection_bucket_enabled"),
                    ValueLayout.JAVA_INT.withName("auto_hwm_connection_bucket_count"),
                    ValueLayout.JAVA_INT.withName("auto_hwm_connection_bucket_index"),
                    ValueLayout.JAVA_INT.withName("auto_hwm_connection_bucket_hwm_4k"),
                    ValueLayout.JAVA_INT.withName("auto_hwm_connection_bucket_hysteresis_retained"),
                    MemoryLayout.paddingLayout(4),
                    ValueLayout.JAVA_LONG_UNALIGNED.withName("auto_hwm_effective_message_bytes"),
                    ValueLayout.JAVA_LONG_UNALIGNED.withName("auto_hwm_planned_sndhwm_bytes"),
                    ValueLayout.JAVA_LONG_UNALIGNED.withName("auto_hwm_planned_rcvhwm_bytes"),
                    ValueLayout.JAVA_LONG_UNALIGNED.withName("auto_hwm_applied_sndhwm_bytes"),
                    ValueLayout.JAVA_LONG_UNALIGNED.withName("auto_hwm_applied_rcvhwm_bytes"),
                    ValueLayout.JAVA_INT.withName("auto_hwm_effective_sndbuf"),
                    ValueLayout.JAVA_INT.withName("auto_hwm_effective_rcvbuf"),
                    ValueLayout.JAVA_LONG_UNALIGNED.withName("auto_hwm_last_recalc_ms"),
                    ValueLayout.JAVA_INT.withName("auto_hwm_last_recalc_reason"),
                    ValueLayout.JAVA_INT.withName("auto_hwm_send_blocked_ratio_ppm"),
                    ValueLayout.JAVA_LONG_UNALIGNED.withName("auto_hwm_deferred_sndhwm_bytes"),
                    ValueLayout.JAVA_LONG_UNALIGNED.withName("auto_hwm_deferred_rcvhwm_bytes"),
                    ValueLayout.JAVA_INT.withName("auto_hwm_deferred_sndhwm_valid"),
                    ValueLayout.JAVA_INT.withName("auto_hwm_deferred_rcvhwm_valid"),
                    ValueLayout.JAVA_LONG_UNALIGNED.withName("snd_bytes_in_flight"),
                    ValueLayout.JAVA_LONG_UNALIGNED.withName("rcv_bytes_in_flight"),
                    ValueLayout.JAVA_LONG_UNALIGNED.withName("minimum_core_message_charge_bytes"),
                    ValueLayout.JAVA_LONG_UNALIGNED.withName("oversize_message_admission_count"),
                    ValueLayout.JAVA_LONG_UNALIGNED.withName("oversize_message_admission_max_bytes"));
    public static final long MONITOR_SNAPSHOT_ABI_VERSION_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("abi_version"));
    public static final long MONITOR_SNAPSHOT_STRUCT_SIZE_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("struct_size"));
    public static final long MONITOR_SNAPSHOT_SOURCE_KIND_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("source_kind"));
    public static final long MONITOR_SNAPSHOT_STATE_FLAGS_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("state_flags"));
    public static final long MONITOR_SNAPSHOT_DETAIL_FLAGS_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("detail_flags"));
    public static final long MONITOR_SNAPSHOT_SND_PENDING_MSGS_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("snd_pending_msgs"));
    public static final long MONITOR_SNAPSHOT_RCV_PENDING_MSGS_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("rcv_pending_msgs"));
    public static final long MONITOR_SNAPSHOT_AUTO_HWM_ENABLED_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("auto_hwm_enabled"));
    public static final long MONITOR_SNAPSHOT_AUTO_HWM_PROFILE_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("auto_hwm_profile"));
    public static final long MONITOR_SNAPSHOT_AUTO_HWM_ROLE_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("auto_hwm_role"));
    public static final long MONITOR_SNAPSHOT_AUTO_HWM_POLICY_CLASS_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("auto_hwm_policy_class"));
    public static final long MONITOR_SNAPSHOT_AUTO_HWM_UNIT_BUDGET_BYTES_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("auto_hwm_unit_budget_bytes"));
    public static final long MONITOR_SNAPSHOT_AUTO_HWM_SIZE_CAP_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("auto_hwm_size_cap"));
    public static final long MONITOR_SNAPSHOT_AUTO_HWM_SOCKET_MESSAGE_SLOTS_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("auto_hwm_socket_message_slots"));
    public static final long MONITOR_SNAPSHOT_AUTO_HWM_CONNECTION_BUCKET_ENABLED_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("auto_hwm_connection_bucket_enabled"));
    public static final long MONITOR_SNAPSHOT_AUTO_HWM_CONNECTION_BUCKET_COUNT_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("auto_hwm_connection_bucket_count"));
    public static final long MONITOR_SNAPSHOT_AUTO_HWM_CONNECTION_BUCKET_INDEX_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("auto_hwm_connection_bucket_index"));
    public static final long MONITOR_SNAPSHOT_AUTO_HWM_CONNECTION_BUCKET_HWM_4K_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("auto_hwm_connection_bucket_hwm_4k"));
    public static final long MONITOR_SNAPSHOT_AUTO_HWM_CONNECTION_BUCKET_HYSTERESIS_RETAINED_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("auto_hwm_connection_bucket_hysteresis_retained"));
    public static final long MONITOR_SNAPSHOT_AUTO_HWM_EFFECTIVE_MESSAGE_BYTES_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("auto_hwm_effective_message_bytes"));
    public static final long MONITOR_SNAPSHOT_AUTO_HWM_PLANNED_SNDHWM_BYTES_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("auto_hwm_planned_sndhwm_bytes"));
    public static final long MONITOR_SNAPSHOT_AUTO_HWM_PLANNED_RCVHWM_BYTES_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("auto_hwm_planned_rcvhwm_bytes"));
    public static final long MONITOR_SNAPSHOT_AUTO_HWM_APPLIED_SNDHWM_BYTES_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("auto_hwm_applied_sndhwm_bytes"));
    public static final long MONITOR_SNAPSHOT_AUTO_HWM_APPLIED_RCVHWM_BYTES_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("auto_hwm_applied_rcvhwm_bytes"));
    public static final long MONITOR_SNAPSHOT_AUTO_HWM_EFFECTIVE_SNDBUF_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("auto_hwm_effective_sndbuf"));
    public static final long MONITOR_SNAPSHOT_AUTO_HWM_EFFECTIVE_RCVBUF_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("auto_hwm_effective_rcvbuf"));
    public static final long MONITOR_SNAPSHOT_AUTO_HWM_LAST_RECALC_MS_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("auto_hwm_last_recalc_ms"));
    public static final long MONITOR_SNAPSHOT_AUTO_HWM_LAST_RECALC_REASON_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("auto_hwm_last_recalc_reason"));
    public static final long MONITOR_SNAPSHOT_AUTO_HWM_SEND_BLOCKED_RATIO_PPM_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("auto_hwm_send_blocked_ratio_ppm"));
    public static final long MONITOR_SNAPSHOT_AUTO_HWM_DEFERRED_SNDHWM_BYTES_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("auto_hwm_deferred_sndhwm_bytes"));
    public static final long MONITOR_SNAPSHOT_AUTO_HWM_DEFERRED_RCVHWM_BYTES_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("auto_hwm_deferred_rcvhwm_bytes"));
    public static final long MONITOR_SNAPSHOT_AUTO_HWM_DEFERRED_SNDHWM_VALID_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("auto_hwm_deferred_sndhwm_valid"));
    public static final long MONITOR_SNAPSHOT_AUTO_HWM_DEFERRED_RCVHWM_VALID_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("auto_hwm_deferred_rcvhwm_valid"));
    public static final long MONITOR_SNAPSHOT_SND_BYTES_IN_FLIGHT_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("snd_bytes_in_flight"));
    public static final long MONITOR_SNAPSHOT_RCV_BYTES_IN_FLIGHT_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("rcv_bytes_in_flight"));
    public static final long MONITOR_SNAPSHOT_MINIMUM_CORE_MESSAGE_CHARGE_BYTES_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("minimum_core_message_charge_bytes"));
    public static final long MONITOR_SNAPSHOT_OVERSIZE_MESSAGE_ADMISSION_COUNT_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("oversize_message_admission_count"));
    public static final long MONITOR_SNAPSHOT_OVERSIZE_MESSAGE_ADMISSION_MAX_BYTES_OFFSET =
            MONITOR_SNAPSHOT_LAYOUT.byteOffset(
                    PathElement.groupElement("oversize_message_admission_max_bytes"));

    public static final MemoryLayout MONITOR_EVENT_LAYOUT = MemoryLayout.structLayout(
            ValueLayout.JAVA_LONG_UNALIGNED.withName("event"),
            ValueLayout.JAVA_LONG_UNALIGNED.withName("value"),
            ROUTING_ID_LAYOUT.withName("routing_id"),
            MemoryLayout.sequenceLayout(256, ValueLayout.JAVA_BYTE).withName("local_addr"),
            MemoryLayout.sequenceLayout(256, ValueLayout.JAVA_BYTE).withName("remote_addr"));
    public static final long MONITOR_EVENT_OFFSET = MONITOR_EVENT_LAYOUT.byteOffset(
            PathElement.groupElement("event"));
    public static final long MONITOR_VALUE_OFFSET = MONITOR_EVENT_LAYOUT.byteOffset(
            PathElement.groupElement("value"));
    public static final long MONITOR_ROUTING_OFFSET = MONITOR_EVENT_LAYOUT.byteOffset(
            PathElement.groupElement("routing_id"));
    public static final long MONITOR_LOCAL_OFFSET = MONITOR_EVENT_LAYOUT.byteOffset(
            PathElement.groupElement("local_addr"));
    public static final long MONITOR_REMOTE_OFFSET = MONITOR_EVENT_LAYOUT.byteOffset(
            PathElement.groupElement("remote_addr"));

}
