/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.frameworkserver;

import java.nio.ByteBuffer;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.bench.withgrpc.shared.BenchServerMetrics;
import systems.zlink.bench.withgrpc.shared.BenchTypedPayload;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.channels.ZLinkRouteSendHandler;

/** Typed JSON command handler used by ladder L5_7 through the public Framework route. */
public final class BenchTypedCommandHandler implements ZLinkRouteSendHandler<BenchTypedPayload> {
    private final BenchServerMetrics metrics;

    public BenchTypedCommandHandler(BenchServerMetrics metrics) {
        this.metrics = metrics;
    }

    @Override
    public CompletionStage<Void> handle(
            BenchTypedPayload message, ZLinkRouteMessageContext context) {
        metrics.record(ByteBuffer.wrap(message.body()).asReadOnlyBuffer());
        return CompletableFuture.completedFuture(null);
    }
}
