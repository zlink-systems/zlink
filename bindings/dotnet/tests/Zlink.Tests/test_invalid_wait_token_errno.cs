// SPDX-License-Identifier: MPL-2.0

using System.Diagnostics;
using System.Runtime.InteropServices;
using Xunit;

namespace Systems.Zlink.Tests;

public sealed class InvalidWaitTokenErrnoTests
{
    private const string ScenarioVariable = "ZLINK_DOTNET_INVALID_WAIT_TOKEN";
    private const string ShimSource = """
        #include <stdint.h>
        #include <stdlib.h>
        #include <string.h>
        #include "zlink_errno.h"

        static zlink_submit_result_t reject(int flags, uint64_t *id)
        {
            static unsigned calls;
            const char *scenario = getenv("ZLINK_DOTNET_INVALID_WAIT_TOKEN");
            if (flags != 1 || !id || !scenario)
                abort();
            int valid_wait = strstr(scenario, "retry") && calls == 0;
            *id = 71 + calls++;
            errno = valid_wait ? EAGAIN : EINVAL;
            return ZLINK_SUBMIT_BACKPRESSURED;
        }

        zlink_submit_result_t zlink_send(void *socket, void *parts, size_t count,
            int flags, void *context, uint64_t *id)
        {
            return reject(flags, id);
        }

        zlink_submit_result_t zlink_request(void *socket, const void *target,
            void *parts, size_t count, int flags, uint32_t timeout,
            void *context, uint64_t *id)
        {
            return reject(flags, id);
        }
        """;

    [Theory]
    [InlineData("send-first", false, false)]
    [InlineData("request-first", true, false)]
    [InlineData("send-retry", false, true)]
    [InlineData("request-retry", true, true)]
    public async Task Invalid_wait_token_releases_registration_and_staging(
        string scenario, bool request, bool retry)
    {
        if (!OperatingSystem.IsLinux())
            return;

        if (Environment.GetEnvironmentVariable(ScenarioVariable) is { } selected)
        {
            if (selected == scenario)
                await VerifySubmission(request, retry);
            return;
        }

        var directory = Directory.CreateTempSubdirectory("zlink-invalid-wait-token-");
        try
        {
            var core = Environment.GetEnvironmentVariable("ZLINK_LIBRARY_PATH")
                ?? Path.Combine(AppContext.BaseDirectory, "runtimes",
                    RuntimeInformation.RuntimeIdentifier, "native", "libzlink.so.0");
            var shim = Path.Combine(directory.FullName, "libinvalid_wait_token.so");
            var source = Path.Combine(directory.FullName, "invalid_wait_token.c");
            await File.WriteAllTextAsync(source, ShimSource);
            await Run(new ProcessStartInfo("cc")
            {
                ArgumentList =
                {
                    "-shared", "-fPIC", "-o", shim, source,
                    "-I", Path.Combine(AppContext.BaseDirectory, "Native"),
                    "-Wl,--no-as-needed", core, $"-Wl,-rpath,{Path.GetDirectoryName(core)}"
                }
            });
            await Run(new ProcessStartInfo("dotnet")
            {
                ArgumentList =
                {
                    "vstest", typeof(InvalidWaitTokenErrnoTests).Assembly.Location,
                    $"--TestCaseFilter:FullyQualifiedName={typeof(InvalidWaitTokenErrnoTests).FullName}.{nameof(Invalid_wait_token_releases_registration_and_staging)}"
                },
                Environment =
                {
                    [ScenarioVariable] = scenario,
                    ["ZLINK_LIBRARY_PATH"] = shim
                }
            });
        }
        finally
        {
            directory.Delete(recursive: true);
        }
    }

    private static async Task VerifySubmission(bool request, bool retry)
    {
        using var context = Zlink.CreateContext();
        using var socket = context.CreateDealerSocket();
        using var poller = Zlink.CreatePoller();
        poller.Add(socket, PollEventFlags.PollCompletion, 1);
        using var message = Message.From("caller payload");
        var owner = CompletionOwnerTestAccess.Owner(socket);

        Task? reply = null;
        Task Submit()
        {
            if (!request)
                return socket.Send().Message(message).Async().Admitted;
            var submission = socket.Request().Message(message).Async();
            reply = submission.Reply;
            return submission.Admitted;
        }

        if (retry)
        {
            Task admitted = Submit();
            Assert.False(admitted.IsCompleted);
            Assert.True(message.IsEmpty);
            object entry = Assert.Single(CompletionOwnerTestAccess.Entries(owner));
            var staging = Assert.IsType<Message[]>(
                CompletionOwnerTestAccess.Field(entry, "_retained"));
            Message stagedPart = Assert.Single(staging);
            Assert.Equal("caller payload", stagedPart.GetString());
            object completion = Activator.CreateInstance(
                CompletionOwnerTestAccess.RuntimeType("Systems.Zlink.Runtime.Native.ZlinkCompletion"))!;
            CompletionOwnerTestAccess.SetField(completion, "Kind", CompletionKind.Writable);
            CompletionOwnerTestAccess.SetField(completion, "CompletionId", 71UL);
            CompletionOwnerTestAccess.SetField(completion, "UserContext",
                CompletionOwnerTestAccess.Property(entry, "Context"));
            CompletionOwnerTestAccess.Invoke(entry, "Capture", completion);
            CompletionOwnerTestAccess.Invoke(owner, "Drain");

            Assert.True(admitted.IsCompleted, "An invalid token must not wait for WRITABLE.");
            var error = await Assert.ThrowsAsync<ZlinkSubmitException>(() => admitted);
            if (request)
            {
                Assert.True(reply!.IsCompleted);
                Assert.Same(error, await Assert.ThrowsAsync<ZlinkSubmitException>(() => reply));
            }
            AssertProtocolFailure(error);
            Assert.True(stagedPart.IsEmpty);
            Assert.True(message.IsEmpty);
        }
        else
        {
            AssertProtocolFailure(Assert.Throws<ZlinkSubmitException>(() => { _ = Submit(); }));
            Assert.Equal("caller payload", message.GetString());
        }
        Assert.Empty(CompletionOwnerTestAccess.Entries(owner));
    }

    private static void AssertProtocolFailure(ZlinkSubmitException error)
    {
        Assert.Equal(ZlinkSubmitException.ErrorCode.InternalError, error.Result);
        Assert.Equal(CompletionOwnerTestAccess.Errno("EProto"), error.NativeErrno);
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
