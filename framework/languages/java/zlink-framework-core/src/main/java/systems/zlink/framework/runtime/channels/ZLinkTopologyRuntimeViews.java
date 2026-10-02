package systems.zlink.framework.runtime.channels;

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
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkTopologyRuntimeProjection;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkTopologyStatusSource;

import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.Flow;
import java.util.function.Supplier;

final class ZLinkClientServerRuntimeView implements ZLinkClientServerRuntime {
    private final ZLinkChannelSocketRegistry sockets;
    private final Supplier<ZLinkFrameworkRuntimeState> hostState;
    private final ZLinkTopologyStatusSource<ZLinkClientServerStatus> statuses;

    ZLinkClientServerRuntimeView(
            ZLinkChannelSocketRegistry sockets, Supplier<ZLinkFrameworkRuntimeState> hostState) {
        this.sockets = Objects.requireNonNull(sockets, "sockets");
        this.hostState = Objects.requireNonNull(hostState, "hostState");
        statuses =
                new ZLinkTopologyStatusSource<>(
                        this::buildSnapshot,
                        status ->
                                List.of(
                                        status.localRole(),
                                        status.state(),
                                        status.isReady(),
                                        status.readyTargetCount(),
                                        status.targets()),
                        (status, sequence) ->
                                new ZLinkClientServerStatus(
                                        status.channelName(),
                                        status.localRole(),
                                        status.state(),
                                        status.isReady(),
                                        status.readyTargetCount(),
                                        status.targets(),
                                        sequence,
                                        status.observedAt()),
                        ZLinkClientServerStatus::sequence,
                        status ->
                                status.state() == ZLinkTopologyState.STOPPED
                                        || status.state() == ZLinkTopologyState.FAILED,
                        status -> status.state() == ZLinkTopologyState.STOPPING);
        sockets.onTopologyChanged(statuses::signalAll);
    }

    @Override
    public ZLinkClientServerStatus snapshot(String channelName) {
        return statuses.snapshot(channelName);
    }

    private ZLinkClientServerStatus buildSnapshot(
            String channelName, ZLinkClientServerStatus previous) {
        ZLinkFrameworkRuntimeState currentHostState = hostState.get();
        if (previous != null
                && (currentHostState == ZLinkFrameworkRuntimeState.STOPPED
                        || currentHostState == ZLinkFrameworkRuntimeState.ERROR)) {
            return new ZLinkClientServerStatus(
                    channelName,
                    previous.localRole(),
                    ZLinkTopologyRuntimeProjection.hostState(currentHostState),
                    false,
                    previous.readyTargetCount(),
                    previous.targets(),
                    0,
                    Instant.now());
        }
        ChannelRegistration registration = requireChannel(channelName, ChannelKind.CLIENT_SERVER);
        boolean client = registration.clientServerClientEnabled();
        boolean server = registration.clientServerServerEnabled();
        ZLinkClientServerRole role =
                client && server
                        ? ZLinkClientServerRole.CLIENT_AND_SERVER
                        : client ? ZLinkClientServerRole.CLIENT : ZLinkClientServerRole.SERVER;
        List<ZLinkClientServerTargetStatus> targets =
                sockets.clientServerTargetSnapshots(channelName).stream()
                        .map(
                                target -> {
                                    var peer =
                                            ZLinkTopologyRuntimeProjection.snapshot(
                                                    target.nodeRid(),
                                                    target.hostState(),
                                                    target.ready(),
                                                    target.connecting(),
                                                    ZLinkTopologyReason.NO_READY_TARGET);
                                    return new ZLinkClientServerTargetStatus(
                                            peer.nodeRid(),
                                            target.weight(),
                                            peer.state(),
                                            peer.unavailableReason());
                                })
                        .toList();
        int readyTargetCount =
                Math.toIntExact(
                        targets.stream()
                                .filter(target -> target.state() == ZLinkPeerState.READY)
                                .filter(target -> target.weight() > 0)
                                .count());
        boolean hostServing = currentHostState == ZLinkFrameworkRuntimeState.SERVING;
        boolean ready = hostServing && readyTargetCount > 0;
        return new ZLinkClientServerStatus(
                channelName,
                role,
                ready
                        ? ZLinkTopologyState.READY
                        : hostServing
                                ? ZLinkTopologyState.DEGRADED
                                : ZLinkTopologyRuntimeProjection.hostState(currentHostState),
                ready,
                readyTargetCount,
                targets,
                0,
                Instant.now());
    }

    @Override
    public Flow.Publisher<ZLinkObservedStatus<ZLinkClientServerStatus>> observe(
            String channelName, int capacity) {
        return statuses.observe(channelName, capacity);
    }

    @Override
    public boolean isReady(String channelName) {
        return snapshot(channelName).isReady();
    }

