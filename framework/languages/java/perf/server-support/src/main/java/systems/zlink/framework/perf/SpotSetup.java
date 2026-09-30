package systems.zlink.framework.perf;

import systems.zlink.framework.spots.SpotRef;
import systems.zlink.framework.spots.ZLinkSpotManager;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

// The Channel-side preparation of §10.3-§10.4: every SpotId of the cell is found through the public Spot manager before any
// probe. The manager's find is polled; the probe call itself is never retried.
public final class SpotSetup {
    private SpotSetup() {}

    /** Evidence of every found Spot: {spotId, meshName, nodeRid}. */
    public static CompletionStage<List<Object>> findAll(ZLinkSpotManager manager, RoleConfig config) {
        List<Object> found = new ArrayList<>();
        CompletableFuture<Void> chain = CompletableFuture.completedFuture(null);
        for (String spotId : config.spotIds()) {
            chain = chain.thenCompose(ignored -> Polling.untilAsync(() -> manager.find(spotId).thenApply(spot -> {
                if (spot.isEmpty()) {
                    return false;
                }
                SpotRef ref = spot.get();
                Map<String, Object> observed = new LinkedHashMap<>();
                observed.put("spotId", spotId);
                observed.put("meshName", ref.meshName());
                observed.put("nodeRid", ref.nodeRid().toHex());
                synchronized (found) {
                    found.add(observed);
                }
                return true;
            }), 10, config.workload().setupTimeoutMs()));
        }
        return chain.thenApply(ignored -> found);
    }
}
