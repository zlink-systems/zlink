/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.frameworkserver;

import systems.zlink.bench.withgrpc.shared.BenchGamePayload;
import systems.zlink.bench.withgrpc.shared.BenchServerMetrics;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.channels.ZLinkRouteRequestHandler;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

/** Echo handler for the default typed JSON game-message row. */
public final class BenchGameEchoHandler
        implements ZLinkRequestHandler<BenchGamePayload, BenchGamePayload>,
                ZLinkRouteRequestHandler<BenchGamePayload, BenchGamePayload> {
    private final BenchServerMetrics metrics;

    public BenchGameEchoHandler(BenchServerMetrics metrics) {
        this.metrics = metrics;
    }

    @Override
    public CompletionStage<BenchGamePayload> handle(
            BenchGamePayload request, ZLinkRouteMessageContext context) {
        return handle(request, (ZLinkMessageContext) context);
    }

    @Override
    public CompletionStage<BenchGamePayload> handle(
            BenchGamePayload request, ZLinkMessageContext context) {
        metrics.record(request);
        return CompletableFuture.completedFuture(request);
    }
}
