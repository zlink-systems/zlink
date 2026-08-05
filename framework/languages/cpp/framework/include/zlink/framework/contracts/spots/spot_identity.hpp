/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <string>
#include <string_view>

namespace zlink::framework
{

class node_rid_t
{
  public:
    node_rid_t () = default;
    explicit node_rid_t (std::string value);

    static node_rid_t from_string (std::string value);
    std::string_view value () const noexcept;
    bool empty () const noexcept;

  private:
    std::string _value;
};

using spot_id_t = std::string;

namespace detail
{
bool valid_spot_id (std::string_view value) noexcept;
void require_spot_id (std::string_view value);
spot_id_t new_user_spot_id ();
spot_id_t new_entry_spot_id (std::string_view diagnostic_prefix);
bool is_framework_entry_spot_id (std::string_view value) noexcept;
} // namespace detail

} // namespace zlink::framework
