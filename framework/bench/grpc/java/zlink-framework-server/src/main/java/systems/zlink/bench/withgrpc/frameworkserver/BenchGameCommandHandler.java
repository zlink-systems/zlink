/* SPDX-License-Identifier: Elastic-2.0 */

package systems.zlink.bench.withgrpc.frameworkserver;

import systems.zlink.bench.withgrpc.shared.BenchGamePayload;
import systems.zlink.bench.withgrpc.shared.BenchServerMetrics;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.channels.ZLinkRouteSendHandler;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

/** Server-side receive counter for the default typed JSON game-message row. */
public final class BenchGameCommandHandler implements ZLinkRouteSendHandler<BenchGamePayload> {
    private final BenchServerMetrics metrics;

    public BenchGameCommandHandler(BenchServerMetrics metrics) {
        this.metrics = metrics;
    }

    @Override
    public CompletionStage<Void> handle(
            BenchGamePayload message, ZLinkRouteMessageContext context) {
        metrics.record(message);
        return CompletableFuture.completedFuture(null);
    }
}
