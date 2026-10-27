#pragma once
#include "server/region/domain/positions.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace MyGame::E2E
{
inline void check_domain ()
{
    int checked = 0;
    const auto check = [&] (bool condition, const char *name) {
        if (!condition)
            throw std::runtime_error (std::string ("Domain boundary failed: ") + name);
        ++checked;
    };

    check (Names::max_speed == Names::max_step * 5.0, "movement speed contract");

    auto now = std::chrono::steady_clock::time_point{};
    Positions positions ([&] { return now; });
    positions.add ("p1");
    check (positions.move ("p1", {static_cast<double> (Names::max_step), 1}) == std::nullopt
             && positions.view ().front ()
                  == PlayerPosition{"p1", static_cast<double> (Names::max_step), 1},
           "first movement accepts MaxStep");

    auto before = positions.capture ();
    const auto budget = positions.move ("p1", {static_cast<double> (Names::max_step), 2});
    check (budget && budget->Reason == Rejection::move_budget && budget->RetryAfterMs == 200
             && before == positions.capture (),
           "move budget rejection preserves position, sequence and remaining time");
    const auto fractional_budget = positions.move ("p1", {0.01, 2});
    check (fractional_budget && fractional_budget->Reason == Rejection::move_budget
             && fractional_budget->RetryAfterMs > 0 && before == positions.capture (),
           "positive movement deficit returns positive retry time");

    const auto too_large = positions.move ("p1", {Names::max_step + 1.0, 2});
    check (too_large && too_large->Reason == Rejection::step_too_large
             && too_large->RetryAfterMs == 0 && before == positions.capture (),
           "MaxStep plus one is rejected without state change");
    const auto stale = positions.move ("p1", {1, 1});
    check (stale && stale->Reason == Rejection::stale_sequence && stale->RetryAfterMs == 0
             && before == positions.capture (),
           "current sequence is stale without state change");

    for (const auto value : {std::numeric_limits<double>::quiet_NaN (),
                             std::numeric_limits<double>::infinity (),
                             -std::numeric_limits<double>::infinity ()}) {
        const auto non_finite = positions.move ("p1", {value, 2});
        check (non_finite && non_finite->Reason == Rejection::non_finite
                 && non_finite->RetryAfterMs == 0 && before == positions.capture (),
               "non-finite movement is rejected without state change");
    }

    Positions edge ([&] { return now; });
    edge.restore (PositionsSnapshot{{PositionSnapshot{"edge", Names::region_max_x, 0, 0}}});
    before = edge.capture ();
    const auto outside_max = edge.move ("edge", {1, 1});
    check (outside_max && outside_max->Reason == Rejection::outside_region
             && outside_max->RetryAfterMs == 0 && before == edge.capture (),
           "region maximum plus one is rejected without state change");
    edge.restore (PositionsSnapshot{{PositionSnapshot{"edge", Names::region_min_x, 0, 0}}});
    before = edge.capture ();
    const auto outside_min = edge.move ("edge", {-1, 1});
    check (outside_min && outside_min->Reason == Rejection::outside_region
             && outside_min->RetryAfterMs == 0 && before == edge.capture (),
           "region minimum minus one is rejected without state change");

    Positions safe_sequence ([&] { return now; });
    safe_sequence.add ("safe");
    check (safe_sequence.move ("safe", {0, Names::max_safe_integer}) == std::nullopt,
           "maximum safe integer sequence is accepted");
    before = safe_sequence.capture ();
    const auto unsafe_sequence = safe_sequence.move (
      "safe", {0, Names::max_safe_integer + 1});
    check (unsafe_sequence && unsafe_sequence->Reason == Rejection::integer_out_of_range
             && unsafe_sequence->RetryAfterMs == 0 && before == safe_sequence.capture (),
           "sequence above maximum safe integer is rejected without overflow");
    Positions oversized_domain ([&] { return now; });
    oversized_domain.add ("oversized");
    before = oversized_domain.capture ();
    const auto oversized_number =
      oversized_domain.move ("oversized", {std::numeric_limits<double>::max (), 1});
    check (oversized_number && oversized_number->Reason == Rejection::step_too_large
             && oversized_number->RetryAfterMs == 0 && before == oversized_domain.capture (),
           "finite step above MaxStep is rejected without overflow");

    auto source_now = std::chrono::steady_clock::time_point{};
    Positions source ([&] { return source_now; });
    source.add ("relocated");
    check (source.move ("relocated", {static_cast<double> (Names::max_step), 1}) == std::nullopt,
           "snapshot source move");
    const auto snapshot = source.capture ();
    check (snapshot.Players.front ().RemainingMs == 200,
           "snapshot stores remaining milliseconds");
    check (snapshot == source.capture (), "capture does not change the source domain");

    auto destination_now = std::chrono::steady_clock::time_point{} + std::chrono::hours (8);
    Positions destination ([&] { return destination_now; });
    destination.restore (snapshot);
    check (source.view () == destination.view ()
             && destination.capture ().Players.front ().RemainingMs == 200,
           "relocation restores state under a different clock origin");
    before = destination.capture ();
    const auto relocated_budget =
      destination.move ("relocated", {static_cast<double> (Names::max_step), 2});
    check (relocated_budget && relocated_budget->Reason == Rejection::move_budget
             && relocated_budget->RetryAfterMs == 200 && before == destination.capture (),
           "relocated movement retains its remaining budget");
    destination_now += std::chrono::milliseconds (relocated_budget->RetryAfterMs);
    check (destination.move ("relocated", {static_cast<double> (Names::max_step), 2})
             == std::nullopt,
           "relocated movement accepts at the server-provided boundary");

    std::cout << "Domain boundaries: " << checked << "/" << checked << " passed" << std::endl;
}
}
