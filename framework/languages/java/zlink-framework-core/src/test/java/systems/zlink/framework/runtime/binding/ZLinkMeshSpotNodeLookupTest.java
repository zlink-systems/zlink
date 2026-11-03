package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertSame;

import jdk.jfr.Recording;
import jdk.jfr.consumer.RecordingFile;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.Zlink;

import java.nio.file.Files;

class ZLinkMeshSpotNodeLookupTest {
    @Test
    void spotNodeLookupDoesNotScheduleLaneOrCompletionTasks() throws Exception {
        ZLinkJavaIngressAllocationTest.runIsolated(ZLinkMeshSpotNodeLookupTest.class);
    }

    public static void main(String[] args) throws Exception {
        var output = Files.createTempFile("zlink-spot-node-lookup-", ".jfr");
        try (var context = Zlink.createContext();
                var node = new ZLinkJavaRawMeshNode(context, "spot-node-lookup");
                var recording = new Recording()) {
            var expected = node.spotNode();
            recording.enable("jdk.VirtualThreadStart");
            recording.start();
            for (int record = 0; record < 1000; record++) {
                assertSame(expected, node.spotNode());
            }
            recording.stop();
            recording.dump(output);
            long starts;
            try (var events = new RecordingFile(output)) {
                starts = 0;
                while (events.hasMoreEvents()) {
                    if (events.readEvent()
                            .getEventType()
                            .getName()
                            .equals("jdk.VirtualThreadStart")) {
                        starts++;
                    }
                }
            }
            System.out.println("spotNode lookups=1000 virtualThreadStarts=" + starts);
            assertEquals(0, starts, "A stable SpotNode reference needs no lane or completion task");
        } finally {
            Files.deleteIfExists(output);
        }
    }
}