    private ChannelRegistration requireChannel(String channelName, ChannelKind kind) {
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
    private final Supplier<ZLinkManualFanoutRuntime> manualRuntime;
    private final Supplier<ZLinkFrameworkRuntimeState> hostState;
    private final ZLinkTopologyStatusSource<ZLinkFanoutStatus> statuses;

    ZLinkFanoutRuntimeView(
            ZLinkChannelSocketRegistry sockets,
            Supplier<ZLinkFanoutLocationRuntime> locationRuntime,
            Supplier<ZLinkManualFanoutRuntime> manualRuntime,
            Supplier<ZLinkFrameworkRuntimeState> hostState) {
        this.sockets = Objects.requireNonNull(sockets, "sockets");
        this.locationRuntime = Objects.requireNonNull(locationRuntime, "locationRuntime");
        this.manualRuntime = Objects.requireNonNull(manualRuntime, "manualRuntime");
        this.hostState = Objects.requireNonNull(hostState, "hostState");
        statuses =
                new ZLinkTopologyStatusSource<>(
                        this::buildSnapshot,
                        status ->
                                List.of(
                                        status.state(),
                                        status.isReady(),
                                        status.readyPublisherCount(),
                                        status.publishers()),
                        (status, sequence) ->
                                new ZLinkFanoutStatus(
                                        status.channelName(),
                                        status.state(),
                                        status.isReady(),
                                        status.readyPublisherCount(),
                                        status.publishers(),
                                        sequence,
                                        status.observedAt()),
                        ZLinkFanoutStatus::sequence,
                        status ->
                                status.state() == ZLinkTopologyState.STOPPED
                                        || status.state() == ZLinkTopologyState.FAILED,
                        status -> status.state() == ZLinkTopologyState.STOPPING);
        sockets.onTopologyChanged(statuses::signalAll);
    }

    @Override
    public ZLinkFanoutStatus snapshot(String channelName) {
        return statuses.snapshot(channelName);
    }

    private ZLinkFanoutStatus buildSnapshot(String channelName, ZLinkFanoutStatus previous) {
        ZLinkFrameworkRuntimeState currentHostState = hostState.get();
        if (previous != null
                && (currentHostState == ZLinkFrameworkRuntimeState.STOPPED
                        || currentHostState == ZLinkFrameworkRuntimeState.ERROR)) {
            return new ZLinkFanoutStatus(
                    channelName,
                    ZLinkTopologyRuntimeProjection.hostState(currentHostState),
                    false,
                    previous.readyPublisherCount(),
                    previous.publishers(),
                    0,
                    Instant.now());
        }
        requireChannel(channelName);
        ZLinkFanoutLocationRuntime location = locationRuntime.get();
        List<ZLinkMeshPeerSnapshot> publishers = new ArrayList<>();
        if (location != null) {
            publishers.addAll(
                    location.publisherSnapshots(channelName).stream()
                            .map(
                                    publisher ->
                                            ZLinkTopologyRuntimeProjection.snapshot(
                                                    publisher.nodeRid(),
                                                    publisher.hostState(),
                                                    publisher.ready(),
                                                    publisher.connecting(),
                                                    ZLinkTopologyReason.NO_READY_PEER))
                            .toList());
        }
        ZLinkManualFanoutRuntime manual = manualRuntime.get();
        if (manual != null) {
            publishers.addAll(
                    manual.publisherSnapshots(channelName).stream()
                            .map(
                                    publisher ->
                                            ZLinkTopologyRuntimeProjection.snapshot(
                                                    publisher.nodeRid(),
                                                    ZLinkFrameworkRuntimeState.SERVING,
                                                    publisher.ready(),
                                                    publisher.connecting(),
                                                    ZLinkTopologyReason.NO_READY_PEER))
                            .toList());
        }
        int readyPublisherCount =
                Math.toIntExact(
                        publishers.stream()
                                .filter(publisher -> publisher.state() == ZLinkPeerState.READY)
                                .count());
        boolean hostServing = currentHostState == ZLinkFrameworkRuntimeState.SERVING;
        boolean ready = hostServing && readyPublisherCount > 0;
        return new ZLinkFanoutStatus(
                channelName,
                ready
                        ? ZLinkTopologyState.READY
                        : hostServing
                                ? ZLinkTopologyState.DEGRADED
                                : ZLinkTopologyRuntimeProjection.hostState(currentHostState),
                ready,
                readyPublisherCount,
                publishers,
                0,
                Instant.now());
    }

    @Override
    public Flow.Publisher<ZLinkObservedStatus<ZLinkFanoutStatus>> observe(
            String channelName, int capacity) {
        return statuses.observe(channelName, capacity);
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
