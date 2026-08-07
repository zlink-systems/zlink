/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Infrastructure/evidence_store.hpp"
#include "../Infrastructure/fault_state.hpp"

#include <zlink/framework.hpp>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace zlink::framework::e2e::resilience_lifecycle::provider
{

inline void configure_evidence_dispatch_error_observer (
  zlink::framework::logging_builder_t &logging,
  std::shared_ptr<evidence_store_t> evidence,
  std::shared_ptr<fault_state_t> fault_state)
{
    logging.use_provider (
      "resilience-lifecycle-evidence",
      [evidence = std::move (evidence),
       fault_state = std::move (fault_state)] (const zlink::framework::log_record_t &record) {
          if (record.message != "dispatch error")
              return;
          const auto field = [&record] (std::string_view name) {
              const auto found = std::find_if (
                record.fields.begin (), record.fields.end (),
                [name] (const auto &value) { return value.key == name; });
              return found == record.fields.end () ? std::string{} : found->value;
          };
          evidence->record (
            "DispatchError", field ("reason") + ":" + field ("action"));
          if (fault_state->mode () == "observer-throws") {
              throw std::runtime_error ("logging provider failure");
          }
      });
}

} // namespace zlink::framework::e2e::resilience_lifecycle::provider
