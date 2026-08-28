package systems.zlink.perf.multi;



import org.junit.jupiter.api.Test;
import systems.zlink.perf.PerfUtil;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

class PerfArgsRegressionTest {
    @Test
    void singleArgsRejectUnknownOption() {
        IllegalArgumentException failure = assertThrows(IllegalArgumentException.class,
            () -> PerfUtil.parseSingleArgs(new String[] {
                "PAIR", "tcp", "64", "--unknown", "1"
            }));

        assertTrue(failure.getMessage().contains("unknown option"));
    }

    @Test
    void multiArgsRejectMissingOptionValue() {
        IllegalArgumentException failure = assertThrows(IllegalArgumentException.class,
            () -> PerfUtil.parseMultiArgs(new String[] {
                "--multi-server", "MULTI_DEALER_DEALER", "tcp", "64",
                "--duration"
            }));

        assertTrue(failure.getMessage().contains("requires a value"));
    }

    @Test
    void bandwidthDirectionIsSuiteAware() {
        assertEquals("0.001", bandwidthValue("single", "DEALER_ROUTER"));
        assertEquals("0.001", bandwidthValue("single", "ROUTER_ROUTER"));
        assertEquals("0.002", bandwidthValue("single", "DEALER_ROUTER_REQREP"));
        assertEquals("0.002", bandwidthValue("multi", "DEALER_ROUTER_SENDSEND"));
    }

    private static String bandwidthValue(String suite, String pattern) {
        PerfUtil.Config config = new PerfUtil.Config(suite, pattern, "tcp",
            1_000, 1, "", 1, 1, 0, 0, 0, 0, 200, 200, 1_000,
            1_000, 128);
        PerfUtil.Metrics metrics = new PerfUtil.Metrics(config);
        metrics.recordEvent();
        PerfUtil.Result result = "single".equals(suite)
            ? metrics.finishSingle(config)
            : metrics.finishMulti(config);
        for (String line : result.toLine("current")
                 .split(System.lineSeparator())) {
            String[] fields = line.split(",", -1);
            if (fields.length == 7 && "bandwidth".equals(fields[5])) {
                return fields[6];
            }
        }
        throw new AssertionError("bandwidth RESULT line missing");
    }
}
