/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.frameworkserver;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.bench.withgrpc.proto.BenchPayload;
import systems.zlink.bench.withgrpc.shared.BenchServerMetrics;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.channels.ZLinkRouteSendHandler;

/** spec section 5 / G3: the send row's throughput is this count, taken on the server. */
public final class BenchCommandHandler implements ZLinkRouteSendHandler<BenchPayload> {
    private final BenchServerMetrics metrics;

    public BenchCommandHandler(BenchServerMetrics metrics) {
        this.metrics = metrics;
        if (System.getenv("BENCH_DISPATCH_PROBE") != null) {
            System.err.println("[probe] BenchCommandHandler instantiated");
        }
    }

    @Override
    public CompletionStage<Void> handle(
        BenchPayload message, ZLinkRouteMessageContext context) {
        metrics.record(message.getBody().asReadOnlyByteBuffer());
        return CompletableFuture.completedFuture(null);
    }
}
