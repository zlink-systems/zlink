// SPDX-License-Identifier: MPL-2.0

using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink.Runtime.Sockets.Internal;

internal sealed partial class SocketKernel : IDisposable
{
    public void Dispose()
    {
        DisposeCore();
    }

    public void Close()
    {
        DisposeCore();
    }

    private void DisposeCore()
    {
        _handle.Dispose();

        // The Core raw API has no STREAM detach entry point; the packet
        // callback lifecycle ends with the successful socket close. Clearing the
        // managed stream callback state keeps the pinned delegates collectable.
        if (_streamAttached)
        {
            _streamAttached = false;
            _callbacks.ClearStream();
        }

        // Core delivers a terminal completion for every operation still
        // pending when the socket closes, so nothing is drained here. Managed
        // completion state changes only after Core accepted the close; EBUSY
        // leaves the native handle and every managed callback intact.
        _sendCompletion?.BeginClose();

        // Core no longer holds the reverse-P/Invoke stub once the socket is
        // closed, so the completion delegate root can finally be released.
        _sendCompletion?.ReleaseAfterNativeClose();
        _callbacks.ClearAllNonStream();
        GC.SuppressFinalize(this);
    }
}
