using System.Runtime.CompilerServices;
using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_socket_handle_reservation
{
    [Fact]
    public void reservation_roots_the_finalizing_kernel()
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        using var context = Zlink.CreateContext();
        (object handle, IDisposable call, WeakReference<object> owner) =
            CreateReservationWithoutSocketRoot(context);
        try
        {
            GC.Collect();
            GC.WaitForPendingFinalizers();
            GC.Collect();
            Assert.True(owner.TryGetTarget(out _));
        }
        finally
        {
            call.Dispose();
            CompletionOwnerTestAccess.Invoke(handle, "Dispose");
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static (object, IDisposable, WeakReference<object>)
        CreateReservationWithoutSocketRoot(IContext context)
    {
        IPairSocket socket = context.CreatePairSocket();
        object kernel = CompletionOwnerTestAccess.Property(socket, "Kernel");
        object handle = Handle(socket);
        return (handle, Reserve(handle), new WeakReference<object>(kernel));
    }

    [Fact]
    public async Task reservations_overlap_and_close_is_busy_until_the_last_release()
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        using var context = Zlink.CreateContext();
        using var socket = context.CreatePairSocket();
        object handle = Handle(socket);
        IDisposable first = Reserve(handle);
        IDisposable? second = null;
        try
        {
            second = await Task.Run(() => Reserve(handle))
                .WaitAsync(TimeSpan.FromSeconds(3));
            AssertBusy(socket);
            first.Dispose();
            first = null!;
            AssertBusy(socket);
        }
        finally
        {
            first?.Dispose();
            second?.Dispose();
        }
        // A rejected close cannot disable reservations.
        Reserve(handle).Dispose();
        socket.Close();
        ZlinkSubmitException error = Assert.Throws<ZlinkSubmitException>(
            () => Reserve(handle));
        Assert.Equal(ZlinkSubmitException.ErrorCode.Terminated, error.Result);
    }

    private static object Handle(ISocket socket)
    {
        object kernel = CompletionOwnerTestAccess.Property(socket, "Kernel");
        return CompletionOwnerTestAccess.Field(kernel, "_handle");
    }

    private static IDisposable Reserve(object handle) =>
        (IDisposable)CompletionOwnerTestAccess.Invoke(handle, "Reserve", true)!;

    private static void AssertBusy(ISocket socket)
    {
        ZlinkCloseException error = Assert.Throws<ZlinkCloseException>(socket.Close);
        Assert.Equal(ZlinkCloseException.ErrorCode.Busy, error.Result);
        Assert.Equal(CoreTestSupport.Errno("EBUSY"), error.NativeErrno);
    }
}
