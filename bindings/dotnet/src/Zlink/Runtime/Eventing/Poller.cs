// SPDX-License-Identifier: MPL-2.0

using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

internal sealed class Poller : NativeOwner, IPoller
{
    private readonly List<PollItem> _items = new();
    private ZlinkPollerEvent[] _nativeEvents = Array.Empty<ZlinkPollerEvent>();

    public Poller() : base(CreateHandle())
    {
        _items.Clear();
        _nativeEvents = Array.Empty<ZlinkPollerEvent>();
    }

    internal int Count
    {
        get
        {
            EnsureNotDisposed();
            var rc = NativeMethods.zlink_poller_size(_handle, out var errorOut);
            if (rc < 0)
                throw ZlinkException.CreateConfigException((ConfigResult)errorOut);
            return rc;
        }
    }

    public int Size => Count;

    public void Add(IZlinkSocket socket, PollEventFlags events, nuint slot)
    {
        EnsureNotDisposed();
        var concreteSocket = SocketInterop.RequireSocket(socket, nameof(socket));
        var socketHandle = concreteSocket.Handle;
        EnumValidation.EnsurePollEvents(events, nameof(events));

        var ownsCompletion = (events & PollEventFlags.PollCompletion) != 0;
        var completionOwner = concreteSocket.Kernel.Completion;
        if (ownsCompletion)
            completionOwner.TransferToPublic(this);

        var userData = SlotToUserData(slot);
        var rc = NativeMethods.zlink_poller_add(_handle, socketHandle,
            userData, (short)events);
        if (rc != 0)
        {
            if (ownsCompletion)
                completionOwner.TransferToRuntime(this);
            ZlinkException.ThrowConfigIfError(rc);
        }
        RegisterItem(new PollItem(PollItemKind.Socket, socket, socketHandle, 0,
            null, events, slot, completionOwner, ownsCompletion));
    }

    public void AddFd(int fd, PollEventFlags events, nuint slot)
    {
        EnsureNotDisposed();
        EnumValidation.EnsurePollEvents(events, nameof(events));

        var userData = SlotToUserData(slot);
        var rc = NativeMethods.zlink_poller_add_fd(_handle, fd, userData,
            (short)events);
        if (rc != 0)
            ZlinkException.ThrowConfigIfError(rc);
        RegisterItem(new PollItem(PollItemKind.Fd, null, IntPtr.Zero, fd, null,
            events, slot, null, false));
    }

    public void Add(IZlinkTimer timer, nuint slot)
    {
        EnsureNotDisposed();
        var concreteTimer = SocketInterop.RequireTimer(timer, nameof(timer));

        var userData = SlotToUserData(slot);
        var rc = NativeMethods.zlink_poller_add_timer(_handle,
            concreteTimer.Handle, userData);
        if (rc != 0)
            ZlinkException.ThrowConfigIfError(rc);
        RegisterItem(new PollItem(PollItemKind.Timer, null, IntPtr.Zero, 0,
            concreteTimer, PollEventFlags.PollIn, slot, null, false));
    }

    public void Modify(IZlinkSocket socket, PollEventFlags events)
    {
        EnsureNotDisposed();
        var socketHandle = SocketInterop.RequirePollableHandle(socket,
            nameof(socket));
        EnumValidation.EnsurePollEvents(events, nameof(events));

        var index = FindSocket(socketHandle);
        if (index < 0)
            throw new ArgumentException("socket is not registered",
                nameof(socket));

        var item = _items[index];
        var wantsCompletion = (events & PollEventFlags.PollCompletion) != 0;
        if (!item.OwnsCompletion && wantsCompletion)
            item.CompletionOwner!.TransferToPublic(this);

        var rc = NativeMethods.zlink_poller_modify(_handle, socketHandle,
            (short)events);
        if (rc != 0)
        {
            if (!item.OwnsCompletion && wantsCompletion)
                item.CompletionOwner!.TransferToRuntime(this);
            ZlinkException.ThrowConfigIfError(rc);
        }
        var hadCompletion = item.OwnsCompletion;
        item.Events = events;
        item.OwnsCompletion = wantsCompletion;
        if (hadCompletion && !wantsCompletion)
            item.CompletionOwner!.TransferToRuntime(this);
    }

    public void ModifyFd(int fd, PollEventFlags events)
    {
        EnsureNotDisposed();
        EnumValidation.EnsurePollEvents(events, nameof(events));

        var index = FindFd(fd);
        if (index < 0)
            throw new ArgumentException("fd is not registered", nameof(fd));

        var rc = NativeMethods.zlink_poller_modify_fd(_handle, fd,
            (short)events);
        ZlinkException.ThrowConfigIfError(rc);
        _items[index].Events = events;
    }

