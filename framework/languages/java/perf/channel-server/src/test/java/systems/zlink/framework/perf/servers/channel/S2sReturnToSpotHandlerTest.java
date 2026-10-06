package systems.zlink.framework.perf.servers.channel;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.perf.Measurement;
import systems.zlink.framework.perf.PerfEchoRequest;
import systems.zlink.framework.perf.PerfValidationException;
import systems.zlink.framework.perf.RoleConfig;

import java.util.List;
import java.util.Map;

class S2sReturnToSpotHandlerTest {
    @Test
    void returnSpotMustMatchTheSourceClientMapping() {
        RoleConfig config =
                new RoleConfig(
                        "test",
                        "s2s-spot-to-channel-send-send-echo/4096/test",
                        "b".repeat(64),
                        "java",
                        "channel",
                        0,
                        "s2s-spot-to-channel-send-send-echo",
                        "send-send",
                        "ordinary",
                        "routemesh",
                        "ch",
                        "mesh",
                        Map.of(),
                        null,
                        "",
                        "",
                        false,
                        "ObjectClient",
                        true,
                        null,
                        List.of("spot-a", "spot-b"),
                        List.of(),
                        null,
                        null,
                        null,
                        "SpotWide",
                        new RoleConfig.Workload(
                                1024, .05, .05, null, 1, 1, null, 30000, 30000, 5000, 1000),
                        null,
                        Map.of());
        Measurement measurement = new Measurement(config, false);
        S2sReturnToSpotHandler handler = new S2sReturnToSpotHandler(measurement, null, config);
        PerfEchoRequest wrongAddress = measurement.request(1, 1, true).withReturnSpotId("spot-a");

        PerfValidationException error =
                assertThrows(
                        PerfValidationException.class, () -> handler.handle(wrongAddress, null));
        assertEquals("IdentityMismatch", error.kind());
    }
}
