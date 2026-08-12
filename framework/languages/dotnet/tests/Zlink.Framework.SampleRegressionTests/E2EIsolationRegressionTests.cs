using System.Text.RegularExpressions;
using Xunit;

namespace Zlink.Framework.SampleRegressionTests;

public sealed partial class RegressionTests
{
    [Fact]
    public void DotnetE2ERunnersUseSeparatedCheckedPortsAndOneLanguageLock()
    {
        var e2eRoot = ResolveE2eRoot();
        var runners = Directory
            .EnumerateFiles(e2eRoot, "run_e2e.sh", SearchOption.AllDirectories)
            .Order(StringComparer.Ordinal)
            .ToArray();

        Assert.Equal(14, runners.Length);
        Assert.Empty(Directory.EnumerateFiles(
            e2eRoot,
            "*.ps1",
            SearchOption.AllDirectories));

        var redisRunnerCount = 0;
        foreach (var runner in runners)
        {
            var source = File.ReadAllText(runner);
            var relativePath = NormalizeRelativePath(
                Path.GetRelativePath(e2eRoot, runner));

            Assert.Contains("redis-common.sh", source,
                StringComparison.Ordinal);
            Assert.Single(Regex.Matches(
                    source,
                    Regex.Escape(
                        "zlink_dotnet_e2e_acquire_run_lock \"$0\" \"$@\""))
                .Cast<Match>());
            var lockOffset = source.IndexOf(
                "zlink_dotnet_e2e_acquire_run_lock \"$0\" \"$@\"",
                StringComparison.Ordinal);
            foreach (var resourceMarker in new[]
            {
                "RUN_ID=", "run_id=", "mktemp", "zlink_dotnet_e2e_allocate_ports",
                "zlink_redis_start_scoped", "docker create", "docker start"
            })
            {
                var resourceOffset = source.IndexOf(resourceMarker, StringComparison.Ordinal);
                Assert.True(resourceOffset < 0 || lockOffset < resourceOffset,
                    $"{relativePath} must acquire the whole-run lock before {resourceMarker}");
            }
            Assert.DoesNotContain(
                ".bind((\"127.0.0.1\", 0))",
                source,
                StringComparison.Ordinal);
            Assert.DoesNotContain("random.randint(", source,
                StringComparison.Ordinal);
            Assert.DoesNotMatch(
                @"127\.0\.0\.[0-9]+:[0-9]{2,5}",
                source);
            Assert.DoesNotContain("docker create", source,
                StringComparison.Ordinal);
            Assert.DoesNotContain("docker rm -fv", source,
                StringComparison.Ordinal);
            Assert.DoesNotContain("redis-cli -h", source,
                StringComparison.Ordinal);
            foreach (var line in source.Split('\n'))
            {
                if (!Regex.IsMatch(
                        line,
                        @"\bdocker\s+(?:pause|unpause|stop|start|inspect|exec)\b",
                        RegexOptions.CultureInvariant))
                    continue;
                Assert.Contains("timeout ", line, StringComparison.Ordinal);
            }

            if (relativePath == "InstanceSpot/run_e2e.sh")
                Assert.Contains("bash \"$SCRIPT_DIR/../SpotService/run_e2e.sh\"", source,
                    StringComparison.Ordinal);
            else
                Assert.Contains("zlink_dotnet_e2e_allocate_ports", source,
                    StringComparison.Ordinal);

            if (relativePath == "SpotService/run_e2e.sh")
                Assert.Contains("bash \"$SCRIPT_DIR/run_e2e.sh\" --all-child", source,
                    StringComparison.Ordinal);

            if (source.Contains(
                    "zlink_redis_start_scoped_assign",
                    StringComparison.Ordinal))
                redisRunnerCount++;
        }

        Assert.Equal(11, redisRunnerCount);

        var aggregate = File.ReadAllText(Path.Combine(
            e2eRoot,
            "run_e2e_all.sh"));
        Assert.DoesNotContain("zlink_dotnet_e2e_acquire_run_lock", aggregate,
            StringComparison.Ordinal);
        Assert.Contains("exec nice -n 10 timeout", aggregate,
            StringComparison.Ordinal);
        Assert.Contains("bash ./run_e2e.sh", aggregate,
            StringComparison.Ordinal);
        var childLaunchOffset = aggregate.IndexOf(
            "active_config_pid=\"$!\"", StringComparison.Ordinal);
        var childWaitOffset = aggregate.IndexOf(
            "wait \"$active_config_pid\"", childLaunchOffset,
            StringComparison.Ordinal);
        Assert.True(childLaunchOffset >= 0 && childLaunchOffset < childWaitOffset,
            "the aggregate must wait for each launched config before continuing");
        Assert.Contains("run_config \"$config\" \"$scenario\"", aggregate,
            StringComparison.Ordinal);
        Assert.DoesNotContain("run_config \"$config\" \"$scenario\" &", aggregate,
            StringComparison.Ordinal);

        foreach (var relativePath in new[]
        {
            "ResilienceLifecycle/Client/Support/ResilienceProcessManager.cs",
            "StoreFailure/Client/Support/StoreFailureProcessManager.cs",
            "RuntimeMonitoring/Client/Scenarios/MonA5FixedKindsScenario.cs"
        })
        {
            var source = File.ReadAllText(Path.Combine(
                e2eRoot,
                relativePath.Replace('/', Path.DirectorySeparatorChar)));
            Assert.Contains("WaitAsync(TimeSpan.FromSeconds(10))", source,
                StringComparison.Ordinal);
            Assert.Contains("process.Kill(entireProcessTree: true)", source,
                StringComparison.Ordinal);
            Assert.Contains("WaitAsync(TimeSpan.FromSeconds(2))", source,
                StringComparison.Ordinal);
            Assert.Contains("timed out after 10 seconds.", source,
                StringComparison.Ordinal);
        }

        var helper = File.ReadAllText(Path.Combine(
            e2eRoot,
            "redis-common.sh"));
        Assert.Contains("local lock_path=\"/tmp/zlink-dotnet-framework-e2e.lock\"", helper,
            StringComparison.Ordinal);
        Assert.DoesNotContain("TMPDIR", helper, StringComparison.Ordinal);
        Assert.Contains("ZLINK_DOTNET_E2E_RUN_LOCK_HELD", helper,
            StringComparison.Ordinal);
        Assert.Contains("ZLINK_DOTNET_E2E_PORT_REGISTRY_READY", helper,
            StringComparison.Ordinal);
        Assert.Contains("exec flock --close", helper,
            StringComparison.Ordinal);
        Assert.Contains("bash \"${runner}\" \"$@\"", helper,
            StringComparison.Ordinal);
        Assert.Contains("minimum_port = 32100", helper,
            StringComparison.Ordinal);
        Assert.Contains("maximum_port = 33999", helper,
            StringComparison.Ordinal);
        Assert.Contains("current.bind((\"127.0.0.1\", port))", helper,
            StringComparison.Ordinal);

        Assert.Contains("local redis_min_port=32000", helper,
            StringComparison.Ordinal);
        Assert.Contains("local redis_max_port=32099", helper,
            StringComparison.Ordinal);
        Assert.Contains("sock.bind((\"127.0.0.1\", int(sys.argv[1])))",
            helper,
            StringComparison.Ordinal);
        Assert.Contains("-p \"127.0.0.1:${port}:6379\"", helper,
            StringComparison.Ordinal);
        Assert.Contains("if [[ \"${host_port}\" != \"${port}\" ]]", helper,
            StringComparison.Ordinal);
        Assert.Contains("zlink_redis_is_bind_conflict", helper,
            StringComparison.Ordinal);
        Assert.Contains("zlink_redis_remove_attempt", helper,
            StringComparison.Ordinal);
        Assert.Contains("zlink_redis_remove_by_id", helper,
            StringComparison.Ordinal);
        Assert.Contains("[[ \"${container_id}\" =~ ^[0-9a-f]{12,64}$ ]] || return 1",
            helper,
            StringComparison.Ordinal);
        Assert.Contains("timeout -k 2s \"${docker_timeout_seconds}s\" docker rm -fv",
            helper,
            StringComparison.Ordinal);
        Assert.Contains("redis_port_retry port=%s stage=start", helper,
            StringComparison.Ordinal);
        Assert.DoesNotContain("127.0.0.1::6379", helper,
            StringComparison.Ordinal);
    }
}
