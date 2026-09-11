package systems.zlink.framework.testkit;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Base64;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Properties;
import java.util.Set;
import java.util.concurrent.TimeUnit;
import java.util.regex.Pattern;
import org.junit.jupiter.api.Assumptions;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

final class WindowsSampleBatchLauncherContractTest {
    @TempDir
    Path temporaryRoot;

    @Test
    void batchLauncherPreservesGradleSuccessFallThroughAndFailureExitCodes() throws Exception {
        Assumptions.assumeTrue(System.getProperty("os.name").startsWith("Windows"));

        Path success = writeBatch("success.bat", """
            @echo off
            setlocal
            echo argument-one:%~1
            echo argument-two:%~2
            echo argument-three:%~3
            echo standard-error:%~1 1>&2
            cmd /c exit 0
            if %ERRORLEVEL% equ 0 goto mainEnd
            :fail
            exit /b %ERRORLEVEL%
            :mainEnd
            endlocal
            """);
        Path failure = writeBatch("failure.bat", "@exit /b 23\n");

        BatchResult successful = launchAndWait(
            success,
            "\"lifecycle path with spaces\"");
        assertEquals(0, successful.exitCode(), successful.output());
        String standardOutput = new String(
            Files.readAllBytes(batchLogPath("success.bat.out.log")), StandardCharsets.ISO_8859_1);
        String standardError = new String(
            Files.readAllBytes(batchLogPath("success.bat.err.log")), StandardCharsets.ISO_8859_1);
        assertTrue(standardOutput.contains("argument-one:lifecycle path with spaces"), standardOutput);
        assertTrue(standardError.contains("standard-error:lifecycle path with spaces"), standardError);
        BatchResult failing = launchAndWait(failure);
        assertEquals(23, failing.exitCode(), failing.output());
    }

    @Test
    void gradleStyleBatchForwardsQuotedAndShellArgumentsToJava() throws Exception {
        Assumptions.assumeTrue(System.getProperty("os.name").startsWith("Windows"));

        Path receivedArguments = temporaryRoot.resolve("received-arguments.txt");
        Path batch = writeBatch("forwarding.bat", """
            @echo off
            "%%JAVA_HOME%%\\bin\\java.exe" -cp "%%ZLINK_BATCH_RECORDER_CLASSPATH%%" %s %%*
            exit /b %%ERRORLEVEL%%
            """.formatted(ArgumentRecorder.class.getName()));

        BatchResult result = launchAndWait(
            batch,
            Map.of(
            "ZLINK_BATCH_RECORDER_CLASSPATH", System.getProperty("java.class.path"),
                "ZLINK_BATCH_ARGUMENT_OUTPUT", receivedArguments.toString()),
            "--key=\"value with spaces\"",
            "shell&metacharacter",
            "value%PATH%value",
            "bang!caret^value");

        assertEquals(0, result.exitCode(), result.output());
        List<String> recordedArguments = Files.readAllLines(receivedArguments, StandardCharsets.UTF_8);
        assertEquals(List.of(
                "--key=value with spaces",
                "shell&metacharacter",
                "value%PATH%value",
                "bang!caret^value"),
            recordedArguments, recordedArguments + "\n" + result.output());
    }

