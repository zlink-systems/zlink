/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework.hpp>
#include "runtime/configuration/service_scope.hpp"

#include <memory>

namespace
{

struct singleton_t
{
    int value = 1;
};

struct scoped_t
{
    static inline int destroyed = 0;
    ~scoped_t () { ++destroyed; }
};

struct transient_t
{
};

struct dependent_t
{
    explicit dependent_t (singleton_t &singleton) : value (singleton.value) {}
    int value;
};

struct second_dependent_t
{
    second_dependent_t (singleton_t &singleton, dependent_t &dependent) :
        value (singleton.value + dependent.value)
    {
    }
    int value;
};

struct logger_dependent_t
{
    explicit logger_dependent_t (zlink::framework::logger_t<logger_dependent_t> &logger) :
        logger (logger)
    {
    }

    void write () { logger.info ("dependency logger resolved"); }

    zlink::framework::logger_t<logger_dependent_t> logger;
};

} // namespace

int main ()
{
    zlink::framework::service_collection_t services;
    services.add_singleton<singleton_t> ();
    services.add_scoped<scoped_t> ();
    services.add_transient<transient_t> ();
    services.add_transient<dependent_t, singleton_t> ();
    services.add_transient<second_dependent_t, singleton_t, dependent_t> ();

    auto provider = services.build_provider ();

    auto &singleton_a = provider.get_required<singleton_t> ();
    auto &singleton_b = provider.get_required<singleton_t> ();
    if (&singleton_a != &singleton_b) {
        return 1;
    }

    auto &transient_a = provider.get_required<transient_t> ();
    auto &transient_b = provider.get_required<transient_t> ();
    if (&transient_a == &transient_b) {
        return 2;
    }

    if (provider.get_required<dependent_t> ().value != 1) {
        return 3;
    }

    if (provider.get_required<second_dependent_t> ().value != 2) {
        return 12;
    }

    struct unregistered_t
    {
    };
    if (provider.get<unregistered_t> ().has_value ()) {
        return 13;
    }
    const auto optional_singleton = provider.get<singleton_t> ();
    if (!optional_singleton.has_value () || &optional_singleton->get () != &singleton_a) {
        return 14;
    }

    bool scoped_from_root_failed = false;
    try {
        (void) provider.get_required<scoped_t> ();
    }
    catch (const zlink::framework::framework_exception_t &error) {
        scoped_from_root_failed =
          error.kind () == zlink::framework::framework_error_kind_t::protocol_error;
    }
    if (!scoped_from_root_failed) {
        return 4;
    }

    scoped_t::destroyed = 0;
    scoped_t *first_scope_instance = nullptr;
    {
        auto scope = zlink::framework::detail::service_scope_t::create (
          provider, zlink::framework::detail::service_scope_kind_t::stream_session);
        if (scope.kind () != zlink::framework::detail::service_scope_kind_t::stream_session) {
            return 5;
        }
        auto &scoped_a = scope.get_required<scoped_t> ();
        auto &scoped_b = scope.get_required<scoped_t> ();
        if (&scoped_a != &scoped_b) {
            return 6;
        }
        first_scope_instance = &scoped_a;
    }
    if (scoped_t::destroyed != 1) {
        return 7;
    }

    {
        auto scope = zlink::framework::detail::service_scope_t::create (
          provider, zlink::framework::detail::service_scope_kind_t::spot_activation);
        if (scope.kind () != zlink::framework::detail::service_scope_kind_t::spot_activation) {
            return 8;
        }
        auto &scoped_c = scope.get_required<scoped_t> ();
        if (&scoped_c == first_scope_instance) {
            return 9;
        }
    }

    {
        auto scope = zlink::framework::detail::service_scope_t::create (
          provider, zlink::framework::detail::service_scope_kind_t::actor_creation);
        (void) scope.get_required<scoped_t> ();
    }
    {
        auto scope = zlink::framework::detail::service_scope_t::create (
          provider, zlink::framework::detail::service_scope_kind_t::entry_spot);
        (void) scope.get_required<scoped_t> ();
    }

    provider.close ();
    bool shutdown_resolve_failed = false;
    try {
        (void) provider.get_required<singleton_t> ();
    }
    catch (const zlink::framework::framework_exception_t &error) {
        shutdown_resolve_failed =
          zlink::framework::detail::boundary_state (error) == zlink::framework::detail::boundary_error_t::shutdown;
    }
    if (!shutdown_resolve_failed) {
        return 10;
    }

    bool duplicate_failed = false;
    try {
        zlink::framework::service_collection_t duplicate_services;
        duplicate_services.add_singleton<singleton_t> ();
        duplicate_services.add_singleton<singleton_t> ();
    }
    catch (const zlink::framework::framework_exception_t &error) {
        duplicate_failed =
          error.kind () == zlink::framework::framework_error_kind_t::protocol_error;
    }
    if (!duplicate_failed) {
        return 11;
    }

    zlink::framework::logging_builder_t logging;
    zlink::framework::service_collection_t logging_services;
    logging_services.add_singleton<zlink::framework::logger_factory_t> (
      std::make_unique<zlink::framework::logger_factory_t> (logging.factory ()));
    logging_services
      .add_transient<logger_dependent_t, zlink::framework::logger_t<logger_dependent_t>> ();
    auto logging_provider = logging_services.build_provider ();
    logging_provider.get_required<logger_dependent_t> ().write ();
    if (logging.captured_records ().empty ()) {
        return 13;
    }

    return 0;
}
