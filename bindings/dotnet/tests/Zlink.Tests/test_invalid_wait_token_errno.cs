// SPDX-License-Identifier: MPL-2.0

using System.Diagnostics;
using System.Runtime.InteropServices;
using Xunit;

namespace Systems.Zlink.Tests;

public sealed class InvalidWaitTokenErrnoTests
{
    private const string ScenarioVariable = "ZLINK_DOTNET_INVALID_WAIT_TOKEN";
    private static readonly TimeSpan DrainDeadline = TimeSpan.FromSeconds(2);
    private static readonly TimeSpan ScenarioDeadline = TimeSpan.FromSeconds(5);
    private const string ShimSource = """
        #include <dlfcn.h>
        #include <stdlib.h>
        #include <string.h>
        #include "zlink.h"

        static unsigned calls;
        static int closed;
        static void *rejected_context;
        static void *next_context;
        static void (*drain_before_return)(void);
        static zlink_completion_kind_t late_kind;
        static zlink_completion_kind_t queued_kind;
        static void *queued_context;
        static const zlink_completion_id_t token = 71;

        void fixture_prepare_late_record(zlink_completion_kind_t kind, void (*drain)(void))
        {
            late_kind = kind;
            drain_before_return = drain;
        }

        void fixture_queue_next_writable(void)
        {
            if (calls != 2 || queued_kind || !next_context)
                abort();
            queued_kind = ZLINK_COMPLETION_WRITABLE;
            queued_context = next_context;
        }

        int fixture_closed_records(void)
        {
            return closed;
        }

        zlink_recv_result_t zlink_completion_recv(void *socket,
            zlink_completion_t *completion, zlink_recv_flags_t flags)
        {
            if (flags != ZLINK_RECV_FLAGS_DONTWAIT || completion->struct_size != sizeof(*completion))
                abort();
            if (!queued_kind)
                return ZLINK_RECV_NO_DATA;
            completion->kind = queued_kind;
            completion->completion_id = token;
            completion->user_context = queued_context;
            queued_kind = 0;
            return ZLINK_RECV_OK;
        }

        void zlink_completion_close(zlink_completion_t *completion)
        {
            void (*close_record)(zlink_completion_t *) = dlsym(RTLD_NEXT, "zlink_completion_close");
            if (!close_record || completion->completion_id != token)
                abort();
            close_record(completion);
            ++closed;
        }

        static zlink_submit_result_t submit(zlink_msg_t *parts, size_t count,
            zlink_send_flags_t flags, void *context, zlink_completion_id_t *id)
        {
            const char *scenario = getenv("ZLINK_DOTNET_INVALID_WAIT_TOKEN");
            if (flags != ZLINK_SEND_FLAGS_DONTWAIT || !id || !scenario || !context)
                abort();
            zlink_multipart_close(parts, count);
            unsigned call = ++calls;
            if (drain_before_return) {
                *id = token;
                if (call == 1) {
                    rejected_context = context;
                    errno = EINVAL;
                    return ZLINK_SUBMIT_BACKPRESSURED;
                }
                if (call == 2) {
                    if (context == rejected_context)
                        abort();
                    next_context = context;
                    queued_kind = late_kind;
                    queued_context = rejected_context;
                    drain_before_return();
                    errno = EAGAIN;
                    return ZLINK_SUBMIT_BACKPRESSURED;
                }
                if (call != 3 || context != next_context || closed != 2)
                    abort();
                *id = 0;
                return ZLINK_SUBMIT_OK;
            }
            int valid_wait = strstr(scenario, "retry") && call == 1;
            *id = token + call - 1;
            errno = valid_wait ? EAGAIN : EINVAL;
            return ZLINK_SUBMIT_BACKPRESSURED;
        }

        zlink_submit_result_t zlink_send(void *socket, zlink_msg_t *parts, size_t count,
            zlink_send_flags_t flags, void *context, zlink_completion_id_t *id)
        {
            return submit(parts, count, flags, context, id);
        }

        zlink_submit_result_t zlink_request(void *socket, const zlink_routing_id_t *target,
            zlink_msg_t *parts, size_t count, zlink_send_flags_t flags, uint32_t timeout,
            void *context, zlink_completion_id_t *id)
        {
            return submit(parts, count, flags, context, id);
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

        await RunScenario(scenario, nameof(Invalid_wait_token_releases_registration_and_staging));
    }

    [Theory]
    [InlineData("send-late-writable", false, CompletionKind.Writable)]
    [InlineData("send-late-request", false, CompletionKind.Request)]
    [InlineData("request-late-writable", true, CompletionKind.Writable)]
    [InlineData("request-late-request", true, CompletionKind.Request)]
    public async Task Late_record_does_not_join_next_send(
        string scenario, bool request, CompletionKind lateKind)
    {
        if (!OperatingSystem.IsLinux())
            return;

        if (Environment.GetEnvironmentVariable(ScenarioVariable) is { } selected)
        {
            if (selected == scenario)
                await VerifyLateRecord(request, lateKind);
            return;
        }

        await RunScenario(scenario, nameof(Late_record_does_not_join_next_send));
    }

    private static async Task RunScenario(string scenario, string testMethod)
    {
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
                    "-Wl,--no-as-needed", core, $"-Wl,-rpath,{Path.GetDirectoryName(core)}",
                    "-ldl"
                }
            });
            await Run(new ProcessStartInfo("dotnet")
            {
                ArgumentList =
                {
                    "vstest", typeof(InvalidWaitTokenErrnoTests).Assembly.Location,
                    $"--TestCaseFilter:FullyQualifiedName={typeof(InvalidWaitTokenErrnoTests).FullName}.{testMethod}"
                },
                Environment =
                {
                    [ScenarioVariable] = scenario,
                    ["ZLINK_LIBRARY_PATH"] = shim
                }
            }, ScenarioDeadline);
        }
        finally
        {
            directory.Delete(recursive: true);
        }
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void DrainDuringSubmit();

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void PrepareLateRecord(CompletionKind kind, DrainDuringSubmit drain);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void QueueNextWritable();

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int ClosedRecords();

    private static async Task VerifyLateRecord(bool request, CompletionKind lateKind)
    {
        using var context = Zlink.CreateContext();
        using var socket = context.CreateDealerSocket();
        using var poller = Zlink.CreatePoller();
        poller.Add(socket, PollEventFlags.PollCompletion, 1);
        var owner = CompletionOwnerTestAccess.Owner(socket);
        IntPtr shim = NativeLibrary.Load(Environment.GetEnvironmentVariable("ZLINK_LIBRARY_PATH")!);
        try
        {
            T Export<T>(string name) where T : Delegate =>
                Marshal.GetDelegateForFunctionPointer<T>(NativeLibrary.GetExport(shim, name));

            var prepare = Export<PrepareLateRecord>("fixture_prepare_late_record");
            var queueNext = Export<QueueNextWritable>("fixture_queue_next_writable");
            var closed = Export<ClosedRecords>("fixture_closed_records");
            Exception? drainFailure = null;
            DrainDuringSubmit drain = () => drainFailure = Record.Exception(() =>
                Task.Run(() =>
                {
                    Assert.Single(CompletionOwnerTestAccess.Entries(owner));
                    object result = CompletionOwnerTestAccess.Invoke(owner, "Drain")!;
                    Assert.Equal(1, CompletionOwnerTestAccess.Property(result, "TotalCount"));
                    Assert.Equal(1, closed());
                }).WaitAsync(DrainDeadline).GetAwaiter().GetResult());
            prepare(lateKind, drain);

            using var rejected = Message.From("rejected");
            AssertProtocolFailure(Assert.Throws<ZlinkSubmitException>(() =>
            {
                if (request)
                    _ = socket.Request().Message(rejected).Async();
                else
                    _ = socket.Send().Message(rejected).Async();
            }));
            Assert.Equal("rejected", rejected.GetString());
            Assert.Empty(CompletionOwnerTestAccess.Entries(owner));
            Assert.Equal(0, closed());

            using var next = Message.From("next");
            var submission = socket.Send().Message(next).Async();
            GC.KeepAlive(drain);
            Assert.Null(drainFailure);
            Assert.Equal(1, closed());
            Assert.Equal(SubmitResult.Backpressured, submission.Result);
            Assert.False(submission.Admitted.IsCompleted);

            queueNext();
            CompletionOwnerTestAccess.Invoke(owner, "Drain");
            await submission.Admitted.WaitAsync(ScenarioDeadline);
            Assert.Equal(2, closed());
            Assert.Empty(CompletionOwnerTestAccess.Entries(owner));
        }
        finally
        {
            NativeLibrary.Free(shim);
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

    private static async Task Run(ProcessStartInfo start, TimeSpan? deadline = null)
    {
        start.RedirectStandardOutput = true;
        start.RedirectStandardError = true;
        using var process = Process.Start(start)!;
        var output = process.StandardOutput.ReadToEndAsync();
        var errors = process.StandardError.ReadToEndAsync();
        try
        {
            await process.WaitForExitAsync().WaitAsync(deadline ?? Timeout.InfiniteTimeSpan);
        }
        catch (TimeoutException exception)
        {
            if (!process.HasExited)
                process.Kill(entireProcessTree: true);
            await process.WaitForExitAsync();
            throw new TimeoutException(
                $"{start.FileName} exceeded the {deadline} scenario deadline.", exception);
        }
        Assert.True(process.ExitCode == 0, (await output) + (await errors));
    }
}
