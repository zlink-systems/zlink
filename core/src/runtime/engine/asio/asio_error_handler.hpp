/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_ASIO_ERROR_HANDLER_HPP_INCLUDED__
#define __ZLINK_ASIO_ERROR_HANDLER_HPP_INCLUDED__

#include <boost/asio/error.hpp>
#include <boost/system/error_code.hpp>
#include "engine/i_engine.hpp"

namespace zlink
{
namespace asio_error
{

//  Error severity levels
enum class severity
{
    ignore,      //  Operation cancelled, not an error
    normal,      //  Expected condition (EOF, connection reset by peer)
    recoverable, //  Temporary failure, can retry
    fatal        //  Protocol violation, internal error
};

//  Error categories for detailed handling
enum class category
{
    cancelled,
    connection_closed,
    connection_refused,
    connection_reset,
    timeout,
    protocol_error,
    resource_exhausted,
    unknown
};

//  Error information structure
struct error_info_t
{
    severity sev;
    category cat;
    i_engine::error_reason_t zlink_reason;
};

//  Classify Boost.Asio error codes into severity and category
inline error_info_t classify (const boost::system::error_code &ec)
{
    //  Operation cancelled (normal during shutdown)
    if (ec == boost::asio::error::operation_aborted)
        return {severity::ignore, category::cancelled, i_engine::connection_error};

    //  Connection closed by peer (normal shutdown)
    if (ec == boost::asio::error::eof)
        return {severity::normal, category::connection_closed, i_engine::connection_error};

    //  Connection actively refused
    if (ec == boost::asio::error::connection_refused)
        return {severity::normal, category::connection_refused, i_engine::connection_error};

    //  Connection reset by peer
    if (ec == boost::asio::error::connection_reset)
        return {severity::normal, category::connection_reset, i_engine::connection_error};

    //  Broken pipe (write to closed connection)
    if (ec == boost::asio::error::broken_pipe)
        return {severity::normal, category::connection_closed, i_engine::connection_error};

    //  Timeout
    if (ec == boost::asio::error::timed_out)
        return {severity::recoverable, category::timeout, i_engine::timeout_error};

    //  Resource exhaustion (too many open files, etc.)
    if (ec == boost::asio::error::no_descriptors)
        return {severity::recoverable, category::resource_exhausted, i_engine::connection_error};

    //  Would block (shouldn't happen in async, but handle gracefully)
    if (ec == boost::asio::error::would_block || ec == boost::asio::error::try_again)
        return {severity::recoverable, category::unknown, i_engine::connection_error};

    //  Network unreachable / host unreachable
    if (ec == boost::asio::error::network_unreachable || ec == boost::asio::error::host_unreachable)
        return {severity::recoverable, category::connection_refused, i_engine::connection_error};

    //  Unknown error (treat as fatal)
    return {severity::fatal, category::unknown, i_engine::connection_error};
}

//  Helper function to check if error should terminate connection
inline bool is_terminal (const error_info_t &err)
{
    return err.sev == severity::normal || err.sev == severity::fatal;
}

//  Helper function to check if error should be ignored
inline bool should_ignore (const error_info_t &err)
{
    return err.sev == severity::ignore;
}

} // namespace asio_error
} // namespace zlink

#endif
