/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "context_options.hpp"
#include "../Errors/errors.hpp"

#include <memory>
#include <cstdint>
#include <string_view>

namespace zlink
{

class context_t;
namespace detail
{
struct context_access_t;
} // namespace detail

/**
 * @brief A messaging context: the factory and owner of sockets.
 *
 * @note Every socket created from a context is owned by the caller
 *       and must be closed before the context is terminated.  Calling term()
 *       interrupts blocking operations on all sockets created from it.
 */
class context_t
{
  public:
    context_t ();
    explicit context_t (io_thread_count_t io_threads_);
    ~context_t ();

    context_t (context_t &&other) noexcept;
    context_t &operator= (context_t &&other) noexcept;

    context_t (const context_t &) = delete;
    context_t &operator= (const context_t &) = delete;

    bool valid () const noexcept;

    /// @brief Terminates the context, interrupting blocking operations on sockets without closing them.
    void shutdown ();
    /// @brief Terminates and destroys the context.
    void term ();

    context_options_t options () { return context_options_t (*this); }

    /// @brief Recomputes automatic high-water marks for sockets configured with an auto-HWM profile.
    void recalculate_auto_hwm ();

  private:
    friend class context_options_t;
    friend struct detail::context_access_t;

    void term_noexcept () noexcept;
    int get_option_raw (int option_, int *error_out_) const;
    config_result_t set_option_raw (int option_, int value_);
    config_result_t set_option_data_raw (int option_, std::string_view value_);
    uint64_t get_option_uint64_raw (int option_) const;
    config_result_t set_option_uint64_raw (int option_, uint64_t value_);

    struct impl;
    std::unique_ptr<impl> _impl;
};

} // namespace zlink
