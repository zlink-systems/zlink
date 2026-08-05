/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <string>
#include <string_view>

namespace zlink::stream_connector::detail
{

enum class connector_backend_kind_t
{
    asio,
    unreal_sockets
};

struct connector_backend_contract_t
{
    connector_backend_kind_t backend = connector_backend_kind_t::asio;
    std::string transport_name = "asio";
    bool owns_connection = true;
    bool projects_connection_state = true;
};

inline std::string_view backend_name (connector_backend_kind_t backend) noexcept
{
    switch (backend) {
        case connector_backend_kind_t::asio:
            return "asio";
        case connector_backend_kind_t::unreal_sockets:
            return "unreal-sockets";
    }
    return "unknown";
}

} // namespace zlink::stream_connector::detail
