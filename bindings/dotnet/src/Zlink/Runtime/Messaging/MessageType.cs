// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

internal enum MessageType : byte
{
    Data = 0,
    Request = 1,
    Reply = 2
}