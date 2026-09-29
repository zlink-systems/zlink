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
        // Core close is fail-fast: EBUSY reaches the caller as-is and nothing
        // in the binding waits for or retries in-flight calls.
        try
        {
            _handle.Dispose();
        }
        catch
        {
            if (finalizing)
                _completion?.CompleteClose();
            throw;
        }
        _completion?.CompleteClose();
        if (!finalizing)
            GC.SuppressFinalize(this);
    }
}