    public bool Remove(IZlinkSocket socket)
    {
        EnsureNotDisposed();
        var socketHandle = SocketInterop.RequirePollableHandle(socket,
            nameof(socket));

        var index = FindSocket(socketHandle);
        if (index < 0)
            return false;

        var rc = NativeMethods.zlink_poller_remove(_handle, socketHandle);
        ZlinkException.ThrowConfigIfError(rc);
        var item = _items[index];
        UnregisterItem(index);
        if (item.OwnsCompletion)
            item.CompletionOwner!.TransferToRuntime(this);
        return true;
    }

    public bool Remove(IZlinkTimer timer)
    {
        EnsureNotDisposed();
        var concreteTimer = SocketInterop.RequireTimer(timer, nameof(timer));

        var index = FindTimer(concreteTimer.Handle);
        if (index < 0)
            return false;

        var rc = NativeMethods.zlink_poller_remove_timer(_handle,
            concreteTimer.Handle);
        ZlinkException.ThrowConfigIfError(rc);
        UnregisterItem(index);
        return true;
    }

    public bool Remove(int fd)
    {
        EnsureNotDisposed();

        var index = FindFd(fd);
        if (index < 0)
            return false;

        var rc = NativeMethods.zlink_poller_remove_fd(_handle, fd);
        ZlinkException.ThrowConfigIfError(rc);
        UnregisterItem(index);
        return true;
    }

    public void Clear()
    {
        EnsureNotDisposed();

        _ = DestroyHandle(DestroyNative, throwOnError: true);
        ReleaseCompletionOwners();
        _items.Clear();
        _nativeEvents = Array.Empty<ZlinkPollerEvent>();

        _handle = NativeMethods.zlink_poller_new();
        if (_handle == IntPtr.Zero)
        {
            _handle = IntPtr.Zero;
            throw ZlinkException.CreateConfigException(NativeMethods.zlink_errno());
        }
    }

    public void Close()
    {
        Dispose();
    }

    public int Wait(Span<PollEvent> destination, TimeSpan timeout)
    {
        EnsureNotDisposed();
        if (destination.Length == 0)
            throw new ArgumentException("destination must not be empty.",
                nameof(destination));
        if (_items.Count == 0)
            return 0;

        var capacity = Math.Min(destination.Length, _items.Count);
        EnsureEventCapacity(capacity);
        var timeoutMs = ToTimeoutMilliseconds(timeout);
        var deadline = timeoutMs > 0
            ? Environment.TickCount64 + timeoutMs
            : 0;
        while (true)
        {
            var ready = WaitNative(timeoutMs, capacity, out var errorOut);
            if (ready < 0)
                throw ZlinkException.CreateConfigException(
                    (ConfigResult)errorOut);
            if (ready == 0)
                return 0;

            var written = 0;
            for (var i = 0; i < ready; i++)
            {
                var nativeEvent = _nativeEvents[i];
                var completionReady = (nativeEvent.Events
                    & (short)PollEventFlags.PollCompletion) != 0;
                var writableReady = (nativeEvent.Events
                    & (short)PollEventFlags.PollOut) != 0;
                if (completionReady || writableReady)
                {
                    var itemIndex = FindSocket(nativeEvent.Socket);
                    if (itemIndex >= 0 && _items[itemIndex].OwnsCompletion)
                    {
                        var drained = _items[itemIndex].CompletionOwner!.Drain();
                        if (completionReady && drained.RequestCount == 0)
                            nativeEvent.Events &= unchecked(
                                (short)~(short)PollEventFlags.PollCompletion);
                    }
                }
                if (nativeEvent.Events == 0)
                    continue;
                destination[written++] = MapEvent(nativeEvent);
            }
            if (written != 0 || timeoutMs == 0)
                return written;

            // A WRITABLE-only completion can be consumed while the caller only
            // requested POLLCOMPLETION. That internal progress is not a timeout;
            // continue the same wait for its remaining deadline.
            if (timeoutMs > 0)
            {
                var remaining = deadline - Environment.TickCount64;
                if (remaining <= 0)
                    return 0;
                timeoutMs = (int)Math.Min(remaining, int.MaxValue);
            }
        }
    }

    public void Dispose()
    {
        Destroy(true);
        GC.SuppressFinalize(this);
    }

    public ValueTask DisposeAsync()
    {
        Dispose();
        return ValueTask.CompletedTask;
    }

    ~Poller()
    {
        Destroy(false);
    }

    private static int DestroyNative(ref IntPtr handle)
    {
        return NativeMethods.zlink_poller_destroy(ref handle);
    }

