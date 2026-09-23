using System;
using System.Runtime.InteropServices;

namespace Systems.Zlink.Stream.Connector.Runtime
{
    /// <summary>
    ///     Declarations for Plugins/WebGL/ZlinkStreamConnector.jslib.
    /// </summary>
    /// <remarks>
    ///     <para>
    ///         Buffer ownership at this boundary, restated from the jslib header so the two
    ///         sides cannot drift:
    ///     </para>
    ///     <list type="bullet">
    ///         <item>
    ///             <description>
    ///                 <see cref="Send" /> and <see cref="Request" />: the payload pointer is
    ///                 owned by this side. JavaScript copies the bytes before the call
    ///                 returns and never keeps the pointer, so the caller frees the buffer
    ///                 immediately afterwards.
    ///             </description>
    ///         </item>
    ///         <item>
    ///             <description>
    ///                 <see cref="EventCallback" />: the text and payload pointers are owned
    ///                 by JavaScript and are freed as soon as the callback returns. The
    ///                 callback must copy what it needs; it must never store a pointer or
    ///                 free one.
    ///             </description>
    ///         </item>
    ///         <item>
    ///             <description>
    ///                 <see cref="TakeLastError" />: ownership of the returned string passes
    ///                 to this side, which frees it with <see cref="FreeBuffer" />.
    ///             </description>
    ///         </item>
    ///     </list>
    /// </remarks>
    internal static class ZlinkStreamInterop
    {
        /// <summary>
        ///     <c>ZlinkStreamPump</c> refused the call because a pump is already running
        ///     on this stack. Nothing was dispatched; this is not a failure.
        /// </summary>
        public const int PumpRefused = -1;

        /// <summary>
        ///     <c>ZlinkStreamPump</c> failed. The reason is waiting in
        ///     <see cref="TakeLastError" />. Every other result is the number of events
        ///     delivered.
        /// </summary>
        public const int PumpFailed = -2;

        public const int EventCallCompleted = 1;
        public const int EventMessage = 2;
        public const int EventErrorReceived = 3;
        public const int EventDisconnected = 4;
        public const int EventStateChanged = 5;
        public const int EventActorBound = 6;
        public const int EventActorUnbound = 7;

        /// <summary>
        ///     Signature of the JavaScript to C# event sink. Marshalled as a function
        ///     pointer, so the target must be a static method.
        /// </summary>
        public delegate void EventCallback(
            int handle,
            int eventType,
            int id,
            int value,
            IntPtr text,
            IntPtr bytes,
            int bytesLength
        );

#if UNITY_WEBGL && !UNITY_EDITOR
        [DllImport("__Internal", EntryPoint = "ZlinkStreamCreate")]
        public static extern int Create(string optionsJson);

        [DllImport("__Internal", EntryPoint = "ZlinkStreamDestroy")]
        public static extern void Destroy(int handle);

        [DllImport("__Internal", EntryPoint = "ZlinkStreamTakeLastError")]
        public static extern IntPtr TakeLastError();

        [DllImport("__Internal", EntryPoint = "ZlinkStreamFreeBuffer")]
        public static extern void FreeBuffer(IntPtr buffer);

        [DllImport("__Internal", EntryPoint = "ZlinkStreamSetEventSink")]
        public static extern int SetEventSink(int handle, IntPtr callback);

        /// <summary>
        ///     Returns the number of events delivered, <see cref="PumpRefused" />, or
        ///     <see cref="PumpFailed" />.
        /// </summary>
        [DllImport("__Internal", EntryPoint = "ZlinkStreamPump")]
        public static extern int Pump(int handle, int maxEvents);

        [DllImport("__Internal", EntryPoint = "ZlinkStreamConnect")]
        public static extern void Connect(int handle, int callId);

        [DllImport("__Internal", EntryPoint = "ZlinkStreamClose")]
        public static extern void Close(int handle, int callId);

        [DllImport("__Internal", EntryPoint = "ZlinkStreamDispatch")]
        public static extern void Dispatch(int handle, int callId);

        [DllImport("__Internal", EntryPoint = "ZlinkStreamCancel")]
        public static extern void Cancel(int handle, int callId);

        [DllImport("__Internal", EntryPoint = "ZlinkStreamSend")]
        public static extern void Send(
            int handle,
            int callId,
            string callJson,
            IntPtr payload,
            int payloadLength
        );

        [DllImport("__Internal", EntryPoint = "ZlinkStreamRequest")]
        public static extern void Request(
            int handle,
            int callId,
            string callJson,
            IntPtr payload,
            int payloadLength
        );

