using System.Reflection;
using System.Runtime.InteropServices;
using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_writable_token_delivery
{
    [Fact]
    public void writable_wait_requires_backpressure_result_and_nonzero_token()
    {
        Type ownerType = CompletionOwnerTestAccess.RuntimeType(
            "Systems.Zlink.CompletionOwner");
        Type attemptType = ownerType.GetNestedType("SubmitAttempt",
            BindingFlags.NonPublic)!;
        int wouldBlock = CompletionOwnerTestAccess.Errno("EAgain");

        // Core returned the typed BACKPRESSURED result; the errno is diagnostic
        // only and never reclassifies it.
        Assert.True(IsWritableWait(73UL, SubmitResult.Backpressured, 0));
        Assert.False(IsWritableWait(73UL, SubmitResult.NotFound, wouldBlock));
        Assert.True(IsWritableWait(73UL, SubmitResult.Backpressured, wouldBlock));
        Assert.False(IsWritableWait(0UL, SubmitResult.Backpressured, wouldBlock));

        bool IsWritableWait(ulong id, SubmitResult result, int errno)
        {
            object attempt = CompletionOwnerTestAccess.Create(
                attemptType, id, result, errno, null);
            return (bool)CompletionOwnerTestAccess.InvokeStatic(ownerType,
                "IsWritableWait", attempt)!;
        }
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
        Assert.Equal(SubmitResult.Backpressured,
            CompletionOwnerTestAccess.Property(attempt, "Result"));
        Assert.Equal(CompletionOwnerTestAccess.Errno("EAgain"),
            CompletionOwnerTestAccess.Property(attempt, "NativeErrno"));
        Assert.Null(CompletionOwnerTestAccess.Property(attempt, "ManagedFailure"));
    }

    [Fact]
    public void backpressured_send_captures_errno_from_submitting_pinvoke()
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        Type nativeMethods = CompletionOwnerTestAccess.RuntimeType(
            "Systems.Zlink.Runtime.Native.NativeMethods");
        MethodInfo send = nativeMethods.GetMethod("zlink_send",
            BindingFlags.Static | BindingFlags.NonPublic)!;
        Assert.True(send.GetCustomAttribute<LibraryImportAttribute>()!
            .SetLastError);
        using var context = Zlink.CreateContext();
        using var dealer = context.CreateDealerSocket();
        dealer.Connect(CoreTestSupport.NewEndpoint(
            "inproc", "send-errno-capture"));
        object owner = CompletionOwnerTestAccess.Owner(dealer);

        using Message part = Message.From("waiting-for-route");
        object attempt = CompletionOwnerTestAccess.Invoke(owner,
            "SubmitSend", null, new[] { part }, 1, new IntPtr(73))!;

        Assert.NotEqual(0UL, CompletionOwnerTestAccess.Property(
            attempt, "CompletionId"));
        Assert.Equal(SubmitResult.Backpressured,
            CompletionOwnerTestAccess.Property(attempt, "Result"));
        Assert.Equal(CompletionOwnerTestAccess.Errno("EAgain"),
            CompletionOwnerTestAccess.Property(attempt, "NativeErrno"));
        Assert.Null(CompletionOwnerTestAccess.Property(attempt, "ManagedFailure"));
    }

    [Theory]
    [InlineData(false, false, 801, "ENoent", (int)ZlinkSubmitException.ErrorCode.NotFound, "ENoent")]
    [InlineData(false, true, 801, "ENoent", (int)ZlinkSubmitException.ErrorCode.NotFound, "ENoent")]
    [InlineData(true, false, 801, "ENoent", (int)ZlinkSubmitException.ErrorCode.NotFound, "ENoent")]
    [InlineData(true, true, 801, "ENoent", (int)ZlinkSubmitException.ErrorCode.NotFound, "ENoent")]
    [InlineData(false, false, 802, "ENotConn", (int)ZlinkSubmitException.ErrorCode.NotConnected, "ENotConn")]
    [InlineData(true, false, 802, "ENotConn", (int)ZlinkSubmitException.ErrorCode.NotConnected, "ENotConn")]
    [InlineData(false, false, 803, "EAgain", (int)ZlinkSubmitException.ErrorCode.InternalError, "EProto")]
    [InlineData(true, false, 803, "EAgain", (int)ZlinkSubmitException.ErrorCode.InternalError, "EProto")]
    [InlineData(false, false, 999, "ENoent", (int)ZlinkSubmitException.ErrorCode.InternalError, "EProto")]
    [InlineData(true, false, 999, "ENoent", (int)ZlinkSubmitException.ErrorCode.InternalError, "EProto")]
    public async Task writable_delivers_core_result_to_matching_token(
        bool request, bool differentEcho, int sendResult, string nativeErrno,
        int expectedResult, string expectedErrno)
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        Type ownerType = CompletionOwnerTestAccess.RuntimeType(
            "Systems.Zlink.CompletionOwner");
        using var context = Zlink.CreateContext();
        using var socket = context.CreateRouterSocket();
        object owner = CompletionOwnerTestAccess.Owner(socket);
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
            IntPtr.Zero);
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
        CompletionOwnerTestAccess.SetField(completion, "SendResult", sendResult);
        CompletionOwnerTestAccess.SetField(completion, "SendTerminalErrno",
            CompletionOwnerTestAccess.Errno(nativeErrno));

        // A nonconforming RID is deliberately injected to prove the binding
        // does not replace the Core terminal result after token/context lookup.
        CompletionOwnerTestAccess.Invoke(entry, "Capture", completion);

        ZlinkSubmitException error = await Assert.ThrowsAsync<ZlinkSubmitException>(
            () => pending);
        Assert.Equal((ZlinkSubmitException.ErrorCode)expectedResult, error.Result);
        Assert.Equal(CompletionOwnerTestAccess.Errno(expectedErrno),
            error.NativeErrno);
        Assert.Empty(CompletionOwnerTestAccess.Entries(owner));
    }
}
