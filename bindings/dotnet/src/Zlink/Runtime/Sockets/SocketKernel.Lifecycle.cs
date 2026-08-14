// SPDX-License-Identifier: MPL-2.0

using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink.Runtime.Sockets.Internal;

internal sealed partial class SocketKernel : IDisposable
{
    public void Dispose()
    {
        Dispose(true);
    }

    public void Close()
    {
        Dispose(true);
    }

    private void Dispose(bool closeNativeSocket)
    {
        PublisherAdmissionScheduler? publisherAdmission;
        lock (_publisherAdmissionSync)
        {
            _publisherAdmissionClosing = true;
            publisherAdmission = _publisherAdmission;
        }
        publisherAdmission?.BeginClose();
        publisherAdmission?.WaitForSubmitQuiescence();

        RoutedAdmissionScheduler? routedAdmission;
        lock (_routedAdmissionSync)
        {
            _routedAdmissionClosing = true;
            routedAdmission = _routedAdmission;
        }
        routedAdmission?.BeginClose();
        routedAdmission?.WaitForSubmitQuiescence();

        // The Core raw API has no STREAM detach entry point; the packet
        // callback lifecycle ends with the socket close below. Clearing the
        // managed stream callback state keeps the pinned delegates collectable.
        if (_streamAttached)
        {
            _streamAttached = false;
            _callbacks.ClearStream();
        }

        if (closeNativeSocket)
            _handle.Dispose();
        else
            _handle.ReleaseWithoutClose();
        _callbacks.ClearAllNonStream();
        GC.SuppressFinalize(this);
    }
}
