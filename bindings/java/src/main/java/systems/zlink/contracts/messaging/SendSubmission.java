/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.messaging;

import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.sockets.SubmitResult;

/** Snapshot and admission completion for an asynchronous send submission. */
public interface SendSubmission {
    /** Returns the immutable OK or BACKPRESSURED initial result. */
    SubmitResult result();

    /** Completes when the packet is admitted to the local send queue. */
    CompletionStage<Void> admitted();
}
