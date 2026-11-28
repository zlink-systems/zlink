// SPDX-License-Identifier: MPL-2.0

using System.Diagnostics;
using Xunit;

namespace Systems.Zlink.Tests;

public sealed class SubscriptionAtTests
{
    private const string ScenarioVariable = "ZLINK_DOTNET_SUBSCRIPTION_CASE";
    private const string TraceVariable = "ZLINK_DOTNET_SUBSCRIPTION_TRACE";
    private const int LongFilterSize = 4096;
    private const string TestName = nameof(Core_calls_follow_subscription_query_contract);

    [Theory]
    [InlineData("missing", false, 1, ZlinkConfigException.ErrorCode.NotFound)]
    [InlineData("empty", true, 0, ZlinkConfigException.ErrorCode.Ok)]
    [InlineData("long", false, 0, ZlinkConfigException.ErrorCode.BufferTooSmall, ZlinkConfigException.ErrorCode.Ok)]
    [InlineData("removed-before-retry", true, 0, ZlinkConfigException.ErrorCode.BufferTooSmall, ZlinkConfigException.ErrorCode.NotFound)]
    [InlineData("invalid-argument", false, 0, ZlinkConfigException.ErrorCode.InvalidArgument)]
    public async Task Core_calls_follow_subscription_query_contract(
        string scenario, bool xsub, int index, ZlinkConfigException.ErrorCode first, ZlinkConfigException.ErrorCode? retry = null)
    {
        if (!OperatingSystem.IsLinux())
            return;

        if (Environment.GetEnvironmentVariable(ScenarioVariable) is { } childScenario)
        {
            if (scenario == childScenario)
                VerifyQuery(xsub, index, first, retry);
            return;
        }

        var directory = Directory.CreateTempSubdirectory("zlink-subscription-");
        try
        {
            var core = Environment.GetEnvironmentVariable("ZLINK_LIBRARY_PATH")
                ?? Path.Combine(AppContext.BaseDirectory, "runtimes",
                    System.Runtime.InteropServices.RuntimeInformation.RuntimeIdentifier,
                    "native", "libzlink.so.0");
            var shim = Path.Combine(directory.FullName, "libsubscription_at_shim.so");
            await Run(new ProcessStartInfo("cc")
            {
                ArgumentList =
                {
                    "-shared", "-fPIC", "-o", shim,
                    Path.Combine(AppContext.BaseDirectory, "Native", "subscription_at_shim.c"),
                    "-Wl,--no-as-needed", core, $"-Wl,-rpath,{Path.GetDirectoryName(core)}",
                    "-ldl"
                }
            });

            var trace = Path.Combine(directory.FullName, "core-calls.txt");
            var test = new ProcessStartInfo("dotnet")
            {
                ArgumentList =
                {
                    "vstest", Path.Combine(AppContext.BaseDirectory, "Systems.Zlink.Tests.dll"),
                    $"--TestCaseFilter:FullyQualifiedName={typeof(SubscriptionAtTests).FullName}.{TestName}"
                },
                Environment =
                {
                    [ScenarioVariable] = scenario,
                    [TraceVariable] = trace,
                    ["ZLINK_LIBRARY_PATH"] = shim
                }
            };
            await Run(test);
            Assert.True(File.Exists(trace), "The isolated test did not call the Core shim.");
        }
        finally
        {
            directory.Delete(recursive: true);
        }
    }

    private static void VerifyQuery(
        bool xsub, int index, ZlinkConfigException.ErrorCode first, ZlinkConfigException.ErrorCode? retry)
    {
        using var context = Zlink.CreateContext();
        using ISubscriberSocket socket = xsub ? context.CreateXSubSocket() : context.CreateSubSocket();
        var filter = retry.HasValue ? new string('x', LongFilterSize) : string.Empty;
        socket.SetSubscription(filter);

        var trace = Environment.GetEnvironmentVariable(TraceVariable)!;
        if (first == ZlinkConfigException.ErrorCode.InvalidArgument)
        {
            var error = Assert.Throws<ZlinkConfigException>(() => socket.SubscriptionAt(index));
            Assert.Equal(ZlinkConfigException.ErrorCode.InvalidArgument, error.Result);
            Assert.Equal(CoreTestSupport.Errno("EINVAL"), error.NativeErrno);
        }
        else
        {
            var expected = (retry ?? first) == ZlinkConfigException.ErrorCode.NotFound
                ? null
                : new SubscriptionEntry(filter, false);
            Assert.Equal(expected, socket.SubscriptionAt(index));
        }

        var calls = File.ReadLines(trace).Select(line =>
        {
            var fields = line.Split(' ');
            return (Capacity: int.Parse(fields[0]), Result: (ZlinkConfigException.ErrorCode)int.Parse(fields[1]));
        }).ToArray();
        Assert.Equal(retry.HasValue ? new[] { first, retry.Value } : new[] { first },
            calls.Select(call => call.Result));
        Assert.True(calls[0].Capacity > 0, "The first Core call must supply a buffer.");
        if (retry.HasValue)
            Assert.Equal(LongFilterSize, calls[1].Capacity);
    }

    private static async Task Run(ProcessStartInfo start)
    {
        start.RedirectStandardOutput = true;
        start.RedirectStandardError = true;
        using var process = Process.Start(start)!;
        var output = process.StandardOutput.ReadToEndAsync();
        var errors = process.StandardError.ReadToEndAsync();
        await process.WaitForExitAsync();
        Assert.True(process.ExitCode == 0, (await output) + (await errors));
    }
}
