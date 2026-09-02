/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.multi;

import systems.zlink.perf.PerfUtil;

/** STREAM packet benchmark migration is deferred to Phase 7. */
final class PerfMultiStream {
    private PerfMultiStream() {
    }

    static PerfUtil.Result runServer(PerfUtil.Config config) {
        throw new UnsupportedOperationException(
            "MULTI_STREAM pull-receive benchmark is deferred to Phase 7");
    }

    static PerfUtil.Result runClient(PerfUtil.Config config) {
        throw new UnsupportedOperationException(
            "MULTI_STREAM uses the external raw stream client");
    }
}
