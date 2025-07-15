#include <core/krka.hpp>
#include <cstdlib>
#include <iostream>
#include <signal.h>

using ::std::unique_ptr;

unique_ptr<KrkaWM> wm;

void signalHandler(int signum) {
    wm->logger_.err() << "Received signal " << signum
                      << ", terminating window manager." << std::endl;
    if (wm) {
        wm->~KrkaWM();
    }
    exit(EXIT_SUCCESS);
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    signal(SIGQUIT, signalHandler);
    signal(SIGABRT, signalHandler);

    unique_ptr<KrkaWM> wm(KrkaWM::Create());
    if (!wm) {
        std::cerr << "Failed to create window manager." << std::endl;
        return EXIT_FAILURE;
    }

    wm->Run();
    wm->~KrkaWM();
    return EXIT_SUCCESS;
}
