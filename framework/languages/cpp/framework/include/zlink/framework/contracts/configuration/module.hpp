/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/configuration/services.hpp>
#include <zlink/framework/contracts/configuration/zlink_builder.hpp>
#include <zlink/framework/contracts/handlers/handler_registry.hpp>

#include <concepts>

namespace zlink::framework
{

class hosted_service_t
{
  public:
    virtual ~hosted_service_t () = default;

    virtual void start (service_provider_t &services) = 0;
    virtual void request_stop () noexcept {}
    virtual void stop () noexcept = 0;
};

class module_t
{
  public:
    virtual ~module_t () = default;

    virtual void configure_services (service_collection_t &services) { (void) services; }

    virtual void configure_zlink (zlink_builder_t &zlink) { (void) zlink; }

    virtual void configure_handlers (handler_registry_t &handlers) { (void) handlers; }

};

template <typename TModule>
concept framework_module_contract_t = requires (TModule & module,
                                                service_collection_t &services,
                                                zlink_builder_t &zlink,
                                                handler_registry_t &handlers)
{
    {
        module.configure_services (services)
    } -> std::same_as<void>;
    {
        module.configure_zlink (zlink)
    } -> std::same_as<void>;
    {
        module.configure_handlers (handlers)
    } -> std::same_as<void>;
};

} // namespace zlink::framework