        [DllImport("__Internal", EntryPoint = "ZlinkStreamObserve")]
        public static extern int Observe(int handle, string name);

        [DllImport("__Internal", EntryPoint = "ZlinkStreamUnobserve")]
        public static extern void Unobserve(int handle, string name);

        [DllImport("__Internal", EntryPoint = "ZlinkStreamIsConnected")]
        public static extern int IsConnected(int handle);

        [DllImport("__Internal", EntryPoint = "ZlinkStreamGetState")]
        public static extern int GetState(int handle);

        [DllImport("__Internal", EntryPoint = "ZlinkStreamGetCloseReason")]
        public static extern int GetCloseReason(int handle);

        [DllImport("__Internal", EntryPoint = "ZlinkStreamGetPendingDispatchCount")]
        public static extern int GetPendingDispatchCount(int handle);

        [DllImport("__Internal", EntryPoint = "ZlinkStreamGetDiagnosticsLevel")]
        public static extern int GetDiagnosticsLevel(int handle);

        [DllImport("__Internal", EntryPoint = "ZlinkStreamSetDiagnosticsLevel")]
        public static extern int SetDiagnosticsLevel(int handle, int level);
#else
        // The assembly definition limits this package to the WebGL player, so these
        // bodies exist only to keep the file readable in an IDE that ignores the
        // platform filter. A native or Editor build uses the Zlink.Stream.Connector
        // NuGet package instead; the two assemblies are never compiled together.
        private const string NotWebGl =
            "com.zlink.stream-connector.webgl runs in WebGL player builds only. "
            + "Use the Zlink.Stream.Connector NuGet package for the Editor and native builds.";

        public static int Create(string optionsJson)
        {
            throw new PlatformNotSupportedException(NotWebGl);
        }

        public static void Destroy(int handle)
        {
            throw new PlatformNotSupportedException(NotWebGl);
        }

        public static IntPtr TakeLastError()
        {
            throw new PlatformNotSupportedException(NotWebGl);
        }

        public static void FreeBuffer(IntPtr buffer)
        {
            throw new PlatformNotSupportedException(NotWebGl);
        }

        public static int SetEventSink(int handle, IntPtr callback)
        {
            throw new PlatformNotSupportedException(NotWebGl);
        }

        public static int Pump(int handle, int maxEvents)
        {
            throw new PlatformNotSupportedException(NotWebGl);
        }

        public static void Connect(int handle, int callId)
        {
            throw new PlatformNotSupportedException(NotWebGl);
        }

        public static void Close(int handle, int callId)
        {
            throw new PlatformNotSupportedException(NotWebGl);
        }

        public static void Dispatch(int handle, int callId)
        {
            throw new PlatformNotSupportedException(NotWebGl);
        }

        public static void Cancel(int handle, int callId)
        {
            throw new PlatformNotSupportedException(NotWebGl);
        }

        public static void Send(
            int handle,
            int callId,
            string callJson,
            IntPtr payload,
            int payloadLength
        )
        {
            throw new PlatformNotSupportedException(NotWebGl);
        }

        public static void Request(
            int handle,
            int callId,
            string callJson,
            IntPtr payload,
            int payloadLength
        )
        {
            throw new PlatformNotSupportedException(NotWebGl);
        }

        public static int Observe(int handle, string name)
        {
            throw new PlatformNotSupportedException(NotWebGl);
        }

        public static void Unobserve(int handle, string name)
        {
            throw new PlatformNotSupportedException(NotWebGl);
        }

        public static int IsConnected(int handle)
        {
            throw new PlatformNotSupportedException(NotWebGl);
        }

        public static int GetState(int handle)
        {
            throw new PlatformNotSupportedException(NotWebGl);
        }

        public static int GetCloseReason(int handle)
        {
            throw new PlatformNotSupportedException(NotWebGl);
        }

        public static int GetPendingDispatchCount(int handle)
        {
            throw new PlatformNotSupportedException(NotWebGl);
        }

        public static int GetDiagnosticsLevel(int handle)
        {
            throw new PlatformNotSupportedException(NotWebGl);
        }

        public static int SetDiagnosticsLevel(int handle, int level)
        {
            throw new PlatformNotSupportedException(NotWebGl);
        }
#endif

        public static string TakeLastErrorText()
        {
            var pointer = TakeLastError();
            if (pointer == IntPtr.Zero)
                return null;
            try
            {
                return Marshal.PtrToStringUTF8(pointer);
            }
            finally
            {
                FreeBuffer(pointer);
            }
        }
    }
}
