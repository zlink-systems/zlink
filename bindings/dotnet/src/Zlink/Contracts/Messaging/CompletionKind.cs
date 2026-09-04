// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>Identifies records stored in a socket completion queue.</summary>
public enum CompletionKind
{
    /// <summary>
    ///     ABI-reserved SEND completion value. Successful SEND admission uses
    ///     completion ID 0 and does not enqueue a completion record.
    /// </summary>
    Send = 1,

    /// <summary>A request completed with a reply or terminal result.</summary>
    Request = 2,

    /// <summary>
    ///     A DONTWAIT SEND wait token became writable. This grants permission to
    ///     retry the same packet; it does not report SEND success.
    /// </summary>
    Writable = 3
}
