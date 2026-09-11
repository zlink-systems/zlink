/* SPDX-License-Identifier: FSL-1.1-ALv2 */

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
    detail::task_completion_source_t<store_read_result_t> source;
    auto read = read_from_provider (source.task ());
    source.complete (result_t<store_read_result_t>::success (store_missing_t{}));
    return read.result () && std::holds_alternative<store_missing_t> (read.result ().value ())
             ? 0 : 1;
}
