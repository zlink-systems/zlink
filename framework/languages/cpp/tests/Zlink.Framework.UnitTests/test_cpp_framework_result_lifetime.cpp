/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#include <zlink/framework/contracts/errors/result.hpp>

#include <iostream>
#include <stdexcept>
#include <string>

using namespace zlink::framework;

namespace
{
void require (bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error (message);
}

template <typename T> void check_failure ()
{
    const std::string message (256, 'x');
    auto original = detail::with_failure_code (
      detail::with_error_origin (
        detail::with_failure_origin (
          detail::make_boundary_exception (detail::boundary_error_t::disconnected, message),
          detail::failure_origin_t::payload_decode),
        detail::error_origin_t::framework),
      123);
    auto result = detail::result_access_t::failure<T> (std::make_exception_ptr (original));
    const auto *error = result.error ();
    require (error != nullptr, "missing framework error");
    for (int i = 0; i != 32; ++i) {
        require (std::string (error->what ()) == message, "error message lifetime");
        require (error->kind () == framework_error_kind_t::unavailable, "error kind lifetime");
        require (result.error () == error, "error address changed");
        require (result.error_kind () == error->kind (), "result kind mismatch");
        require (error->code () == std::errc::not_connected, "boundary code lost");
        require (detail::failure_origin (*error) == detail::failure_origin_t::payload_decode,
                 "failure origin lost");
        require (detail::error_origin (*error) == detail::error_origin_t::framework,
                 "error origin lost");
        require (detail::failure_code (*error) == 123, "failure code lost");
    }
    auto copy = result;
    auto moved = std::move (copy);
    require (std::string (moved.error ()->what ()) == message, "copied error lifetime");
    try {
        result.value ();
        require (false, "failure value did not throw");
    }
    catch (const framework_exception_t &caught) {
        require (std::string (caught.what ()) == message, "original exception lost");
    }
}

struct derived_error_t : framework_exception_t
{
    derived_error_t () : framework_exception_t (framework_error_kind_t::rejected, "derived") {}
    int detail = 42;
};
} // namespace

int main ()
{
    try {
        check_failure<int> ();
        check_failure<void> ();
        const auto value = result_t<int>::success (7);
        const auto empty = result_t<void>::success ();
        require (value.value () == 7 && !value.error () && !value.exception (), "value success");
        require (empty.has_value () && !empty.error () && !empty.exception (), "void success");
        empty.value ();
        const auto foreign = detail::result_access_t::failure<void> (
          std::make_exception_ptr (std::runtime_error ("foreign")));
        require (!foreign.error () && !foreign.has_value (), "foreign exception classification");
        const auto derived = detail::result_access_t::failure<int> (
          std::make_exception_ptr (derived_error_t ()));
        require (std::string (derived.error ()->what ()) == "derived", "derived base message");
        try {
            derived.value ();
            require (false, "derived exception did not throw");
        }
        catch (const derived_error_t &caught) {
            require (caught.detail == 42, "derived payload lost");
        }
        std::cout << "result lifetime passed; sizeof(int)=" << sizeof (result_t<int>)
                  << " sizeof(void)=" << sizeof (result_t<void>) << '\n';
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << '\n';
        return 1;
    }
}
