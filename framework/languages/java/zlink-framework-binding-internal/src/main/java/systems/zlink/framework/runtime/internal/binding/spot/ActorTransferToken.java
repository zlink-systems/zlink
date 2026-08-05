/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

import java.util.Arrays;
import java.util.Objects;

/**
 * An opaque, framework-owned handle for a prepared actor transfer fence.
 *
 * <p>The token is passed unchanged to commit, activate, or abort. Applications
 * must not inspect or persist its bytes.</p>
 */
public final class ActorTransferToken {
    private final byte[] opaque;

    /**
     * Creates a framework-owned token for backend adapter implementations.
     * Application code cannot access this internal package through the public
     * module or API classpath.
     */
    public ActorTransferToken(byte[] opaque) {
        this.opaque = Objects.requireNonNull(opaque, "opaque").clone();
    }

    byte[] opaque() {
        return opaque.clone();
    }
}
