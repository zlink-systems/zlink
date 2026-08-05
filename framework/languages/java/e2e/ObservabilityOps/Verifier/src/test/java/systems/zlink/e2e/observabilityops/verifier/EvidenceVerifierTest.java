package systems.zlink.e2e.observabilityops.verifier;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

final class EvidenceVerifierTest {
    @TempDir
    Path temporaryDirectory;

    @Test
    void selectorContainsEveryConfig11Scenario() {
        assertEquals(13, EvidenceVerifier.scenarioIds("all").size());
        assertEquals("OBS-C5", EvidenceVerifier.scenarioIds("OBS-C5").getFirst());
    }

    @Test
    void missingEvidenceCannotBeReportedAsPass() {
        assertThrows(IllegalStateException.class,
            () -> new EvidenceVerifier().verify(temporaryDirectory, "OBS-A1"));
    }

    @Test
    void highCardinalityMetricTagFailsScenario() throws IOException {
        Files.writeString(temporaryDirectory.resolve("OBS-B3.json"), """
            {"scenario":"OBS-B3","dropObservable":false,"metrics":[
              {"name":"zlink.location.owner_lease.renew.lateness","kind":"histogram","unit":"s","value":1,
               "tags":{"flow_id":"forbidden"}}
            ]}
            """);
        assertThrows(IllegalStateException.class,
            () -> new EvidenceVerifier().verify(temporaryDirectory, "OBS-B3"));
    }
}
