using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_writable_token_delivery
{
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
