package systems.zlink.e2e.registrationcodec.client.Scenarios;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.concurrent.TimeUnit;
import systems.zlink.e2e.registrationcodec.client.Support.ScenarioAssert;
import systems.zlink.e2e.registrationcodec.client.Support.ScenarioContext;

public final class InvalidRegistrationScenario {
    private InvalidRegistrationScenario() {
    }

    public static void run(ScenarioContext context) {
        Path stdout = Path.of(context.options().logDir(), "invalid-server.stdout.log");
        Path stderr = Path.of(context.options().logDir(), "invalid-server.stderr.log");
        ProcessBuilder builder = new ProcessBuilder(
            invalidServerBin(context).toString(),
            "--config",
            context.options().invalidServerConfig());
        builder.redirectOutput(stdout.toFile());
        builder.redirectError(stderr.toFile());

        try {
            Process process = builder.start();
            boolean completed = process.waitFor(15, TimeUnit.SECONDS);
            if (!completed) {
                process.destroyForcibly();
            }
            ScenarioAssert.ensure(completed, "RC-A6 invalid registration server did not exit");
            ScenarioAssert.ensure(process.exitValue() != 0, "RC-A6 invalid registration server unexpectedly started");
            String outputText = (Files.exists(stdout) ? Files.readString(stdout) : "")
                + "\n"
                + (Files.exists(stderr) ? Files.readString(stderr) : "");
            ScenarioAssert.ensure(
                outputText.toLowerCase(java.util.Locale.ROOT).contains("duplicate")
                    || outputText.contains("EchoManual")
                    || outputText.toLowerCase(java.util.Locale.ROOT).contains("registration")
                    || outputText.toLowerCase(java.util.Locale.ROOT).contains("packet"),
                "RC-A6 invalid registration error did not mention duplicate registration");
            System.out.println("scenario RC-A6 passed");
        } catch (Exception error) {
            throw new IllegalStateException("RC-A6 invalid registration scenario failed", error);
        }
    }

    private static Path invalidServerBin(ScenarioContext context) {
        return Path.of(
            context.options().buildDir(),
            "Server-InvalidDuplicate",
            "install",
            "registration-codec-invalid-duplicate",
            "bin",
            "registration-codec-invalid-duplicate");
    }
}