    @Test
    void genericProcessArgumentPreservesDockerGoTemplateQuoting() throws Exception {
        Assumptions.assumeTrue(System.getProperty("os.name").startsWith("Windows"));

        String template = "{{(index (index .NetworkSettings.Ports \"6379/tcp\") 0).HostPort}}";
        String expected = "\"" + template.replace("\"", "\\\"") + "\"";
        String script = """
            $ErrorActionPreference = 'Stop'
            . '%s'
            $converted = ConvertTo-ZlinkSampleProcessArgument '%s'
            if ($converted -cne '%s') { throw "docker template changed: $converted" }
            """.formatted(
                quoteForPowerShell(samplesRoot().resolve("redis-common.ps1")), template, expected);
        String encoded = Base64.getEncoder().encodeToString(script.getBytes(StandardCharsets.UTF_16LE));
        Process process = new ProcessBuilder(
            "powershell.exe", "-NoProfile", "-NonInteractive", "-EncodedCommand", encoded)
            .redirectErrorStream(true)
            .start();
        assertTrue(process.waitFor(10, TimeUnit.SECONDS), "docker template contract timed out");
        String output = new String(process.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
        assertEquals(0, process.exitValue(), output);
    }

    @Test
    void propertiesWriterRoundTripsWindowsReleasePath() throws Exception {
        Assumptions.assumeTrue(System.getProperty("os.name").startsWith("Windows"));

        Path releaseFile = temporaryRoot.resolve("release directory").resolve("owner-release");
        Files.createDirectories(releaseFile.getParent());
        Path propertiesFile = temporaryRoot.resolve("owner-unavailable.properties");
        String script = """
            $ErrorActionPreference = 'Stop'
            . '%s'
            Set-ZlinkSampleUtf8File -Path '%s' -Value @(
                'sample.ownerUnavailableReleaseFile=%s',
                'sample.logDirectory=%s')
            """.formatted(
                quoteForPowerShell(samplesRoot().resolve("redis-common.ps1")),
                quoteForPowerShell(propertiesFile),
                quoteForPowerShell(releaseFile),
                quoteForPowerShell(releaseFile.getParent()));
        String encoded = Base64.getEncoder().encodeToString(script.getBytes(StandardCharsets.UTF_16LE));
        Process process = new ProcessBuilder(
            "powershell.exe", "-NoProfile", "-NonInteractive", "-EncodedCommand", encoded)
            .redirectErrorStream(true)
            .start();
        assertTrue(process.waitFor(10, TimeUnit.SECONDS), "properties writer timed out");
        assertEquals(0, process.exitValue(), new String(process.getInputStream().readAllBytes(), StandardCharsets.UTF_8));

        Properties properties = new Properties();
        try (var reader = Files.newBufferedReader(propertiesFile, StandardCharsets.UTF_8)) {
            properties.load(reader);
        }
        Path parsedRelease = Path.of(properties.getProperty("sample.ownerUnavailableReleaseFile"))
            .toAbsolutePath().normalize();
        assertEquals(releaseFile.toAbsolutePath().normalize(), parsedRelease);
        Files.createFile(releaseFile);
        assertTrue(Files.exists(parsedRelease), parsedRelease.toString());
    }

    @Test
    void kotlinGameQuestStyleProcessStartInfoPreservesBatchExitAndArguments() throws Exception {
        Assumptions.assumeTrue(System.getProperty("os.name").startsWith("Windows"));

        Path receivedArgument = temporaryRoot.resolve("received-config.txt");
        Path success = writeBatch("direct-success.bat", """
            @echo off
            setlocal
            echo %~2 > "%ZLINK_DIRECT_ARGUMENT_OUTPUT%"
            cmd /c exit 0
            if %ERRORLEVEL% equ 0 goto mainEnd
            :fail
            exit /b %ERRORLEVEL%
            :mainEnd
            endlocal
            """);
        Path failure = writeBatch("direct-failure.bat", "@exit /b 23\n");

        BatchResult successful = launchWithProcessStartInfo(
            success,
            Map.of("ZLINK_DIRECT_ARGUMENT_OUTPUT", receivedArgument.toString()),
            "--config \"value with spaces\"");
        assertEquals(0, successful.exitCode(), successful.output());
        assertEquals("value with spaces", Files.readString(receivedArgument).trim());
        assertEquals(23, launchWithProcessStartInfo(failure, Map.of(), "").exitCode());
    }

    @Test
    void zoneWorldStylePowerShellLauncherPreservesBatchExitAndArguments() throws Exception {
        Assumptions.assumeTrue(System.getProperty("os.name").startsWith("Windows"));

        Path receivedArgument = temporaryRoot.resolve("zoneworld-config.txt");
        Path success = writeBatch("zoneworld-success.bat", """
            @echo off
            setlocal
            echo %~2 > "%ZLINK_ZONEWORLD_ARGUMENT_OUTPUT%"
            cmd /c exit 0
            if %ERRORLEVEL% equ 0 goto mainEnd
            :fail
            exit /b %ERRORLEVEL%
            :mainEnd
            endlocal
            """);
        Path failure = writeBatch("zoneworld-failure.bat", "@exit /b 23\n");

        BatchResult successful = launchWithZoneWorldPowerShell(
            success,
            Map.of("ZLINK_ZONEWORLD_ARGUMENT_OUTPUT", receivedArgument.toString()),
            "--config", "value with spaces");
        assertEquals(0, successful.exitCode(), successful.output());
        assertEquals("value with spaces", Files.readString(receivedArgument).trim());
        assertEquals(23, launchWithZoneWorldPowerShell(failure, Map.of()).exitCode());
    }

    @Test
    void allManifestSamplesUseAnExitCodeAndArgumentCoveredLauncher() throws Exception {
        Path samples = samplesRoot();
        String manifest = Files.readString(samples.resolve("sample-manifest.env"), StandardCharsets.UTF_8);
        Set<String> runners = new LinkedHashSet<>();
        for (String line : manifest.lines().toList()) {
            String[] assignment = line.split("=", 2);
            if (!assignment[0].equals("JAVA_SAMPLES") && !assignment[0].equals("KOTLIN_SAMPLES")) {
                continue;
            }
            String language = assignment[0].replace("_SAMPLES", "").toLowerCase();
            for (String sample : assignment[1].replace("\"", "").split(" ")) {
                runners.add(language + "/" + sample);
            }
        }
        assertEquals(14, runners.size(), runners.toString());
        Pattern directBatchInvocation = Pattern.compile(
            "(?m)^\\s*&\\s*(?:\\(Get-AppBin|\\$[A-Za-z]+Bin)");

        for (String runner : runners) {
            Path script = samples.resolve(runner).resolve("run_sample.ps1");
            String source = Files.readString(script, StandardCharsets.UTF_8);
            assertTrue(source.contains("redis-common.ps1"), script.toString());
            assertTrue(source.contains("Set-ZlinkSampleUtf8File") || source.contains("zoneworld-common.ps1"),
                "shared Java/Kotlin properties writer is not reachable from " + script);
            assertTrue(!directBatchInvocation.matcher(source).find(),
                "direct batch invocation in " + script);
            assertTrue(!source.contains("Start-Process"), "raw process launcher in " + script);
            if (source.contains("standalone.settings.gradle.kts")) {
                assertTrue(source.contains("Set-Location $SampleDir"),
                    "standalone Gradle runner must set its sample working directory: " + script);
            }
            if (runner.equals("java/GameQuest") || runner.equals("java/SupportChat")) {
                assertTrue(source.contains("Join-Path $LogDir \"$Role.err.log\""),
                    "failure cleanup must include role stderr: " + script);
                assertTrue(source.contains("Get-ChildItem $LogDir -Filter \"*.log\""),
                    "failure cleanup must read logs, not config files: " + script);
                assertTrue(!source.contains("Get-ChildItem $ConfigDir"),
                    "failure cleanup must not read config files: " + script);
            }
            if (runner.endsWith("/ZoneWorld")) {
                assertTrue(source.contains("zoneworld-common.ps1"), script.toString());
            } else if (runner.equals("kotlin/GameQuest")) {
                assertTrue(source.contains("[System.Diagnostics.ProcessStartInfo]::new()"), script.toString());
            } else {
                assertTrue(source.contains("Start-ZlinkSampleProcess"), script.toString());
            }
        }
        String zoneWorld = Files.readString(samples.resolve("zoneworld-common.ps1"), StandardCharsets.UTF_8);
        assertTrue(zoneWorld.contains("if ($null -ne $LASTEXITCODE) { exit $LASTEXITCODE }"));
    }

    @Test
    void javaShoppingMallPowerShellReadinessMatchesOwnedListeners() throws Exception {
        Path shoppingMall = samplesRoot().resolve("java/ShoppingMall");
        String powershellRunner = Files.readString(shoppingMall.resolve("run_sample.ps1"), StandardCharsets.UTF_8);
        String linuxRunner = Files.readString(shoppingMall.resolve("run_sample.sh"), StandardCharsets.UTF_8);
        Path program = shoppingMall.resolve(
            "Server/OrderWorkflow/src/main/java/systems/zlink/samples/shoppingmall/server/orderworkflow/Program.java");
        String workflowProgram = Files.readString(program, StandardCharsets.UTF_8);

        assertTrue(workflowProgram.contains("node.listen(workflow.spotRouterEndpoint())"));
        assertTrue(workflowProgram.contains("URI.create(topology.workflow().httpUrl())"));
        assertTrue(powershellRunner.contains("Wait-Port $workflowARouter.Host $workflowARouter.Port"));
        assertTrue(powershellRunner.contains("Wait-Port $workflowAHttp.Host $workflowAHttp.Port"));
        assertTrue(linuxRunner.contains("wait_port \"$workflow_a_router\""));
        assertTrue(linuxRunner.contains("wait_http \"$workflow_a_http\""));
        assertTrue(!powershellRunner.contains("Wait-Port $workflowAChannel.Host $workflowAChannel.Port"));
        assertTrue(!powershellRunner.contains("Wait-Port $workflowASpot.Host $workflowASpot.Port"));
    }

    @Test
    void zoneWorldPythonDiscoveryUsesCanonicalWindowsFallback() throws Exception {
        Assumptions.assumeTrue(System.getProperty("os.name").startsWith("Windows"));

        Path installedPython = Path.of(System.getenv("LOCALAPPDATA"), "Programs", "Python", "Python312", "python.exe");
        Assumptions.assumeTrue(Files.isRegularFile(installedPython));
        String script = """
            $ErrorActionPreference = 'Stop'
            . '%s'
            . '%s'
            $python = Resolve-ZlinkZoneWorldPython
            if ($python -cne '%s') { throw "unexpected python: $python" }
            """.formatted(
                quoteForPowerShell(samplesRoot().resolve("redis-common.ps1")),
                quoteForPowerShell(samplesRoot().resolve("zoneworld-common.ps1")),
                quoteForPowerShell(installedPython));
        String encoded = Base64.getEncoder().encodeToString(script.getBytes(StandardCharsets.UTF_16LE));
        Process process = new ProcessBuilder(
            "powershell.exe", "-NoProfile", "-NonInteractive", "-EncodedCommand", encoded)
            .redirectErrorStream(true)
            .start();
        assertTrue(process.waitFor(10, TimeUnit.SECONDS), "ZoneWorld Python discovery timed out");
        assertEquals(0, process.exitValue(), new String(process.getInputStream().readAllBytes(), StandardCharsets.UTF_8));

        String canonicalDiscovery = Files.readString(
            repositoryRoot().resolve("scripts/local-package/build-windows.ps1"),
            StandardCharsets.UTF_8);
        String zoneWorldDiscovery = Files.readString(samplesRoot().resolve("zoneworld-common.ps1"), StandardCharsets.UTF_8);
        assertTrue(canonicalDiscovery.contains("Join-Path $env:LOCALAPPDATA \"Programs\\Python\""));
        assertTrue(zoneWorldDiscovery.contains("Join-Path $env:LOCALAPPDATA \"Programs\\Python\""));
        assertTrue(zoneWorldDiscovery.contains("Resolve-Path -LiteralPath $pythonExecutable"));
    }

    private Path writeBatch(String name, String contents) throws Exception {
        Path batchDirectory = Files.createDirectories(temporaryRoot.resolve("batch files with spaces"));
        Path batch = batchDirectory.resolve(name);
        Files.writeString(batch, contents.replace("\n", "\r\n"), StandardCharsets.US_ASCII);
        return batch;
    }

    private BatchResult launchAndWait(Path batch, String... arguments) throws Exception {
        return launchAndWait(batch, Map.of(), arguments);
    }

    private BatchResult launchAndWait(
        Path batch,
        Map<String, String> environment,
        String... arguments) throws Exception {
        Path output = batchLogPath(batch.getFileName() + ".out.log");
        Path error = batchLogPath(batch.getFileName() + ".err.log");
        String script = """
            $ErrorActionPreference = 'Stop'
            . '%s'
            $process = Start-ZlinkSampleProcess -FilePath '%s' -ArgumentList @(%s) -WorkingDirectory '%s' `
                -StandardOutputPath '%s' -StandardErrorPath '%s'
            $process.WaitForExit()
            exit $process.ExitCode
            """.formatted(
                quoteForPowerShell(samplesRoot().resolve("redis-common.ps1")),
                quoteForPowerShell(batch),
                String.join(", ", java.util.Arrays.stream(arguments)
                    .map(argument -> "'" + argument.replace("'", "''") + "'")
                    .toList()),
                quoteForPowerShell(temporaryRoot),
                quoteForPowerShell(output),
                quoteForPowerShell(error));
        String encoded = Base64.getEncoder().encodeToString(script.getBytes(StandardCharsets.UTF_16LE));
        ProcessBuilder builder = new ProcessBuilder(
            "powershell.exe", "-NoProfile", "-NonInteractive", "-EncodedCommand", encoded)
            .redirectErrorStream(true);
        builder.environment().putAll(environment);
        Process process = builder.start();
        assertTrue(process.waitFor(10, TimeUnit.SECONDS), "batch launcher timed out");
        return new BatchResult(
            process.exitValue(),
            new String(process.getInputStream().readAllBytes(), StandardCharsets.UTF_8));
    }

    private BatchResult launchWithProcessStartInfo(
        Path batch,
        Map<String, String> environment,
        String arguments) throws Exception {
        String script = """
            $ErrorActionPreference = 'Stop'
            $startInfo = [Diagnostics.ProcessStartInfo]::new()
            $startInfo.FileName = '%s'
            $startInfo.WorkingDirectory = '%s'
            $startInfo.RedirectStandardOutput = $true
            $startInfo.RedirectStandardError = $true
            $startInfo.UseShellExecute = $false
            $startInfo.CreateNoWindow = $true
            $startInfo.Arguments = '%s'
            $process = [Diagnostics.Process]::new()
            $process.StartInfo = $startInfo
            $process.Start() | Out-Null
            $process.BeginOutputReadLine()
            $process.BeginErrorReadLine()
            $process.WaitForExit()
            exit $process.ExitCode
            """.formatted(
                quoteForPowerShell(batch),
                quoteForPowerShell(temporaryRoot),
                arguments.replace("'", "''"));
        String encoded = Base64.getEncoder().encodeToString(script.getBytes(StandardCharsets.UTF_16LE));
        ProcessBuilder builder = new ProcessBuilder(
            "powershell.exe", "-NoProfile", "-NonInteractive", "-EncodedCommand", encoded)
            .redirectErrorStream(true);
        builder.environment().putAll(environment);
        Process process = builder.start();
        assertTrue(process.waitFor(10, TimeUnit.SECONDS), "direct batch launcher timed out");
        return new BatchResult(
            process.exitValue(),
            new String(process.getInputStream().readAllBytes(), StandardCharsets.UTF_8));
    }

    private BatchResult launchWithZoneWorldPowerShell(
        Path batch,
        Map<String, String> environment,
        String... arguments) throws Exception {
        String quotedFilePath = "'" + quoteForPowerShell(batch) + "'";
        String quotedArguments = java.util.Arrays.stream(arguments)
            .map(argument -> "'" + argument.replace("'", "''") + "'")
            .collect(java.util.stream.Collectors.joining(" "));
        String script = """
            $ErrorActionPreference = 'Stop'
            & %s %s
            if ($null -ne $LASTEXITCODE) { exit $LASTEXITCODE }
            """.formatted(quotedFilePath, quotedArguments);
        String encoded = Base64.getEncoder().encodeToString(script.getBytes(StandardCharsets.UTF_16LE));
        ProcessBuilder builder = new ProcessBuilder(
            "powershell.exe", "-NoProfile", "-NonInteractive", "-EncodedCommand", encoded)
            .redirectErrorStream(true);
        builder.environment().putAll(environment);
        Process process = builder.start();
        assertTrue(process.waitFor(10, TimeUnit.SECONDS), "ZoneWorld batch launcher timed out");
        return new BatchResult(
            process.exitValue(),
            new String(process.getInputStream().readAllBytes(), StandardCharsets.UTF_8));
    }

    private static String quoteForPowerShell(Path path) {
        return path.toAbsolutePath().toString().replace("'", "''");
    }

    private Path batchLogPath(String name) throws Exception {
        return Files.createDirectories(temporaryRoot.resolve("batch logs with spaces")).resolve(name);
    }

    private static Path samplesRoot() {
        Path current = Path.of("").toAbsolutePath();
        for (Path candidate = current; candidate != null; candidate = candidate.getParent()) {
            Path samples = candidate.resolve("framework/languages/java/samples");
            if (Files.isRegularFile(samples.resolve("redis-common.ps1"))) {
                return samples;
            }
        }
        throw new IllegalStateException("cannot locate framework/languages/java/samples");
    }

    private static Path repositoryRoot() {
        Path current = Path.of("").toAbsolutePath();
        for (Path candidate = current; candidate != null; candidate = candidate.getParent()) {
            if (Files.isRegularFile(candidate.resolve("scripts/local-package/build-windows.ps1"))) {
                return candidate;
            }
        }
        throw new IllegalStateException("cannot locate repository root");
    }

    private record BatchResult(int exitCode, String output) {
    }

    public static final class ArgumentRecorder {
        public static void main(String[] arguments) throws Exception {
            Files.write(Path.of(System.getenv("ZLINK_BATCH_ARGUMENT_OUTPUT")), List.of(arguments));
        }
    }
}
