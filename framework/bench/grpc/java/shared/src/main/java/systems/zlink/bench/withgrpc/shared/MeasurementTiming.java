/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.bench.withgrpc.shared;

import jdk.jfr.Event;
import jdk.jfr.Name;
import jdk.jfr.StackTrace;

/** Maps JFR timestamps to the driver's host-monotonic measurement windows. */
public final class MeasurementTiming {
    private MeasurementTiming() {}

    @Name("measure.MonotonicAnchor")
    @StackTrace(false)
    public static final class MonotonicAnchor extends Event {
        public long lowerNs;
        public long upperNs;
    }

    @Name("measure.Phase")
    @StackTrace(false)
    public static final class Phase extends Event {
        public String phase;
        public String cellId;
        public long startNs;
        public long durationNs;
    }

    public static void initialize() {
        new Phase().commit();
        MonotonicAnchor anchor = new MonotonicAnchor();
        anchor.lowerNs = BenchMetricHeader.nowNs();
        anchor.begin();
        anchor.upperNs = BenchMetricHeader.nowNs();
        anchor.commit();
    }

    public static void window(BenchHttpApplication.Trigger trigger, long startNs, long endNs) {
        Phase event = new Phase();
        event.phase = trigger.phase();
        event.cellId = trigger.cellId();
        event.startNs = startNs;
        event.durationNs = endNs - startNs;
        event.commit();
    }
}
