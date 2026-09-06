/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.frameworkserver;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.bench.withgrpc.proto.BenchPayload;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;

/**
 * spec section 2: {@code request-serial} and {@code request-window} echo the payload back
 * so the client can validate the 29-byte header it sent (G2).
 */
public final class BenchEchoHandler
    implements ZLinkRequestHandler<BenchPayload, BenchPayload> {

    @Override
    public CompletionStage<BenchPayload> handle(
        BenchPayload request, ZLinkMessageContext context) {
        return CompletableFuture.completedFuture(request);
    }
}
