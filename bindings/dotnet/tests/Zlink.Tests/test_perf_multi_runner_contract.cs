using System.Diagnostics;
using System.Runtime.CompilerServices;
using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_perf_multi_runner_contract
{
    [Fact]
    public void build_dir_is_rejected_before_core_runtime_setup()
    {
        string runner = RunnerPath();
        string source = File.ReadAllText(runner);
        const string error = "--build-dir is not supported by the .NET multi perf runner";

        int rejection = source.IndexOf(error, StringComparison.Ordinal);
        int coreSetup = source.IndexOf(
            "source \"${REPO_DIR}/bindings/tools/local_core_runtime.sh\"",
            StringComparison.Ordinal);
        Assert.True(rejection >= 0);
        Assert.True(coreSetup > rejection);
        Assert.Contains("--build-dir|--build-dir=*)", source,
            StringComparison.Ordinal);
        Assert.DoesNotContain("\nBUILD_DIR=", source, StringComparison.Ordinal);

        if (OperatingSystem.IsWindows())
            return;

        ProcessResult result = RunRunner(runner, "--build-dir", "alternate-build");
        Assert.Equal(1, result.ExitCode);
        Assert.Contains(error, result.StandardError, StringComparison.Ordinal);
        Assert.DoesNotContain("dotnet build", result.StandardOutput,
            StringComparison.Ordinal);
    }

    [Fact]
    public void reuse_skips_all_builds_and_validates_required_outputs()
    {
        string source = File.ReadAllText(RunnerPath());
        string buildBody = FunctionBody(source, "ensure_build_output", "validate_reuse_outputs");
        string reuseBody = FunctionBody(source, "validate_reuse_outputs", "prepare_core_runtime");
        string streamBody = FunctionBody(source, "ensure_stream_client", "normalize_platform");

        Assert.True(buildBody.IndexOf("if [[ \"${REUSE_BUILD}\" -eq 1 ]]",
                StringComparison.Ordinal)
            < buildBody.IndexOf("dotnet build", StringComparison.Ordinal));
        Assert.Contains(".NET multi ${CONFIGURATION} perf output is missing",
            reuseBody, StringComparison.Ordinal);
        Assert.Contains("shared STREAM client is missing or not executable",
            reuseBody, StringComparison.Ordinal);
        Assert.True(streamBody.IndexOf("if [[ \"${REUSE_BUILD}\" -eq 1 ]]",
                StringComparison.Ordinal)
            < streamBody.IndexOf("cmake -S", StringComparison.Ordinal));

        int validation = source.IndexOf("validate_reuse_outputs\n",
            StringComparison.Ordinal);
        int resultDirectories = source.IndexOf("mkdir -p \"${RESULTS_ROOT}/multi/tmp\"",
            StringComparison.Ordinal);
        Assert.True(validation >= 0);
        Assert.True(resultDirectories > validation);

        if (OperatingSystem.IsWindows())
            return;

        ProcessResult result = RunRunner(
            RunnerPath(),
            new Dictionary<string, string?>
            {
                ["PERF_CONFIGURATION"] = "MissingRunnerContractOutput"
            },
            "--reuse-build", "--pattern", "DEALER_DEALER");
        Assert.Equal(1, result.ExitCode);
        Assert.Contains("--reuse-build requested but .NET multi " +
            "MissingRunnerContractOutput perf output is missing",
            result.StandardError, StringComparison.Ordinal);
        Assert.DoesNotContain("dotnet build", result.StandardOutput,
            StringComparison.Ordinal);
    }

    [Fact]
    public void clean_build_removes_only_fixed_perf_project_outputs()
    {
        string source = File.ReadAllText(RunnerPath());
        string buildBody = FunctionBody(source, "ensure_build_output", "validate_reuse_outputs");

        Assert.Contains(
            "rm -rf \"${PROJECT_DIR}/bin\" \"${PROJECT_DIR}/obj\"",
            buildBody, StringComparison.Ordinal);
        Assert.DoesNotContain("rm -rf \"${DOTNET_DIR}", buildBody,
            StringComparison.Ordinal);
        Assert.True(buildBody.IndexOf("rm -rf", StringComparison.Ordinal)
            < buildBody.IndexOf("dotnet build", StringComparison.Ordinal));
    }

    [Fact]
    public void reqrep_reply_result_is_not_changed_by_graceful_teardown()
    {
        string source = File.ReadAllText(ReqRepSourcePath());
        int replyStart = source.IndexOf(
            "private static bool ReplyReceived(Received received)",
            StringComparison.Ordinal);
        int replyEnd = source.IndexOf(
            "private static bool IsStaleRoute", replyStart,
            StringComparison.Ordinal);

        Assert.True(replyStart >= 0);
        Assert.True(replyEnd > replyStart);
        string replyBody = source[replyStart..replyEnd];
        Assert.DoesNotContain("CancellationToken", replyBody,
            StringComparison.Ordinal);
        Assert.DoesNotContain("IsCancellationRequested", replyBody,
            StringComparison.Ordinal);
        Assert.Contains("received.Reply()", replyBody,
            StringComparison.Ordinal);
        Assert.Contains(
            "catch (ZlinkSubmitException ex) when (IsStaleRoute(ex))",
            replyBody, StringComparison.Ordinal);
        Assert.EndsWith("return true;\n    }\n\n    ", replyBody,
            StringComparison.Ordinal);
        Assert.Contains(
            "error.Result == ZlinkSubmitException.ErrorCode.NotConnected",
            source, StringComparison.Ordinal);
        Assert.Contains(
            "error.Result == ZlinkSubmitException.ErrorCode.NotFound",
            source, StringComparison.Ordinal);
    }

    private static string FunctionBody(string source, string name, string nextName)
    {
        int start = source.IndexOf($"{name}() {{", StringComparison.Ordinal);
        int end = source.IndexOf($"{nextName}() {{", start, StringComparison.Ordinal);
        Assert.True(start >= 0, $"function not found: {name}");
        Assert.True(end > start, $"function boundary not found: {nextName}");
        return source[start..end];
    }

    private static ProcessResult RunRunner(string runner, params string[] arguments) =>
        RunRunner(runner, null, arguments);

    private static ProcessResult RunRunner(
        string runner,
        IReadOnlyDictionary<string, string?>? environment,
        params string[] arguments)
    {
        var start = new ProcessStartInfo("/bin/bash")
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false
        };
        start.ArgumentList.Add(runner);
        foreach (string argument in arguments)
            start.ArgumentList.Add(argument);
        if (environment != null)
        {
            foreach ((string key, string? value) in environment)
                start.Environment[key] = value;
        }

        using Process process = Process.Start(start)
            ?? throw new InvalidOperationException("failed to start .NET perf runner");
        string stdout = process.StandardOutput.ReadToEnd();
        string stderr = process.StandardError.ReadToEnd();
        Assert.True(process.WaitForExit(10_000), "perf runner validation timed out");
        return new ProcessResult(process.ExitCode, stdout, stderr);
    }

    private static string RunnerPath([CallerFilePath] string file = "") =>
        Path.GetFullPath(Path.Combine(
            Path.GetDirectoryName(file)!, "..", "..", "perf", "multi",
            "run_benchmarks.sh"));

    private static string ReqRepSourcePath(
        [CallerFilePath] string file = "") =>
        Path.GetFullPath(Path.Combine(
            Path.GetDirectoryName(file)!, "..", "..", "perf", "multi",
            "Zlink.BindingBench.Multi", "src", "PerfMultiSocketReqRep.cs"));

    private sealed record ProcessResult(
        int ExitCode,
        string StandardOutput,
        string StandardError);
}
