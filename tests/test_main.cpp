#include <cstring>

#include "check.h"

// Runs every registered test, or only those whose name contains argv[1].
int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : "";
    int run = 0;
    for (const check::TestCase& test : check::registry()) {
        if (std::strstr(test.name, filter) == nullptr) continue;
        int before = check::failureCount();
        try {
            test.body();
        } catch (const std::exception& e) {
            check::reportFailure(test.name, 0, std::string("uncaught exception: ") + e.what());
        }
        if (check::failureCount() != before) std::cerr << "  in test " << test.name << "\n";
        ++run;
    }
    std::cout << run << " tests, " << check::failureCount() << " failures\n";
    return check::failureCount() == 0 ? 0 : 1;
}
