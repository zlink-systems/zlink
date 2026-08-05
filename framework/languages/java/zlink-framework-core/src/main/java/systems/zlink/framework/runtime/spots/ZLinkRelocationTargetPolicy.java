package systems.zlink.framework.runtime.spots;

import java.util.Objects;
import java.util.Optional;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationMode;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;

record ZLinkRelocationTargetPolicy(
    ZLinkFrameworkRelocationMode mode,
    long sourceApplicationVersion,
    Optional<String> sourceMaintenanceWave,
    long targetApplicationVersion) {
    ZLinkRelocationTargetPolicy {
        Objects.requireNonNull(mode, "mode");
        Objects.requireNonNull(sourceMaintenanceWave, "sourceMaintenanceWave");
        if (sourceApplicationVersion < 0 || targetApplicationVersion < 0) {
            throw new IllegalArgumentException(
                "application versions must not be negative");
        }
        if (mode == ZLinkFrameworkRelocationMode.PLANNED_MAINTENANCE
            && targetApplicationVersion != sourceApplicationVersion) {
            throw new IllegalArgumentException(
                "planned maintenance must keep the application version");
        }
        if (mode == ZLinkFrameworkRelocationMode.ROLLING_UPDATE
            && targetApplicationVersion <= sourceApplicationVersion) {
            throw new IllegalArgumentException(
                "rolling update target version must be greater than the source version");
        }
    }

    boolean acceptsVersion(ZLinkMeshNodeDescriptor candidate) {
        return candidate.applicationVersion() == targetApplicationVersion;
    }

    boolean acceptsWave(ZLinkMeshNodeDescriptor candidate) {
        return sourceMaintenanceWave
            .map(source -> candidate.maintenanceWave()
                .map(value -> !value.equals(source))
                .orElse(true))
            .orElse(true);
    }
}
