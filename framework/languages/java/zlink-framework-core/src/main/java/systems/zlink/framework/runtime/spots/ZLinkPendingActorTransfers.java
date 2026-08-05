package systems.zlink.framework.runtime.spots;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.TimeUnit;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.actors.ZLinkActorSpotRoutePackets;

final class ZLinkPendingActorTransfers {
    record Admission(
        ZLinkActorSpotRoutePackets.TransferRequest request,
        String routeChannelName,
        RoutingId sourcePeerRid,
        long expiresAtNanos) {
    }

    private final ConcurrentHashMap<String, Admission> admissions =
        new ConcurrentHashMap<>();

    void add(
        ZLinkActorSpotRoutePackets.TransferRequest request,
        String routeChannelName,
        RoutingId sourcePeerRid) {
        long timeoutMillis = Math.max(1L, request.timeoutMillis());
        Admission admission = new Admission(
            request,
            routeChannelName,
            sourcePeerRid,
            System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(timeoutMillis));
        Admission previous = admissions.putIfAbsent(request.transferId(), admission);
        if (previous != null) {
            throw new ZLinkConfigurationException(
                "duplicate actor transfer admission: " + request.transferId());
        }
        CompletableFuture.delayedExecutor(timeoutMillis, TimeUnit.MILLISECONDS)
            .execute(() -> admissions.remove(request.transferId(), admission));
    }

    Admission take(ZLinkActorSpotRoutePackets.TransferRequest commit) {
        Admission admission = admissions.remove(commit.transferId());
        if (admission == null
            || System.nanoTime() >= admission.expiresAtNanos()
            || !admission.request().actorId().equals(commit.actorId())
            || !admission.request().actorType().equals(commit.actorType())) {
            throw new ZLinkConfigurationException(
                "actor transfer admission is missing or expired: " + commit.transferId());
        }
        return admission;
    }
}
