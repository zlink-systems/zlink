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
        _completion?.PrepareClose();
        try
        {
            _handle.Dispose();
        }
        catch
        {
            _completion?.CancelClose();
            throw;
        }
        _completion?.CompleteClose();
        GC.SuppressFinalize(this);
    }
}
