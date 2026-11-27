/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/host/app_internal.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/locations/provider_relocation_repository.hpp"
#include "runtime/stateful/public_store_adapters.hpp"

#include <typeindex>

namespace zlink::framework::detail
{

void configure_app_store_repositories (app_state_t *_state)
{
    if (_state->services.contains (std::type_index (typeid (relocation_store_t)))
        && !_state->services.contains (std::type_index (typeid (relocation_repository_t)))) {
        _state->services.add_factory<relocation_repository_t> (
          [monitoring = _state->monitoring] (service_provider_t &provider) {
              return std::static_pointer_cast<relocation_repository_t> (
                std::make_shared<runtime::provider_relocation_repository_t> (
                  provider.get_required<relocation_store_t> (), monitoring));
          },
          service_lifetime_t::singleton);
    }
    if (_state->services.contains (std::type_index (typeid (location_store_t)))
        && !_state->services.contains (std::type_index (typeid (location_repository_t)))) {
        _state->services.add_factory<location_repository_t> (
          [monitoring = _state->monitoring] (service_provider_t &provider) {
              return std::static_pointer_cast<location_repository_t> (
                std::make_shared<runtime::provider_location_repository_t> (
                  provider.get_required<location_store_t> (), monitoring));
          },
          service_lifetime_t::singleton);
    }
    if (_state->services.contains (std::type_index (typeid (relocation_repository_t)))
        && !_state->services.contains (
          std::type_index (typeid (runtime::stateful::relocation_store_port_t)))) {
        _state->services.add_factory<runtime::stateful::relocation_store_port_t> (
          [] (service_provider_t &provider) {
              return std::unique_ptr<runtime::stateful::relocation_store_port_t> (
                std::make_unique<runtime::stateful::public_relocation_store_adapter_t> (
                  provider.get_required<relocation_repository_t> ()));
          },
          service_lifetime_t::singleton);
    }
}

} // namespace zlink::framework::detail
