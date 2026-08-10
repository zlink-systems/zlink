// SPDX-License-Identifier: MPL-2.0

using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

// Continuation of Message; the type summary lives on the primary partial in
// Message.cs.
public sealed partial class Message : IDisposable, IAsyncDisposable
{
    internal static unsafe int CopySinglePartPayload(IntPtr parts, nuint count,
        Span<byte> destination)
    {
        if (parts == IntPtr.Zero || count == 0)
            return 0;
        if (count != 1)
            throw new InvalidOperationException(
                "Expected a single-part message.");

        var msgv = (ZlinkMsg*)parts;
        var payloadSize = GetNativeSize(ref msgv[0]);
        if (payloadSize <= 0)
            return 0;

        var payloadPtr = NativeMethods.zlink_msg_data(ref msgv[0]);
        if (payloadPtr == IntPtr.Zero)
            return 0;

        var toCopy = payloadSize;
        if (toCopy > destination.Length)
            toCopy = destination.Length;
        new ReadOnlySpan<byte>((void*)payloadPtr, toCopy).CopyTo(destination);
        return payloadSize;
    }

    internal static unsafe Message[] CopyFromNativeReadOnlyVector(IntPtr parts,
        nuint count)
    {
        if (parts == IntPtr.Zero || count == 0)
            return Array.Empty<Message>();

        var length = checked((int)count);
        var result = new Message[length];
        var built = 0;
        try
        {
            var msgv = (ZlinkMsg*)parts;
            for (var i = 0; i < length; i++)
            {
                var size = GetNativeSize(ref msgv[i]);
                if (size <= 0)
                {
                    result[i] = new Message(0);
                }
                else
                {
                    var payloadPtr = NativeMethods.zlink_msg_data(ref msgv[i]);
                    if (payloadPtr == IntPtr.Zero)
                    {
                        result[i] = new Message(0);
                    }
                    else
                    {
                        var payload = new ReadOnlySpan<byte>(
                            (void*)payloadPtr, size);
                        result[i] = new Message(payload);
                    }
                }

                built++;
            }

            return result;
        }
        catch
        {
            for (var i = 0; i < built; i++)
                result[i]?.Dispose();
            throw;
        }
    }

    internal static Message[] FromNativeVector(IntPtr parts, nuint count)
    {
        if (parts == IntPtr.Zero || count == 0)
            return Array.Empty<Message>();
        var length = checked((int)count);
        var result = new Message[length];
        var built = 0;
        try
        {
            unsafe
            {
                var src = (ZlinkMsg*)parts;
                for (var i = 0; i < length; i++)
                {
                    // Hot path: request completion returns native-owned reply
                    // parts. Reuse the managed wrapper and move the native
                    // message into it; do not copy payload bytes here.
                    var msg = RentFromPool();
                    msg.Init();
                    var rc = NativeMethods.zlink_msg_move(ref msg._msg,
                        ref src[i]);
                    if (rc != 0)
                    {
                        msg.Dispose();
                        throw ZlinkException.CreateConfigException(NativeMethods.zlink_errno());
                    }

                    msg._knownSize = -1;
                    result[i] = msg;
                    built++;
                }
            }
        }
        catch
        {
            for (var i = 0; i < built; i++)
                result[i]?.Dispose();
            throw;
        }
        finally
        {
            NativeMethods.zlink_multipart_close(parts, count);
        }

        return result;
    }

    internal static unsafe Message MoveFromNativeSingle(IntPtr message)
    {
        if (message == IntPtr.Zero)
            throw new ArgumentNullException(nameof(message));

        var result = RentFromPool();
        result.Init();
        try
        {
            var src = (ZlinkMsg*)message;
            var rc = NativeMethods.zlink_msg_move(ref result._msg, ref *src);
            if (rc != 0)
                throw ZlinkException.CreateConfigException(NativeMethods.zlink_errno());
            result._knownSize = -1;
            return result;
        }
        catch
        {
            result.Dispose();
            throw;
        }
    }

    internal static Message MoveFromNative(ref ZlinkMsg source)
    {
        var result = new Message(false);
        result.Init();
        try
        {
            var rc = NativeMethods.zlink_msg_move(ref result._msg, ref source);
            if (rc != 0)
                throw ZlinkException.CreateConfigException(NativeMethods.zlink_errno());
            result._knownSize = -1;
            return result;
        }
        catch
        {
            result.Dispose();
            throw;
        }
    }

}
