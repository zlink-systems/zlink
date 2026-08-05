package systems.zlink.framework.runtime.channels;

import java.time.Instant;
import java.util.List;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.Flow;
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.Supplier;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.monitoring.ZLinkClientServerRole;
import systems.zlink.framework.monitoring.ZLinkClientServerRuntime;
import systems.zlink.framework.monitoring.ZLinkClientServerStatus;
import systems.zlink.framework.monitoring.ZLinkClientServerTargetStatus;
import systems.zlink.framework.monitoring.ZLinkFanoutRuntime;
import systems.zlink.framework.monitoring.ZLinkFanoutStatus;
import systems.zlink.framework.monitoring.ZLinkMeshPeerSnapshot;
import systems.zlink.framework.monitoring.ZLinkObservedStatus;
import systems.zlink.framework.monitoring.ZLinkPeerState;
import systems.zlink.framework.monitoring.ZLinkTopologyReason;
import systems.zlink.framework.monitoring.ZLinkTopologyState;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkStatusPublisher;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;

final class ZLinkClientServerRuntimeView implements ZLinkClientServerRuntime {
    private final ZLinkChannelSocketRegistry sockets;
    private final Supplier<ZLinkFrameworkRuntimeState> hostState;
    private final AtomicLong sequence = new AtomicLong();

    ZLinkClientServerRuntimeView(
        ZLinkChannelSocketRegistry sockets,
        Supplier<ZLinkFrameworkRuntimeState> hostState) {
        this.sockets = Objects.requireNonNull(sockets, "sockets");
        this.hostState = Objects.requireNonNull(hostState, "hostState");
    }

    @Override
    public ZLinkClientServerStatus snapshot(String channelName) {
        ChannelRegistration registration = requireChannel(
            channelName, ChannelKind.CLIENT_SERVER);
        boolean client = registration.clientServerClientEnabled();
        boolean server = registration.clientServerServerEnabled();
        ZLinkClientServerRole role = client && server
            ? ZLinkClientServerRole.CLIENT_AND_SERVER
            : client
                ? ZLinkClientServerRole.CLIENT
                : ZLinkClientServerRole.SERVER;
        List<ZLinkClientServerTargetStatus> targets =
            sockets.clientServerTargetSnapshots(channelName).stream()
                .map(target -> new ZLinkClientServerTargetStatus(
                    target.nodeRid(),
                    target.weight(),
                    target.ready()
                        ? ZLinkPeerState.READY
                        : ZLinkPeerState.NOT_CONNECTED,
                    target.ready()
                        ? Optional.empty()
                        : Optional.of(ZLinkTopologyReason.NO_READY_TARGET)))
                .toList();
        int readyTargetCount = Math.toIntExact(targets.stream()
            .filter(target -> target.state() == ZLinkPeerState.READY)
            .filter(target -> target.weight() > 0)
            .count());
        ZLinkFrameworkRuntimeState currentHostState = hostState.get();
        boolean hostServing =
            currentHostState == ZLinkFrameworkRuntimeState.SERVING;
        boolean ready = hostServing
            && (server || !client || readyTargetCount > 0);
        return new ZLinkClientServerStatus(
            channelName,
            role,
            ready
                ? ZLinkTopologyState.READY
                : hostServing
                    ? ZLinkTopologyState.DEGRADED
                    : topologyState(currentHostState),
            ready,
            readyTargetCount,
            targets,
            sequence.incrementAndGet(),
            Instant.now());
    }

    private static ZLinkTopologyState topologyState(
        ZLinkFrameworkRuntimeState state) {
        return switch (state) {
            case PREPARING -> ZLinkTopologyState.STARTING;
            case SERVING -> ZLinkTopologyState.READY;
            case RELOCATING, RELOCATED, DRAINING -> ZLinkTopologyState.STOPPING;
            case STOPPED -> ZLinkTopologyState.STOPPED;
            case ERROR -> ZLinkTopologyState.FAILED;
        };
    }

    @Override
    public Flow.Publisher<ZLinkObservedStatus<ZLinkClientServerStatus>> observe(
        String channelName,
        int capacity) {
        requireChannel(channelName, ChannelKind.CLIENT_SERVER);
        return ZLinkStatusPublisher.create(
            () -> snapshot(channelName),
            status -> List.of(
                status.localRole(),
                status.state(),
                status.isReady(),
                status.readyTargetCount(),
                status.targets()),
            capacity,
            status -> status.state() == ZLinkTopologyState.STOPPED
                || status.state() == ZLinkTopologyState.FAILED,
            status -> status.state() == ZLinkTopologyState.STOPPING);
    }

