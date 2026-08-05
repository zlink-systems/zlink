// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

internal enum CloseResult
{
    Ok = 0,
    Busy = 401,
    Shutdown = 402,
    InvalidHandle = 403,
    InternalError = 404
}

internal enum BindResult
{
    Ok = 0,
    InvalidArgument = 501,
    AddrInUse = 502,
    NotSupported = 503,
    InvalidHandle = 504,
    InternalError = 505
}

internal enum ConnectResult
{
    Ok = 0,
    InvalidArgument = 601,
    NotSupported = 602,
    InvalidHandle = 603,
    InternalError = 604,
    NotFound = 605,
    Conflict = 606,
    Busy = 607
}

internal enum ConfigResult
{
    Ok = 0,
    InvalidHandle = 701,
    InvalidArgument = 702,
    NotSupported = 703,
    InternalError = 704,
    InvalidState = 705,
    NotFound = 706,
    Conflict = 707,
    BufferTooSmall = 708,
    Busy = 709
}

internal enum ErrorCode
{
    None = 0,
    Unknown = -1,

    EBusy = 16,
    EIntr = 4,
    EAgain = 11,
    EBadf = 9,
    ENomem = 12,
    EAccess = 13,
    EFault = 14,
    EInval = 22,
    ENotSock = 88,
    EMsgSize = 90,
    EProtoNoSupport = 93,
    ENotSup = 95,
    EAfNoSupport = 97,
    EAddrInUse = 98,
    EAddrNotAvail = 99,
    ENetDown = 100,
    ENetUnreach = 101,
    ENetReset = 102,
    EConnAborted = 103,
    EConnReset = 104,
    ENoBufs = 105,
    ENotConn = 107,
    ETimedOut = 110,
    EConnRefused = 111,
    EHostUnreach = 113,
    EInProgress = 115,
    EShutdown = 108,

    Efsm = 156384763,
    EnoCompatProto = 156384764,
    Eterm = 156384765,
    EmThread = 156384766
}
