/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.frameworkserver;

import java.nio.ByteBuffer;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.bench.withgrpc.shared.BenchServerMetrics;
import systems.zlink.bench.withgrpc.shared.BenchTypedPayload;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.channels.ZLinkRouteRequestHandler;

/** Typed JSON request handler used by ladder L5_7 through the public Framework route. */
public final class BenchTypedEchoHandler
        implements ZLinkRouteRequestHandler<BenchTypedPayload, BenchTypedPayload> {
    private final BenchServerMetrics metrics;

    public BenchTypedEchoHandler(BenchServerMetrics metrics) {
        this.metrics = metrics;
    }

    @Override
    public CompletionStage<BenchTypedPayload> handle(
            BenchTypedPayload request, ZLinkRouteMessageContext context) {
        metrics.record(ByteBuffer.wrap(request.body()).asReadOnlyBuffer());
        return CompletableFuture.completedFuture(request);
    }
}
