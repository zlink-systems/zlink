using System.Reflection;
using System.Runtime.InteropServices;
using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_writable_token_delivery
{
    [Fact]
    public void writable_wait_requires_backpressure_eagain_and_nonzero_token()
    {
        Type ownerType = CompletionOwnerTestAccess.RuntimeType(
            "Systems.Zlink.CompletionOwner");
        Type attemptType = ownerType.GetNestedType("SendAttempt",
            System.Reflection.BindingFlags.NonPublic)!;
        object missingErrno = CompletionOwnerTestAccess.Create(
            typeof(ZlinkSubmitException),
            ZlinkSubmitException.ErrorCode.Backpressured, 0);
        object wouldBlock = CompletionOwnerTestAccess.Create(
            typeof(ZlinkSubmitException),
            ZlinkSubmitException.ErrorCode.Backpressured, 11);

        object missingErrnoAttempt = CompletionOwnerTestAccess.Create(
            attemptType, 73UL, missingErrno);
        Assert.False((bool)CompletionOwnerTestAccess.InvokeStatic(ownerType,
            "IsWritableWait", missingErrnoAttempt)!);

        object writableWait = CompletionOwnerTestAccess.Create(
            attemptType, 73UL, wouldBlock);
        Assert.True((bool)CompletionOwnerTestAccess.InvokeStatic(ownerType,
            "IsWritableWait", writableWait)!);

        object tokenless = CompletionOwnerTestAccess.Create(attemptType,
            0UL, wouldBlock);
        Assert.False((bool)CompletionOwnerTestAccess.InvokeStatic(ownerType,
            "IsWritableWait", tokenless)!);
    }

    [Fact]
    public void backpressured_request_captures_errno_from_submitting_pinvoke()
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        Type nativeMethods = CompletionOwnerTestAccess.RuntimeType(
            "Systems.Zlink.Runtime.Native.NativeMethods");
        MethodInfo request = nativeMethods.GetMethod("zlink_request",
            BindingFlags.Static | BindingFlags.NonPublic)!;
        Assert.True(request.GetCustomAttribute<DllImportAttribute>()!
            .SetLastError);
        using var context = Zlink.CreateContext();
        using var dealer = context.CreateDealerSocket();
        dealer.Connect(CoreTestSupport.NewEndpoint(
            "inproc", "request-errno-capture"));
        object owner = CompletionOwnerTestAccess.Owner(dealer);

        using Message part = Message.From("waiting-for-route");
        object attempt = CompletionOwnerTestAccess.Invoke(owner,
            "SubmitRequest", null, new[] { part }, 3000u, 1,
            new IntPtr(73))!;

        Assert.NotEqual(0UL, CompletionOwnerTestAccess.Property(
            attempt, "CompletionId"));
        ZlinkSubmitException failure = Assert.IsType<ZlinkSubmitException>(
            CompletionOwnerTestAccess.Property(attempt, "Failure"));
        Assert.Equal(ZlinkSubmitException.ErrorCode.Backpressured,
            failure.Result);
        Assert.True(failure.NativeErrno is 11 or 35 or 10035,
            $"Expected EAGAIN, got {failure.NativeErrno}.");
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(false, true)]
    [InlineData(true, false)]
    [InlineData(true, true)]
    public async Task writable_delivers_core_result_to_matching_token(
        bool request, bool differentEcho)
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        Type ownerType = CompletionOwnerTestAccess.RuntimeType(
            "Systems.Zlink.CompletionOwner");
        object owner = CompletionOwnerTestAccess.Create(ownerType,
            IntPtr.Zero, SocketType.Router);
        CompletionOwnerTestAccess.Invoke(owner, "TransferToPublic", new object());
        RoutingId target = CoreTestSupport.RoutingIdUtf8("original-target");
        Type entryType = ownerType.GetNestedType(request
            ? "RequestCompletionEntry" : "SendCompletionEntry",
            System.Reflection.BindingFlags.NonPublic)!;
        object entry = request
            ? CompletionOwnerTestAccess.Create(entryType, owner, target,
                1000u, CancellationToken.None)
            : CompletionOwnerTestAccess.Create(entryType, owner, target,
                CancellationToken.None);
        CompletionOwnerTestAccess.Invoke(owner, "Register", entry,
            IntPtr.Zero, false);
        using Message part = Message.From("retained");
        const ulong token = 73;
        CompletionOwnerTestAccess.Invoke(entry, request ? "ArmWritable" : "Arm",
            token, new[] { part });
        Task pending = (Task)CompletionOwnerTestAccess.Property(entry, "Task");

        object completion = Activator.CreateInstance(
            CompletionOwnerTestAccess.RuntimeType(
                "Systems.Zlink.Runtime.Native.ZlinkCompletion"))!;
        CompletionOwnerTestAccess.SetField(completion, "Kind",
            CompletionKind.Writable);
        CompletionOwnerTestAccess.SetField(completion, "CompletionId", token);
        CompletionOwnerTestAccess.SetField(completion, "UserContext",
            CompletionOwnerTestAccess.Property(entry, "Context"));
        RoutingId echo = differentEcho
            ? CoreTestSupport.RoutingIdUtf8("unrelated") : target;
        CompletionOwnerTestAccess.SetField(completion, "PeerRoutingId",
            CompletionOwnerTestAccess.Invoke(echo, "ToNative")!);
        CompletionOwnerTestAccess.SetField(completion, "SendResult", 202);
        CompletionOwnerTestAccess.SetField(completion, "SendTerminalErrno", 2);

        // A nonconforming RID is deliberately injected to prove the binding
        // does not replace the Core terminal result after token/context lookup.
        CompletionOwnerTestAccess.Invoke(entry, "Capture", completion);

        ZlinkSubmitException error = await Assert.ThrowsAsync<ZlinkSubmitException>(
            () => pending);
        Assert.Equal(ZlinkSubmitException.ErrorCode.NotFound, error.Result);
        Assert.Equal(2, error.NativeErrno);
        Assert.Empty(CompletionOwnerTestAccess.Entries(owner));
    }
}
