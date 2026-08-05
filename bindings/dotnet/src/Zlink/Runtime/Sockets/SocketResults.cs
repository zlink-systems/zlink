// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

internal enum SendResult
{
    Sent = 0,
    Backpressured = 1,
    NotReady = 2
}

internal enum RecvResult
{
    Ok = 0,
    NoData = 201,
    Busy = 202,
    Terminated = 203,
    InvalidHandle = 204,
    NotSupported = 205,
    InternalError = 206,
    BufferTooSmall = 207
}

internal enum HandlerResult
{
    Ok = 0,
    InvalidArgument = 301,
    Busy = 302,
    NotSupported = 303,
    Deadlock = 304,
    InvalidHandle = 305,
    InternalError = 306
}
