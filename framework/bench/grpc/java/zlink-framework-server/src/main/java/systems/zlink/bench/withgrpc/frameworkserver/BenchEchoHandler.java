/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.frameworkserver;

import com.google.protobuf.UnsafeByteOperations;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.bench.withgrpc.proto.BenchPayload;
import systems.zlink.bench.withgrpc.shared.BenchMetricHeader;
import systems.zlink.bench.withgrpc.shared.BenchServerMetrics;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.channels.ZLinkRouteRequestHandler;

/**
 * Request patterns return a 4096-byte payload carrying the request's flow header.
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
        return CompletableFuture.completedFuture(BenchPayload.newBuilder()
            .setBody(UnsafeByteOperations.unsafeWrap(BenchMetricHeader.createResponsePayload(
                request.getBody().asReadOnlyByteBuffer())))
            .build());
    }
}
