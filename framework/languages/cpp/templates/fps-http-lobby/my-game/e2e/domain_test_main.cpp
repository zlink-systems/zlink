#include <exception>
#include <iostream>

void run_domain_checks ();
void run_lobby_domain_checks ();

int main ()
{
    try {
        run_domain_checks ();
        run_lobby_domain_checks ();
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << '\n';
        return 1;
    }
}
