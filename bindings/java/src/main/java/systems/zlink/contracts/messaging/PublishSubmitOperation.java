/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.messaging;

import systems.zlink.contracts.sockets.SendFlags;

/** Accepts further parts and synchronously submits a publish. */
public interface PublishSubmitOperation
  extends MessageBuilderStage<PublishSubmitOperation> {
    /** Adds another message part. */
    PublishSubmitOperation message(Message part);

    /** Sets the flags applied at submit time. */
    PublishSubmitOperation flags(SendFlags flags);

    /** Publishes immediately; failures are reported by exception. */
    void submit();
}
