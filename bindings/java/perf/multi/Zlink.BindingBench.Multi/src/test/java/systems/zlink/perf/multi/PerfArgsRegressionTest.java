package systems.zlink.perf.multi;



import org.junit.jupiter.api.Test;
import systems.zlink.perf.PerfUtil;

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
}
