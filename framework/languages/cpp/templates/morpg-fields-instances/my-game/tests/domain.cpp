#include "domain_checks.hpp"
#include <iostream>

int main ()
{
    try {
        const auto count = MyGame::DomainChecks::run ();
        std::cout << "Domain checks: " << count << "/" << count << " passed\n";
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << '\n';
        return 1;
    }
}
