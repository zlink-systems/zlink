/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace zlink::framework::runtime::client_server
{

struct weighted_candidate_t
{
    std::string key;
    std::uint32_t weight = 0;
    // Candidate identity used for the contract-defined deterministic tiebreak.
    // key remains the connection lookup key and may include a generation.
    std::string tie_break_key;

    friend bool operator== (const weighted_candidate_t &,
                            const weighted_candidate_t &) = default;
};

class smooth_weighted_selector_t
{
  public:
    void set_candidates (std::span<const weighted_candidate_t> candidates)
    {
        std::map<std::string, weighted_candidate_t> normalized;
        for (const auto &candidate : candidates) {
            if (candidate.weight == 0)
                continue;
            auto value = candidate;
            if (value.tie_break_key.empty ())
                value.tie_break_key = value.key;
            normalized.insert_or_assign (value.key, std::move (value));
        }
        std::vector<weighted_candidate_t> next;
        next.reserve (normalized.size ());
        for (auto &[_, candidate] : normalized)
            next.push_back (std::move (candidate));
        if (next == _candidates)
            return;

        std::int64_t total = 0;
        for (const auto &candidate : next) {
            if (total
                > std::numeric_limits<std::int64_t>::max ()
                    - candidate.weight) {
                throw std::overflow_error (
                  "ClientServer selection weight total exceeds int64");
            }
            total += candidate.weight;
        }

        materialize_precomputed_state ();
        _candidates = std::move (next);
        _total = total;
        for (auto it = _credits.begin (); it != _credits.end ();) {
            const auto active = std::find_if (
              _candidates.begin (), _candidates.end (),
              [&] (const auto &candidate) { return candidate.key == it->first; });
            if (active == _candidates.end ())
                it = _credits.erase (it);
            else
                ++it;
        }
        for (auto &[_, credit] : _credits)
            credit = std::clamp (credit, -_total, _total);
        rebuild_precomputed_schedule ();
    }

    std::optional<std::string> select ()
    {
        if (_candidates.empty () || _total == 0)
            return std::nullopt;

        if (_precomputed) {
            const auto selected_index =
              _precomputed_schedule[_precomputed_cursor++];
            ++_precomputed_selected_counts[selected_index];
            ++_precomputed_total_selections;
            const auto result = _candidates[selected_index].key;
            if (_precomputed_cursor == _precomputed_schedule_end) {
                // The state at the end of the stored cycle is exactly the
                // state at its beginning. Reset the lazy accounting so the
                // hot path remains a cursor advance plus one counter update.
                _precomputed_initial_credits =
                  _precomputed_cycle_start_credits;
                std::fill (_precomputed_selected_counts.begin (),
                           _precomputed_selected_counts.end (), 0);
                _precomputed_total_selections = 0;
                _precomputed_cursor = _precomputed_cycle_start;
            }
            return result;
        }

        const weighted_candidate_t *selected = nullptr;
        for (const auto &candidate : _candidates) {
            auto &credit = _credits[candidate.key];
            if (credit
                > std::numeric_limits<std::int64_t>::max ()
                    - static_cast<std::int64_t> (candidate.weight)) {
                throw std::overflow_error (
                  "ClientServer selection cumulative value is exhausted");
            }
            credit += static_cast<std::int64_t> (candidate.weight);
            if (selected == nullptr
                || credit > _credits[selected->key]
                || (credit == _credits[selected->key]
                    && candidate.tie_break_key < selected->tie_break_key))
                selected = &candidate;
        }

        _credits[selected->key] -= _total;
        return selected->key;
    }

    std::optional<std::string> select (
      std::span<const weighted_candidate_t> candidates)
    {
        set_candidates (candidates);
        return select ();
    }

    std::size_t state_size () const noexcept { return _credits.size (); }

    std::int64_t maximum_absolute_credit () const noexcept
    {
        std::int64_t maximum = 0;
        if (_precomputed) {
            for (std::size_t index = 0; index < _candidates.size (); ++index) {
                const auto credit = current_precomputed_credit (index);
                maximum = std::max (maximum, credit < 0 ? -credit : credit);
            }
        } else {
            for (const auto &[_, credit] : _credits)
                maximum = std::max (
                  maximum, credit < 0 ? -credit : credit);
        }
        return maximum;
    }

  private:
    using credit_vector_t = std::vector<std::int64_t>;

    static constexpr std::size_t max_precomputed_steps = 4096;
    static constexpr auto max_precompute_time = std::chrono::milliseconds (5);

    std::optional<std::size_t> select_index (
      const credit_vector_t &credits) const noexcept
    {
        if (_candidates.empty () || _total == 0)
            return std::nullopt;
        std::optional<std::size_t> selected;
        for (std::size_t index = 0; index < _candidates.size (); ++index) {
            const auto candidate_credit =
              credits[index] + static_cast<std::int64_t> (
                _candidates[index].weight);
            if (!selected
                || candidate_credit >
                     credits[*selected]
                       + static_cast<std::int64_t> (
                           _candidates[*selected].weight)
                || (candidate_credit ==
                      credits[*selected]
                        + static_cast<std::int64_t> (
                            _candidates[*selected].weight)
                    && _candidates[index].tie_break_key
                         < _candidates[*selected].tie_break_key)) {
                selected = index;
            }
        }
        return selected;
    }

    void apply_selection (credit_vector_t &credits,
                          std::size_t selected) const noexcept
    {
        for (std::size_t index = 0; index < _candidates.size (); ++index)
            credits[index] += static_cast<std::int64_t> (
              _candidates[index].weight);
        credits[selected] -= _total;
    }

    std::int64_t current_precomputed_credit (std::size_t index) const noexcept
    {
        const auto total_selections = static_cast<std::int64_t> (
          _precomputed_total_selections);
        return _precomputed_initial_credits[index]
               + total_selections
                   * static_cast<std::int64_t> (_candidates[index].weight)
               - static_cast<std::int64_t> (
                   _precomputed_selected_counts[index])
                   * _total;
    }

    void materialize_precomputed_state ()
    {
        if (!_precomputed)
            return;
        for (std::size_t index = 0; index < _candidates.size (); ++index)
            _credits[_candidates[index].key] =
              current_precomputed_credit (index);
        _precomputed = false;
        _precomputed_schedule.clear ();
        _precomputed_selected_counts.clear ();
        _precomputed_initial_credits.clear ();
        _precomputed_cycle_start_credits.clear ();
        _precomputed_total_selections = 0;
        _precomputed_cursor = 0;
        _precomputed_cycle_start = 0;
        _precomputed_schedule_end = 0;
    }

    void rebuild_precomputed_schedule ()
    {
        _precomputed = false;
        _precomputed_schedule.clear ();
        _precomputed_selected_counts.clear ();
        _precomputed_initial_credits.clear ();
        _precomputed_cycle_start_credits.clear ();
        _precomputed_total_selections = 0;
        _precomputed_cursor = 0;
        _precomputed_cycle_start = 0;
        _precomputed_schedule_end = 0;
        if (_candidates.empty () || _total == 0)
            return;

        credit_vector_t simulated;
        simulated.reserve (_candidates.size ());
        for (const auto &candidate : _candidates)
            simulated.push_back (_credits[candidate.key]);
        const auto initial = simulated;
        std::map<credit_vector_t, std::size_t> seen;
        std::vector<std::size_t> schedule;
        schedule.reserve (max_precomputed_steps);
        const auto started = std::chrono::steady_clock::now ();
        for (std::size_t step = 0; step < max_precomputed_steps; ++step) {
            if (std::chrono::steady_clock::now () - started
                >= max_precompute_time)
                return;
            const auto [found, inserted] = seen.emplace (simulated, step);
            if (!inserted) {
                const auto cycle_start = found->second;
                credit_vector_t cycle_state = initial;
                for (std::size_t index = 0; index < cycle_start; ++index)
                    apply_selection (cycle_state, schedule[index]);
                _precomputed = true;
                _precomputed_initial_credits = initial;
                _precomputed_cycle_start_credits = std::move (cycle_state);
                _precomputed_selected_counts.assign (_candidates.size (), 0);
                _precomputed_cycle_start = cycle_start;
                _precomputed_schedule_end = schedule.size ();
                _precomputed_schedule = std::move (schedule);
                return;
            }
            const auto selected = select_index (simulated);
            if (!selected)
                return;
            schedule.push_back (*selected);
            apply_selection (simulated, *selected);
        }
    }

    std::vector<weighted_candidate_t> _candidates;
    std::int64_t _total = 0;
    std::map<std::string, std::int64_t> _credits;
    bool _precomputed = false;
    credit_vector_t _precomputed_initial_credits;
    credit_vector_t _precomputed_cycle_start_credits;
    std::vector<std::size_t> _precomputed_schedule;
    std::vector<std::size_t> _precomputed_selected_counts;
    std::size_t _precomputed_total_selections = 0;
    std::size_t _precomputed_cursor = 0;
    std::size_t _precomputed_cycle_start = 0;
    std::size_t _precomputed_schedule_end = 0;
};

} // namespace zlink::framework::runtime::client_server
