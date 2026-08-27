package systems.zlink.framework.execution;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import javax.tools.JavaCompiler;
import javax.tools.ToolProvider;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

final class ZLinkExecutionLanePolicyCompileTest {
    @TempDir
    Path temporaryDirectory;

    @Test
    void validLaneStatesCompile() throws IOException {
        assertTrue(compiles("""
            import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
            class ValidLaneStates {
                ZLinkExecutionLanePolicy.Spot spot =
                    ZLinkExecutionLanePolicy.Spot.RETURN_PENDING_RELOCATION_SEALED;
                ZLinkExecutionLanePolicy.Session session =
                    ZLinkExecutionLanePolicy.Session.CONNECTION_CLOSED;
                ZLinkExecutionLanePolicy.ActorDelivery actorDelivery =
                    ZLinkExecutionLanePolicy.ActorDelivery.ACTIVE;
            }
            """));
    }

    @Test
    void laneTypesRejectStatesFromOtherLanesAtCompileTime() throws IOException {
        assertFalse(compiles("""
            import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
            class InvalidLaneStates {
                ZLinkExecutionLanePolicy.Session session =
                    ZLinkExecutionLanePolicy.Spot.RELOCATION_SEALED;
                ZLinkExecutionLanePolicy.ActorDelivery actorDelivery =
                    ZLinkExecutionLanePolicy.Spot.RETURN_PENDING;
                ZLinkExecutionLanePolicy.Spot spot =
                    ZLinkExecutionLanePolicy.Session.CONNECTION_CLOSED;
            }
            """));
    }

    private boolean compiles(String source) throws IOException {
        Path sourceFile = temporaryDirectory.resolve("LanePolicyCheck.java");
        Files.writeString(sourceFile, source, StandardCharsets.UTF_8);
        JavaCompiler compiler = ToolProvider.getSystemJavaCompiler();
        if (compiler == null) {
            throw new IllegalStateException("JDK compiler is required for this test");
        }
        return compiler.run(
            null,
            null,
            null,
            "-classpath",
            System.getProperty("java.class.path"),
            "-d",
            temporaryDirectory.toString(),
            sourceFile.toString()) == 0;
    }
}
