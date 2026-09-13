#include "evo/Application.hpp"
#include "evo_boot_log.h"
#include "evo_boot_trace.h"

#ifdef EVO_HAVE_BUILD_ID
#include "evo_build_id.h"
#endif

int main(int argc, char* argv[]) {
#ifdef EVO_HAVE_BUILD_ID
    evo_bt("BUILD " EVO_BUILD_ID);   /* first thing on screen — catches a stale mount */
#endif
    evo_bt("main() entry");

    auto& app = evo::Application::getInstance();
    if (!app.initialize(argc, argv)) {
        return 1;
    }
    return app.run();
}
