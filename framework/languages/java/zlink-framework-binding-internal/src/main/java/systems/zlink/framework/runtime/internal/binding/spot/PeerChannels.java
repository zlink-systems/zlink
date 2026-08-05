/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

import java.util.List;

/** Channel names and weights advertised by one peer generation. */
public record PeerChannels(List<String> names, List<Integer> weights) {
    public PeerChannels {
        names = List.copyOf(names);
        weights = List.copyOf(weights);
        if (names.size() != weights.size()) {
            throw new IllegalArgumentException("names and weights must have the same size");
        }
    }
}