    @Override
    public boolean isReady(String channelName) {
        return snapshot(channelName).isReady();
    }

    private ChannelRegistration requireChannel(
        String channelName,
        ChannelKind kind) {
        if (channelName == null || channelName.isBlank()) {
            throw new IllegalArgumentException("channelName is required");
        }
        ChannelRegistration registration = sockets.registration(channelName);
        if (registration == null || registration.kind() != kind) {
            throw new ZLinkConfigurationException(
                "ClientServer channel is not configured: " + channelName);
        }
        return registration;
    }
}

final class ZLinkFanoutRuntimeView implements ZLinkFanoutRuntime {
    private final ZLinkChannelSocketRegistry sockets;
    private final Supplier<ZLinkFanoutLocationRuntime> locationRuntime;
    private final Supplier<ZLinkFrameworkRuntimeState> hostState;
    private final AtomicLong sequence = new AtomicLong();

    ZLinkFanoutRuntimeView(
        ZLinkChannelSocketRegistry sockets,
        Supplier<ZLinkFanoutLocationRuntime> locationRuntime,
        Supplier<ZLinkFrameworkRuntimeState> hostState) {
        this.sockets = Objects.requireNonNull(sockets, "sockets");
        this.locationRuntime = Objects.requireNonNull(
            locationRuntime, "locationRuntime");
        this.hostState = Objects.requireNonNull(hostState, "hostState");
    }

    @Override
    public ZLinkFanoutStatus snapshot(String channelName) {
        requireChannel(channelName);
        ZLinkFanoutLocationRuntime location = locationRuntime.get();
        List<ZLinkMeshPeerSnapshot> publishers = location == null
            ? List.of()
            : location.publisherSnapshots(channelName).stream()
                .map(publisher -> new ZLinkMeshPeerSnapshot(
                    publisher.nodeRid(),
                    publisher.ready()
                        ? ZLinkPeerState.READY
                        : ZLinkPeerState.NOT_CONNECTED,
                    publisher.ready()
                        ? Optional.empty()
                        : Optional.of(ZLinkTopologyReason.NO_READY_PEER)))
                .toList();
        int readyPublisherCount = Math.toIntExact(publishers.stream()
            .filter(publisher -> publisher.state() == ZLinkPeerState.READY)
            .count());
        ZLinkFrameworkRuntimeState currentHostState = hostState.get();
        boolean hostServing =
            currentHostState == ZLinkFrameworkRuntimeState.SERVING;
        boolean ready = hostServing && readyPublisherCount > 0;
        return new ZLinkFanoutStatus(
            channelName,
            ready
                ? ZLinkTopologyState.READY
                : hostServing
                    ? ZLinkTopologyState.DEGRADED
                    : topologyState(currentHostState),
            ready,
            readyPublisherCount,
            publishers,
            sequence.incrementAndGet(),
            Instant.now());
    }

    private static ZLinkTopologyState topologyState(
        ZLinkFrameworkRuntimeState state) {
        return switch (state) {
            case PREPARING -> ZLinkTopologyState.STARTING;
            case SERVING -> ZLinkTopologyState.READY;
            case RELOCATING, RELOCATED, DRAINING -> ZLinkTopologyState.STOPPING;
            case STOPPED -> ZLinkTopologyState.STOPPED;
            case ERROR -> ZLinkTopologyState.FAILED;
        };
    }

    @Override
    public Flow.Publisher<ZLinkObservedStatus<ZLinkFanoutStatus>> observe(
        String channelName,
        int capacity) {
        requireChannel(channelName);
        return ZLinkStatusPublisher.create(
            () -> snapshot(channelName),
            status -> List.of(
                status.state(),
                status.isReady(),
                status.readyPublisherCount(),
                status.publishers()),
            capacity,
            status -> status.state() == ZLinkTopologyState.STOPPED
                || status.state() == ZLinkTopologyState.FAILED,
            status -> status.state() == ZLinkTopologyState.STOPPING);
    }

    private void requireChannel(String channelName) {
        if (channelName == null || channelName.isBlank()) {
            throw new IllegalArgumentException("channelName is required");
        }
        ChannelRegistration registration = sockets.registration(channelName);
        if (registration == null || registration.kind() != ChannelKind.FANOUT) {
            throw new ZLinkConfigurationException(
                "Automatic fanout channel is not configured: " + channelName);
        }
    }
}
