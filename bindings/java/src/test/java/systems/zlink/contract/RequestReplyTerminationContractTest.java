/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contract;

import systems.zlink.TestSupport;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

public class RequestReplyTerminationContractTest {

    @Test
    public void dealerRequestRoundTripSubprocessExitsCleanly() throws Exception {
        TestSupport.assumeNative();

        Process process = launchProbe();
        try {
            boolean exited = process.waitFor(12, TimeUnit.SECONDS);
            String output = new String(process.getInputStream().readAllBytes(),
                StandardCharsets.UTF_8);
            assertTrue(exited,
                "request/reply subprocess hung:\n" + output);
            assertEquals(0, process.exitValue(),
                "request/reply subprocess failed:\n" + output);
        } finally {
            process.destroyForcibly();
            process.waitFor(5, TimeUnit.SECONDS);
        }
    }

    @Test
    public void routerCallbackRequestRoundTripSubprocessExitsCleanly() throws Exception {
        TestSupport.assumeNative();

        Process process = launchProbe("callback");
        try {
            boolean exited = process.waitFor(12, TimeUnit.SECONDS);
            String output = new String(process.getInputStream().readAllBytes(),
                StandardCharsets.UTF_8);
            assertTrue(exited,
                "request/reply subprocess hung:\n" + output);
            assertEquals(0, process.exitValue(),
                "request/reply subprocess failed:\n" + output);
        } finally {
            process.destroyForcibly();
            process.waitFor(5, TimeUnit.SECONDS);
        }
    }

    private static Process launchProbe(String... args) throws IOException {
        String[] command = new String[5 + args.length];
        command[0] = javaBinary();
        command[1] = "--enable-native-access=ALL-UNNAMED";
        command[2] = "-cp";
        command[3] = System.getProperty("java.class.path");
        command[4] = RequestReplyTerminationProbe.class.getName();
        System.arraycopy(args, 0, command, 5, args.length);
        return new ProcessBuilder(command)
            .redirectErrorStream(true)
            .start();
    }

    private static String javaBinary() {
        return Path.of(System.getProperty("java.home"), "bin", "java")
            .toString();
    }
}
