/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.messaging;

import java.time.Duration;

/** Common stage for builders that set a timeout and then submit. */
public interface TimeoutSubmitOperation<TSubmission> {
    TimeoutSubmitOperation<TSubmission> timeout(Duration timeout);

    TSubmission submit();
}
