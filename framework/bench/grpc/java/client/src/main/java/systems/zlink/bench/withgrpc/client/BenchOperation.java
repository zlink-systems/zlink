/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.client;

import java.util.concurrent.CompletableFuture;

/** One request or one send, in whichever stack the cell is measuring. */
@FunctionalInterface
public interface BenchOperation {
    CompletableFuture<Void> invoke(int payloadSize, byte phase, long sequence);
}
