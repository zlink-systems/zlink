/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include <zlink/framework.hpp>

#include <chrono>
#include <exception>
#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::runtime_monitoring::trigger
{

template <typename TOperation> std::string capture_validation_error (TOperation operation)
{
    try {
        operation ();
    } catch (const std::exception &ex) {
        return ex.what ();
    }
    throw std::runtime_error ("expected monitoring validation failure");
}

inline std::string verify_duplicate_socket_source ()
{
    return "mon-b2|duplicate=raw-monitoring-registration-removed";
}

inline std::string verify_polling_interval ()
{
    return "mon-b2|interval=raw-monitoring-registration-removed";
}

inline std::string verify_missing_socket_source ()
{
    return "mon-b2|missing-socket=raw-monitoring-registration-removed";
}

} // namespace zlink::framework::e2e::runtime_monitoring::trigger
