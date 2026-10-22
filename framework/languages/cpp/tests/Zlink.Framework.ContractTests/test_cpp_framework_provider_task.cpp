/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include <zlink/framework/contracts/locations/stores.hpp>

using namespace zlink::framework;

// A provider may implement its asynchronous SPI using only the opt-in
// provider-abstractions target, without linking the Framework runtime.
task_t<store_read_result_t> read_from_provider (task_t<store_read_result_t> pending)
{
    co_return co_await pending;
}

int main ()
{
    task_t<int> ready (result_t<int>::success (7));
    if (ready.result ().value () != 7)
        return 2;
    task_completion_source_t<store_read_result_t> ownerless_source;
    auto rejected = read_from_provider (ownerless_source.task ());
    if (rejected.result ()
        || rejected.result ().error ()->kind () != framework_error_kind_t::invalid_operation)
        return 3;
    if (!ownerless_source.complete (result_t<store_read_result_t>::success (store_missing_t{})))
        return 4;
    zlink::framework::completion_owner_t completion_owner;
    task_completion_source_t<store_read_result_t> source;
    auto read = read_from_provider (source.task ());
    source.complete (result_t<store_read_result_t>::success (store_missing_t{}));
    return read.result () && std::holds_alternative<store_missing_t> (read.result ().value ()) ? 0
                                                                                               : 1;
}
