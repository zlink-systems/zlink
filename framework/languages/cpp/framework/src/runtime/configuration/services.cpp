/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "service_scope.hpp"

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace zlink::framework::detail
{

struct service_descriptor_t
{
    service_descriptor_t (service_lifetime_t lifetime_,
                          service_collection_t::service_factory_t factory_) :
        lifetime (lifetime_), factory (std::move (factory_))
    {
    }

    service_lifetime_t lifetime;
    service_collection_t::service_factory_t factory;
    std::once_flag initialization;
    std::atomic<bool> published{false};
    std::shared_ptr<void> instance;
};

namespace
{
class singleton_resolution_t
{
  public:
    explicit singleton_resolution_t (service_descriptor_t &descriptor_) :
        descriptor (&descriptor_), previous (current)
    {
        current = this;
    }

    ~singleton_resolution_t () { current = previous; }

    singleton_resolution_t (const singleton_resolution_t &) = delete;
    singleton_resolution_t &operator= (const singleton_resolution_t &) = delete;

    service_descriptor_t *descriptor;
    singleton_resolution_t *previous;
    static thread_local singleton_resolution_t *current;
};

thread_local singleton_resolution_t *singleton_resolution_t::current = nullptr;
} // namespace

class service_instance_store_t
{
  public:
    ~service_instance_store_t () { clear (); }

    std::shared_ptr<void> find (std::type_index type) const
    {
        return find_in (instances_by_type, type);
    }

    std::shared_ptr<void> cache (std::type_index type, std::shared_ptr<void> instance)
    {
        return cache_in (instances_by_type, type, std::move (instance));
    }

    std::shared_ptr<void> find_framework_dependency (std::type_index type) const
    {
        return find_in (framework_dependencies_by_type, type);
    }

    std::shared_ptr<void> cache_framework_dependency (std::type_index type,
                                                      std::shared_ptr<void> instance)
    {
        return cache_in (framework_dependencies_by_type, type, std::move (instance));
    }

    std::shared_ptr<void> track (std::shared_ptr<void> instance)
    {
        instances.push_back (std::move (instance));
        return instances.back ();
    }

    void clear () noexcept
    {
        instances_by_type.clear ();
        framework_dependencies_by_type.clear ();
        while (!instances.empty ()) {
            instances.pop_back ();
        }
    }

  private:
    using instance_index_t = std::unordered_map<std::type_index, std::size_t>;

    std::shared_ptr<void> find_in (const instance_index_t &index, std::type_index type) const
    {
        const auto found = index.find (type);
        return found == index.end () ? nullptr : instances[found->second];
    }

    std::shared_ptr<void>
    cache_in (instance_index_t &index, std::type_index type, std::shared_ptr<void> instance)
    {
        const auto found = index.find (type);
        if (found != index.end ()) {
            return instances[found->second];
        }

        const auto instance_index = instances.size ();
        instances.push_back (std::move (instance));
        try {
            index.emplace (type, instance_index);
        }
        catch (...) {
            instances.pop_back ();
            throw;
        }
        return instances.back ();
    }

    instance_index_t instances_by_type;
    instance_index_t framework_dependencies_by_type;
    std::vector<std::shared_ptr<void>> instances;
};

class service_registry_t
{
  public:
    ~service_registry_t ()
    {
        while (!owned_singletons.empty ()) {
            auto *descriptor = owned_singletons.back ();
            owned_singletons.pop_back ();
            descriptor->instance.reset ();
        }
    }

    service_descriptor_t &add (std::type_index type,
                               service_lifetime_t lifetime,
                               service_collection_t::service_factory_t factory)
    {
        const auto [found, inserted] =
          descriptors.try_emplace (type, lifetime, std::move (factory));
        if (!inserted) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "duplicate service registration");
        }
        return found->second;
    }

    void add_singleton_instance (std::type_index type, std::shared_ptr<void> instance)
    {
        auto &descriptor = add (type, service_lifetime_t::singleton, {});
        try {
            own_singleton (descriptor, std::move (instance));
        }
        catch (...) {
            descriptors.erase (type);
            throw;
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

    std::shared_ptr<void> singleton (service_descriptor_t &descriptor, service_provider_t &provider)
    {
        if (descriptor.published.load (std::memory_order_acquire)) {
            // Publication makes the owning handle safe to read without a store lock.
            return descriptor.instance;
        }
        // Reject a synchronous cycle before call_once can wait for this thread.
        for (auto *active = singleton_resolution_t::current; active; active = active->previous) {
            if (active->descriptor == &descriptor) {
                throw framework_exception_t (framework_error_kind_t::invalid_operation,
                                             "recursive singleton resolution");
            }
        }
        std::call_once (descriptor.initialization, [&] {
            singleton_resolution_t resolution (descriptor);
            own_singleton (descriptor, descriptor.factory (provider));
        });
        return descriptor.instance;
    }

  private:
    void own_singleton (service_descriptor_t &descriptor, std::shared_ptr<void> instance)
    {
        // Ownership order is shared; factories run outside this critical section.
        std::lock_guard<std::mutex> guard (ownership_mutex);
        owned_singletons.push_back (&descriptor);
        descriptor.instance = std::move (instance);
        descriptor.published.store (true, std::memory_order_release);
    }

    std::unordered_map<std::type_index, service_descriptor_t> descriptors;
    std::mutex ownership_mutex;
    std::vector<service_descriptor_t *> owned_singletons;
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
    service_instance_store_t instances;
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

service_provider_t detail::service_scope_access_t::create_child (service_provider_t &provider,
                                                                 service_scope_kind_t kind)
{
    return service_provider_t (provider._registry,
                               std::make_shared<service_scope_state_t> (true, kind));
}

detail::service_scope_kind_t
detail::service_scope_access_t::kind (const service_provider_t &provider) noexcept
{
    return provider._scope->kind;
}

void service_provider_t::close () noexcept
{
    if (_scope) {
        _scope->closed = true;
        _scope->instances.clear ();
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
            return _registry->singleton (descriptor, *this);
        case service_lifetime_t::scoped: {
            if (!_scope->scoped_context) {
                throw framework_exception_t (framework_error_kind_t::protocol_error,
                                             "scoped service requires a service scope");
            }
            if (auto instance = _scope->instances.find (type)) {
                return instance;
            }
            return _scope->instances.cache (type, descriptor.factory (*this));
        }
        case service_lifetime_t::transient: {
            return _scope->instances.track (descriptor.factory (*this));
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

std::shared_ptr<void> service_provider_t::cached_framework_dependency (std::type_index type)
{
    if (is_closed ()) {
        throw detail::make_boundary_exception (detail::boundary_error_t::shutdown,
                                               "service provider is closed");
    }
    return _scope->instances.find_framework_dependency (type);
}

std::shared_ptr<void>
service_provider_t::cache_framework_dependency (std::type_index type,
                                                std::shared_ptr<void> instance)
{
    if (is_closed ()) {
        throw detail::make_boundary_exception (detail::boundary_error_t::shutdown,
                                               "service provider is closed");
    }
    return _scope->instances.cache_framework_dependency (type, std::move (instance));
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
    _registry->add (type, lifetime, std::move (factory));
    return *this;
}

service_collection_t &service_collection_t::add_singleton_instance (std::type_index type,
                                                                    std::shared_ptr<void> instance)
{
    _registry->add_singleton_instance (type, std::move (instance));
    return *this;
}

service_provider_t service_collection_t::build_provider () const
{
    return service_provider_t (_registry,
                               std::make_shared<detail::service_scope_state_t> (
                                 false, detail::service_scope_kind_t::handler_invocation));
}

bool service_collection_t::contains (std::type_index type) const
{
    return _registry->contains (type);
}

} // namespace zlink::framework
