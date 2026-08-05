/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "service_scope.hpp"

#include <map>
#include <utility>
#include <vector>

namespace zlink::framework::detail
{

struct service_descriptor_t
{
    service_lifetime_t lifetime;
    service_collection_t::service_factory_t factory;
    std::shared_ptr<void> singleton_instance;
};

class service_registry_t
{
  public:
    void add (std::type_index type, service_descriptor_t descriptor)
    {
        const auto [_, inserted] = descriptors.emplace (type, std::move (descriptor));
        if (!inserted) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "duplicate service registration");
        }
    }

    service_descriptor_t &required (std::type_index type)
    {
        const auto found = descriptors.find (type);
        if (found == descriptors.end ()) {
            throw framework_exception_t (framework_error_kind_t::not_found,
                                         std::string ("service is not registered: ")
                                           + type.name ());
        }
        return found->second;
    }

    bool contains (std::type_index type) const
    {
        return descriptors.find (type) != descriptors.end ();
    }

    std::map<std::type_index, service_descriptor_t> descriptors;
};

class service_scope_state_t
{
  public:
    service_scope_state_t (bool scoped_context, service_scope_kind_t kind) :
        scoped_context (scoped_context), kind (kind)
    {
    }

    bool scoped_context;
    service_scope_kind_t kind;
    bool closed = false;
    std::map<std::type_index, std::shared_ptr<void>> scoped_instances;
    std::vector<std::shared_ptr<void>> transient_instances;
};

} // namespace zlink::framework::detail

namespace zlink::framework
{

service_provider_t::service_provider_t () :
    _registry (std::make_shared<detail::service_registry_t> ()),
    _scope (std::make_shared<detail::service_scope_state_t> (
      false, detail::service_scope_kind_t::handler_invocation))
{
}

service_provider_t::service_provider_t (std::shared_ptr<detail::service_registry_t> registry,
                                        std::shared_ptr<detail::service_scope_state_t> scope) :
    _registry (std::move (registry)), _scope (std::move (scope))
{
}

service_provider_t::~service_provider_t () = default;

service_provider_t::service_provider_t (service_provider_t &&) noexcept = default;

service_provider_t &service_provider_t::operator= (service_provider_t &&) noexcept = default;

detail::service_scope_t detail::service_scope_t::create (service_provider_t &provider,
                                                         service_scope_kind_t kind)
{
    if (provider.is_closed ()) {
        throw detail::make_boundary_exception (detail::boundary_error_t::shutdown,
                                     "service provider is closed");
    }
    return service_scope_t (service_scope_access_t::create_child (provider, kind));
}

service_provider_t detail::service_scope_access_t::create_child (
  service_provider_t &provider, service_scope_kind_t kind)
{
    return service_provider_t (
      provider._registry, std::make_shared<service_scope_state_t> (true, kind));
}

detail::service_scope_kind_t detail::service_scope_access_t::kind (
  const service_provider_t &provider) noexcept
{
    return provider._scope->kind;
}

void service_provider_t::close () noexcept
{
    if (_scope) {
        _scope->closed = true;
        _scope->scoped_instances.clear ();
        _scope->transient_instances.clear ();
    }
}

bool service_provider_t::is_closed () const noexcept
{
    return !_scope || _scope->closed;
}

std::shared_ptr<void> service_provider_t::resolve (std::type_index type)
{
    if (is_closed ()) {
        throw detail::make_boundary_exception (detail::boundary_error_t::shutdown,
                                     "service provider is closed");
    }

    auto &descriptor = _registry->required (type);
    switch (descriptor.lifetime) {
        case service_lifetime_t::singleton:
            if (!descriptor.singleton_instance) {
                descriptor.singleton_instance = descriptor.factory (*this);
            }
            return descriptor.singleton_instance;
        case service_lifetime_t::scoped: {
            if (!_scope->scoped_context) {
                throw framework_exception_t (framework_error_kind_t::protocol_error,
                                             "scoped service requires a service scope");
            }
            const auto found = _scope->scoped_instances.find (type);
            if (found != _scope->scoped_instances.end ()) {
                return found->second;
            }
            auto instance = descriptor.factory (*this);
            _scope->scoped_instances.emplace (type, instance);
            return instance;
        }
        case service_lifetime_t::transient: {
            auto instance = descriptor.factory (*this);
            _scope->transient_instances.push_back (instance);
            return instance;
        }
    }

    throw framework_exception_t (framework_error_kind_t::internal_failure,
                                 "unknown service lifetime");
}

std::shared_ptr<void> service_provider_t::try_resolve (std::type_index type)
{
    if (is_closed ()) {
        throw detail::make_boundary_exception (detail::boundary_error_t::shutdown,
                                     "service provider is closed");
    }
    if (!_registry->contains (type)) {
        return nullptr;
    }
    return resolve (type);
}

detail::service_scope_t::service_scope_t (service_provider_t provider) :
    _provider (std::move (provider))
{
}

detail::service_scope_t::~service_scope_t ()
{
    close ();
}

detail::service_scope_t::service_scope_t (service_scope_t &&) noexcept = default;

detail::service_scope_t &detail::service_scope_t::operator= (service_scope_t &&) noexcept = default;

void detail::service_scope_t::close () noexcept
{
    _provider.close ();
}

detail::service_scope_kind_t detail::service_scope_t::kind () const noexcept
{
    return service_scope_access_t::kind (_provider);
}

service_collection_t::service_collection_t () :
    _registry (std::make_shared<detail::service_registry_t> ())
{
}

service_collection_t::~service_collection_t () = default;

service_collection_t::service_collection_t (service_collection_t &&) noexcept = default;

service_collection_t &service_collection_t::operator= (service_collection_t &&) noexcept = default;

service_collection_t &service_collection_t::add_descriptor (std::type_index type,
                                                            service_lifetime_t lifetime,
                                                            service_factory_t factory)
{
    _registry->add (type, detail::service_descriptor_t{lifetime, std::move (factory), nullptr});
    return *this;
}

service_provider_t service_collection_t::build_provider () const
{
    return service_provider_t (_registry, std::make_shared<detail::service_scope_state_t> (
                                            false, detail::service_scope_kind_t::handler_invocation));
}

bool service_collection_t::contains (std::type_index type) const
{
    return _registry->contains (type);
}

} // namespace zlink::framework
