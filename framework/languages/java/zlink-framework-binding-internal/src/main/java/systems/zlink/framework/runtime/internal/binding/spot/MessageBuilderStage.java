/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

import systems.zlink.contracts.messaging.Message;

/** Common stage for builders that add message parts. */
interface MessageBuilderStage<TSubmit> {
    TSubmit message(Message part);
}
