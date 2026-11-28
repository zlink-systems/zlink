/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/spot_close_scenario_fixture.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

TEST (ZLinkFrameworkSpotContextCloseJoin, AfterAuthorityRelease)
{
    zlink::framework::test::spot_close::run_and_check (
      nlohmann::json{{"name", "context-close-join-after-authority-release"},
                     {"given", {{"authority", "Missing"}}},
                     {"act", "contextCloseJoinsAfterAuthorityRelease"},
                     {"expect",
                      {{"authorityBeforeJoin", "Missing"},
                       {"activationDetachedBeforeJoin", true},
                       {"joinedBeforeAcknowledgment", 1},
                       {"firstCompletedBeforeAcknowledgment", false},
                       {"joinedCompletedBeforeAcknowledgment", false},
                       {"firstResult", true},
                       {"joinedResult", true},
                       {"closeTransactionsAfterResults", 0},
                       {"remainingJoiners", 0},
                       {"nextResult", false},
                       {"onClosingCalls", 1}}}});
}
