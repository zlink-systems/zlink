/* SPDX-License-Identifier: MPL-2.0 */
#include "../support/capturing_coroutine_scan.hpp"

#include <iostream>

int main ()
{
    struct case_t
    {
        const char *name;
        const char *source;
        bool violation;
    };
    const case_t cases[]{
      {"capture after await", "[value]() -> task_t<void> { co_await wait(); use(value); }", true},
      {"reference capture", "[&value] { co_await wait(); co_return value; }", true},
      {"init capture",
       "[v = std::move(value)]() mutable -> task_t<int> { co_await wait(); co_return v; }", true},
      {"default reference",
       "int value; auto f = [&]() -> task_t<int> { co_await wait(); co_return value; }", true},
      {"default value", "int value; auto f = [=] { co_await wait(); co_return value; }", true},
      {"implicit member", "[this] { co_await wait(); co_return _value; }", true},
      {"explicit this", "[this] { co_await wait(); co_return this->value; }", true},
      {"multiple awaits", "[v] { co_await work(v); co_await work(v); }", true},
      {"within expression", "[v] { auto x = co_await work() + v; }", true},
      {"after condition", "[v] { if (co_await work()) use(v); }", true},
      {"yield", "[v] { co_yield 1; co_return v; }", true},
      {"nested capture read", "[v] { co_await work(); auto f = [v] { return v; }; }", true},
      {"default callable",
       "void f(std::function<void()> work) { auto f = [&] { co_await wait(); work(); }; }", true},
      {"default structured local",
       "int outer; auto f = [&] { auto [a, b] = co_await wait(); co_return a + b; };", false},
      {"default type", "auto f = [&] { co_await wait(); co_return ns::result<some_type>(); };",
       false},
      {"returning branch",
       "[v](bool b) { if (b) { co_return co_await work(v); } co_return co_await other(v); }",
       false},
      {"exclusive branches",
       "[v](bool b) { if (b) { co_await work(v); } else { co_await other(v); } }", false},
      {"branch merge",
       "[v](bool b) { if (b) { co_await work(v); } else { co_await other(v); } use(v); }", true},
      {"branch without return", "[v](bool b) { if (b) { co_await work(v); } use(v); }", true},
      {"branch capture after await", "[v](bool b) { if (b) { co_await work(); co_return v; } }",
       true},
      {"captureless", "[](int v) -> task_t<int> { co_await wait(); co_return v; }", false},
      {"capture before await", "[v] { use(v); co_await wait(); co_return; }", false},
      {"await operand", "[v] { co_return co_await work(v); }", false},
      {"qualified await", "[v] { co_return co_await ns::work<int>(v); }", false},
      {"parenthesized await", "[v] { co_return co_await (work(v)); }", false},
      {"no explicit suspension", "[v]() -> task_t<int> { co_return v; }", false},
      {"named coroutine owns values",
       "task_t<int> run(int v) { co_await wait(); co_return v; } "
       "auto f = [v] { return run(v); };",
       false},
      {"named coroutine owns shared state",
       "task_t<void> run(shared_ptr<state_t> state) { co_await wait(); use(state); } "
       "auto f = [state] { return run(state); };",
       false},
      {"ordinary lambda", "[v] { return work(v); }", false},
      {"init source is not capture", "[v = value] { co_await work(); co_return value; }", false},
      {"member is not capture", "[v](auto x) { co_await work(); co_return x.v; }", false},
      {"local after await",
       "[&](int parameter) { co_await work(); int local = 1; co_return local + parameter; }",
       false},
      {"nested coroutine only", "[v] { auto f = [] { co_await work(); }; use(v); }", false},
      {"nested body only", "[v] { co_await work(); auto f = [] { return global; }; }", false},
      {"comments", "[v] { /* co_await work(); use(v); } */ return v; }", false},
      {"line comment", "[v] { // co_await work();\n return v; }", false},
      {"digit separator", "[v] { auto n = 1'000; co_await wait(); use(v); auto c = 'x'; }", true},
      {"literal prefix", "[L] { co_await wait(); auto c = L'x'; }", false},
      {"strings", R"test([v] { auto s = "co_await work(); use(v); }"; return v; })test", false},
      {"raw strings",
       R"test([v] { auto s = R"tag(co_await work(); use(v); })tag"; return v; })test", false},
      {"nonempty attributes", "[] [[nodiscard]] () { co_await work(); return v; }", false},
    };
    int failures = 0;
    for (const auto &test : cases) {
        const bool violation =
          !zlink::framework::tests::capturing_coroutine_reads (test.source).empty ();
        if (violation != test.violation) {
            std::cerr << test.name << ": expected violation=" << test.violation << '\n';
            ++failures;
        }
    }
    if (failures)
        return 1;
    std::cout << "CP.51 scanner: " << std::size (cases) << " cases passed\n";
    return 0;
}
