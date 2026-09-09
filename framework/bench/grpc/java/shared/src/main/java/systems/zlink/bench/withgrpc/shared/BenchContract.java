/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.shared;

/**
 * The channel, mesh and routing ids the {@code zlink-framework-<lang>} row uses.
 *
 * <p>spec section 3: gRPC and the ZLink framework use the same protobuf DTO. The request
 * handler and the send handler are registered for the same message type, as in the
 * .NET and node rows, so the two rows stay comparable.
 */
public final class BenchContract {
    public static final String MESH_NAME = "bench";
    public static final String CHANNEL_NAME = "bench";
    public static final String SERVER_ROUTING_ID = "bench-server";
    public static final String CLIENT_ROUTING_ID = "bench-client";

    private BenchContract() {
    }
}
