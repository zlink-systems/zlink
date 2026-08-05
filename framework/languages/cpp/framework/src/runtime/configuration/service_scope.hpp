/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/configuration/services.hpp>

namespace zlink::framework::detail
{

enum class service_scope_kind_t
{
    handler_invocation = 0,
    stream_session = 1,
    spot_activation = 2,
    entry_spot = 3,
    actor_creation = 4
};

class service_scope_access_t
{
  public:
    static service_provider_t create_child (service_provider_t &provider,
                                            service_scope_kind_t kind);
    static service_scope_kind_t kind (const service_provider_t &provider) noexcept;
};

class service_scope_t
{
  public:
    static service_scope_t create (service_provider_t &provider,
                                   service_scope_kind_t kind =
                                     service_scope_kind_t::handler_invocation);

    explicit service_scope_t (service_provider_t provider);
    ~service_scope_t ();

    service_scope_t (service_scope_t &&) noexcept;
    service_scope_t &operator= (service_scope_t &&) noexcept;
    service_scope_t (const service_scope_t &) = delete;
    service_scope_t &operator= (const service_scope_t &) = delete;

    template <typename T> T &get_required () { return _provider.get_required<T> (); }

    service_provider_t &provider () noexcept { return _provider; }
    service_scope_kind_t kind () const noexcept;
    void close () noexcept;

  private:
    service_provider_t _provider;
};

} // namespace zlink::framework::detail
