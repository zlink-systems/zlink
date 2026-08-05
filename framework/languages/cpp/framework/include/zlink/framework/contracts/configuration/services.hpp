/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/errors/error.hpp>
#include <zlink/framework/contracts/configuration/logging.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <typeindex>
#include <type_traits>

namespace zlink::framework
{

template <typename... T> struct dependency_list_t
{
};

namespace detail
{
class service_registry_t;
class service_scope_state_t;
class service_scope_access_t;
} // namespace detail

enum class service_lifetime_t
{
    singleton = 0,
    scoped = 1,
    transient = 2
};

class service_provider_t
{
  public:
    service_provider_t ();
    ~service_provider_t ();

    service_provider_t (service_provider_t &&) noexcept;
    service_provider_t &operator= (service_provider_t &&) noexcept;
    service_provider_t (const service_provider_t &) = default;
    service_provider_t &operator= (const service_provider_t &) = default;

    template <typename T> T &get_required ()
    {
        return *std::static_pointer_cast<T> (resolve (std::type_index (typeid (T))));
    }

    template <typename T> std::optional<std::reference_wrapper<T>> get ()
    {
        auto resolved = try_resolve (std::type_index (typeid (T)));
        if (!resolved) {
            return std::nullopt;
        }
        return std::ref (*std::static_pointer_cast<T> (resolved));
    }

    void close () noexcept;
    bool is_closed () const noexcept;

  private:
    friend class service_collection_t;
    friend class detail::service_scope_access_t;

    service_provider_t (std::shared_ptr<detail::service_registry_t> registry,
                        std::shared_ptr<detail::service_scope_state_t> scope);

    std::shared_ptr<void> resolve (std::type_index type);
    std::shared_ptr<void> try_resolve (std::type_index type);

    std::shared_ptr<detail::service_registry_t> _registry;
    std::shared_ptr<detail::service_scope_state_t> _scope;
};

class service_collection_t
{
  public:
    using service_factory_t = std::function<std::shared_ptr<void> (service_provider_t &)>;

    service_collection_t ();
    ~service_collection_t ();

    service_collection_t (service_collection_t &&) noexcept;
    service_collection_t &operator= (service_collection_t &&) noexcept;
    service_collection_t (const service_collection_t &) = delete;
    service_collection_t &operator= (const service_collection_t &) = delete;

    template <typename T> service_collection_t &add_singleton ()
    {
        static_assert (std::is_default_constructible_v<T>,
                       "add_singleton<T>() requires a default constructor");
        return add_factory<T> ([] (service_provider_t &) { return std::make_unique<T> (); },
                               service_lifetime_t::singleton);
    }

    template <typename T, typename... TDependencies>
    requires (sizeof...(TDependencies) > 0) service_collection_t &add_singleton ()
    {
        return add_injected<T, TDependencies...> (service_lifetime_t::singleton);
    }

    template <typename T> service_collection_t &add_singleton (std::unique_ptr<T> instance)
    {
        auto shared = std::shared_ptr<T> (std::move (instance));
        return add_descriptor (std::type_index (typeid (T)), service_lifetime_t::singleton,
                               [shared] (service_provider_t &) { return shared; });
    }

    template <typename T> service_collection_t &add_scoped ()
    {
        static_assert (std::is_default_constructible_v<T>,
                       "add_scoped<T>() requires a default constructor");
        return add_factory<T> ([] (service_provider_t &) { return std::make_unique<T> (); },
                               service_lifetime_t::scoped);
    }

    template <typename T, typename... TDependencies>
    requires (sizeof...(TDependencies) > 0) service_collection_t &add_scoped ()
    {
        return add_injected<T, TDependencies...> (service_lifetime_t::scoped);
    }

    template <typename T> service_collection_t &add_transient ()
    {
        static_assert (std::is_default_constructible_v<T>,
                       "add_transient<T>() requires a default constructor");
        return add_factory<T> ([] (service_provider_t &) { return std::make_unique<T> (); },
                               service_lifetime_t::transient);
    }

    template <typename T, typename... TDependencies>
    requires (sizeof...(TDependencies) > 0) service_collection_t &add_transient ()
    {
        return add_injected<T, TDependencies...> (service_lifetime_t::transient);
    }

    template <typename T, typename TFactory>
    service_collection_t &add_factory (TFactory factory,
                                       service_lifetime_t lifetime = service_lifetime_t::transient)
    {
        return add_descriptor (
          std::type_index (typeid (T)), lifetime,
          [factory = std::move (factory)] (service_provider_t &provider) mutable {
              using factory_result_t = decltype (factory (provider));
              if constexpr (std::is_same_v<factory_result_t, std::unique_ptr<T>>) {
                  return std::shared_ptr<void> (factory (provider).release ());
              } else if constexpr (std::is_same_v<factory_result_t, std::shared_ptr<T>>) {
                  return std::static_pointer_cast<void> (factory (provider));
              } else {
                  return std::shared_ptr<void> (std::make_shared<T> (factory (provider)));
              }
          });
    }

    service_provider_t build_provider () const;
    bool contains (std::type_index type) const;

  private:
    template <typename T> struct logger_dependency_t : std::false_type
    {
    };

    template <typename TCategory> struct logger_dependency_t<logger_t<TCategory>> : std::true_type
    {
        using category_type = TCategory;
    };

    template <typename T> service_collection_t &add_framework_dependency ()
    {
        if constexpr (logger_dependency_t<T>::value) {
            if (!contains (std::type_index (typeid (T)))) {
                return add_factory<T> (
                  [] (service_provider_t &provider) {
                      auto &factory = provider.get_required<logger_factory_t> ();
                      using category_t = typename logger_dependency_t<T>::category_type;
                      if constexpr (std::is_void_v<category_t>) {
                          return factory.create ("zlink.framework");
                      } else {
                          return factory.template create<category_t> ();
                      }
                  },
                  service_lifetime_t::transient);
            }
        }
        return *this;
    }

    template <typename T, typename... TDependencies>
    service_collection_t &add_injected (service_lifetime_t lifetime)
    {
        static_assert (std::is_constructible_v<T, TDependencies &...>,
                       "injected service constructor must accept dependencies by reference");
        (add_framework_dependency<TDependencies> (), ...);
        return add_factory<T> (
          [] (service_provider_t &provider) {
              return std::make_unique<T> (provider.get_required<TDependencies> ()...);
          },
          lifetime);
    }

    service_collection_t &
    add_descriptor (std::type_index type, service_lifetime_t lifetime, service_factory_t factory);

    std::shared_ptr<detail::service_registry_t> _registry;
};

namespace detail
{

template <typename T> concept static_dependency_types = requires
{
    typename T::dependency_types;
};

template <typename T> struct handler_dependencies_t
{
    using type = dependency_list_t<>;
};

template <static_dependency_types T> struct handler_dependencies_t<T>
{
    using type = typename T::dependency_types;
};

template <typename THandler, typename TDependencies> struct injected_handler_registrar_t;

template <typename THandler, typename... TDependencies>
struct injected_handler_registrar_t<THandler, dependency_list_t<TDependencies...>>
{
    static void add (service_collection_t &services)
    {
        if constexpr (sizeof...(TDependencies) == 0) {
            services.add_transient<THandler> ();
        } else {
            services.add_transient<THandler, TDependencies...> ();
        }
    }
};

} // namespace detail

} // namespace zlink::framework
