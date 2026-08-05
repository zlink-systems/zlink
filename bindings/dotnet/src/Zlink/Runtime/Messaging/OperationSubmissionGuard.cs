// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

internal struct OperationSubmissionGuard
{
    private bool _submitted;

    internal void EnsureNotSubmitted()
    {
        if (_submitted)
            throw new ZlinkConfigException(
                ZlinkConfigException.ErrorCode.InvalidState);
    }

    // HOT PATH: operation readiness validation includes the submission-state
    // check before this transition, so the transition does not repeat it.
    internal void MarkSubmittedAfterValidation()
    {
        _submitted = true;
    }
}
