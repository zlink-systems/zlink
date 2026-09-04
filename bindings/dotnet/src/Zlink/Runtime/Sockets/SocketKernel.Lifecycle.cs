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

    ~SocketKernel()
    {
        try
        {
            DisposeCore(finalizing: true);
        }
        catch
        {
        }
    }

    private void DisposeCore(bool finalizing = false)
    {
        _completion?.PrepareClose();
        try
        {
            _handle.Dispose();
        }
        catch
        {
            if (finalizing)
                _completion?.CompleteClose();
            else
                _completion?.CancelClose();
            throw;
        }
        _completion?.CompleteClose();
        if (!finalizing)
            GC.SuppressFinalize(this);
    }
}
