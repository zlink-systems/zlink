/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.frameworkserver;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.bench.withgrpc.proto.BenchPayload;
import systems.zlink.bench.withgrpc.shared.BenchServerMetrics;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.channels.ZLinkRouteRequestHandler;

/**
 * Request patterns echo the payload back
 * so the client can validate the 29-byte header it sent (G2).
 */
public final class BenchEchoHandler
    implements ZLinkRequestHandler<BenchPayload, BenchPayload>,
        ZLinkRouteRequestHandler<BenchPayload, BenchPayload> {
    private final BenchServerMetrics metrics;

    public BenchEchoHandler(BenchServerMetrics metrics) {
        this.metrics = metrics;
    }

    @Override
    public CompletionStage<BenchPayload> handle(
        BenchPayload request, ZLinkRouteMessageContext context) {
        return handle(request, (ZLinkMessageContext) context);
    }

    @Override
    public CompletionStage<BenchPayload> handle(
        BenchPayload request, ZLinkMessageContext context) {
        metrics.record(request.getBody().asReadOnlyByteBuffer());
        return CompletableFuture.completedFuture(request);
    }
}