    private void Destroy(bool throwOnError)
    {
        _ = DestroyHandle(DestroyNative, throwOnError, _ =>
        {
            ReleaseCompletionOwners();
            _items.Clear();
            _nativeEvents = Array.Empty<ZlinkPollerEvent>();
        });
    }

    private static List<string> GetMissingExports()
    {
        return NativeLibraryLoader.GetMissingExports(
            NativeMethods.RequiredPollerExports);
    }

    private static int ToTimeoutMilliseconds(TimeSpan timeout)
    {
        if (timeout < TimeSpan.Zero)
            return -1;
        var millis = timeout.TotalMilliseconds;
        if (millis > int.MaxValue)
            return int.MaxValue;
        return (int)Math.Ceiling(millis);
    }

    private void EnsureEventCapacity(int count)
    {
        if (_nativeEvents.Length < count)
            _nativeEvents = new ZlinkPollerEvent[count];
    }

    private unsafe int WaitNative(int timeoutMs, int capacity,
        out int errorOut)
    {
        fixed (ZlinkPollerEvent* events = _nativeEvents)
        {
            return NativeMethods.zlink_poller_wait_pinned(_handle, events,
                capacity, timeoutMs, out errorOut);
        }
    }

    private int FindSocket(IntPtr handle)
    {
        for (var i = 0; i < _items.Count; i++)
        {
            var item = _items[i];
            if (item.IsSocket && item.SocketHandle == handle)
                return i;
        }

        return -1;
    }

    private int FindFd(int fd)
    {
        for (var i = 0; i < _items.Count; i++)
        {
            var item = _items[i];
            if (item.Kind == PollItemKind.Fd && item.Fd == fd)
                return i;
        }

        return -1;
    }

    private int FindTimer(IntPtr handle)
    {
        for (var i = 0; i < _items.Count; i++)
        {
            var item = _items[i];
            if (item.Kind == PollItemKind.Timer && item.Timer?.Handle == handle)
                return i;
        }

        return -1;
    }

    private PollEvent MapEvent(ZlinkPollerEvent nativeEvent)
    {
        var sourceKind = (PollSourceKind)nativeEvent.MonitorSourceKind;
        var fd = sourceKind == PollSourceKind.Fd ? nativeEvent.Fd : 0;
        return new PollEvent(sourceKind, UserDataToSlot(nativeEvent.UserData),
            (PollEventFlags)nativeEvent.Events, fd);
    }

    private static IntPtr SlotToUserData(nuint slot)
    {
        return unchecked((IntPtr)slot);
    }

    private static nuint UserDataToSlot(IntPtr userData)
    {
        return unchecked((nuint)userData);
    }

    private void RegisterItem(PollItem item)
    {
        _items.Add(item);
    }

    private void UnregisterItem(int index)
    {
        _items.RemoveAt(index);
    }

    private void ReleaseCompletionOwners()
    {
        foreach (var item in _items)
            if (item.OwnsCompletion)
                item.CompletionOwner!.TransferToRuntime(this);
    }

    private void EnsureNotDisposed()
    {
        EnsureNativeHandle(nameof(Poller));
    }

    private static IntPtr CreateHandle()
    {
        var missing = GetMissingExports();
        if (missing.Count > 0)
            throw new ZlinkConfigException(ConfigResult.NotSupported,
                (int)ErrorCode.ENotSup);

        var handle = NativeMethods.zlink_poller_new();
        if (handle == IntPtr.Zero)
            throw ZlinkException.CreateConfigException(NativeMethods.zlink_errno());
        return handle;
    }

    private enum PollItemKind
    {
        Socket,
        Fd,
        Timer
    }

    private sealed class PollItem
    {
        public PollItem(PollItemKind kind, IZlinkSocket? socket,
            IntPtr socketHandle, int fd, Timer? timer, PollEventFlags events,
            nuint slot, CompletionOwner? completionOwner,
            bool ownsCompletion)
        {
            Kind = kind;
            Socket = socket;
            SocketHandle = socketHandle;
            Fd = fd;
            Timer = timer;
            Events = events;
            Slot = slot;
            CompletionOwner = completionOwner;
            OwnsCompletion = ownsCompletion;
        }

        public PollItemKind Kind { get; }
        public IZlinkSocket? Socket { get; }
        public IntPtr SocketHandle { get; }
        public int Fd { get; }
        public Timer? Timer { get; }
        public PollEventFlags Events { get; set; }
        public nuint Slot { get; }
        public CompletionOwner? CompletionOwner { get; }
        public bool OwnsCompletion { get; set; }
        public bool IsSocket => Kind == PollItemKind.Socket;
    }
}
