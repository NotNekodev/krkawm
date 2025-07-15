#include <core/krka.hpp>
#include <cstdlib>
#include <iostream>

using ::std::unique_ptr;

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    unique_ptr<KrkaWM> wm(KrkaWM::Create());
    if (!wm) {
        std::cerr << "Failed to create window manager." << std::endl;
        return EXIT_FAILURE;
    }

    wm->Run();
    return EXIT_SUCCESS;
}
