namespace Zlink.Framework.Runtime.Messaging;

//  A remote request reply carried an application terminal (a non-OK
//  RequestResult) together with a Framework fine failure code. The fine code
//  refines the coarse terminal during ownership-aware classification
//  (spec 32-framework-error-model:81-118): e.g. a remote workerQueueFull(18)
//  stays Unavailable while an actorSessionNotBound(8) is InvalidOperation.
//
//  This is a Framework-side carrier, thrown ONLY at the three
//  application-terminal sites (RequestToActorAsync, the local spot completion,
//  and DecodeDirectApplicationReply). It deliberately does NOT derive from the
//  Core `ZlinkRequestException`: that type's `NativeErrno` is an OS-level errno
//  populated by the Core binding on transport failures, it lives behind a
//  package boundary with no InternalsVisibleTo to Framework, and the same catch
//  blocks also receive genuine Core-thrown `ZlinkRequestException`s. Carrying
//  the fine code in a distinct type keeps the two ownership domains apart: the
//  carrier is the source-visible application terminal, a bare
//  `ZlinkRequestException` is an opaque Core-transport failure classified
//  coarsely.
internal sealed class ZLinkRequestTerminalException : Exception
{
    internal ZLinkRequestTerminalException(RequestResult result, int failureErrno)
        : base($"Request completed with terminal result '{result}'.")
    {
        Result = result;
        FailureErrno = failureErrno;
    }

    //  The coarse wire terminal (RequestResult), always populated.
    internal RequestResult Result { get; }

    //  The Framework fine failure code (ServiceWireConstants.FrameworkErrorCode),
    //  or 0 (None) when the reply carried no fine code.
    internal int FailureErrno { get; }
}
