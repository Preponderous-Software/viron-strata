#include <cstdio>
#include <exception>
#include <string>

#include "check.h"

namespace check {

void fail(const char* file, int line, const std::string& what) {
    ++failures();
    std::fprintf(stderr, "  FAILED %s:%d: %s\n", file, line, what.c_str());
}

}  // namespace check

int main(int argc, char** argv) {
    const std::string tag = argc > 1 ? argv[1] : "";
    int run = 0;
    for (const auto& t : check::registry()) {
        if (!tag.empty() && t.tag != tag) continue;
        ++run;
        const int before = check::failures();
        try {
            t.fn();
        } catch (const check::Abort&) {
        } catch (const std::exception& e) {
            check::fail(__FILE__, __LINE__, std::string("exception: ") + e.what());
        }
        std::printf("%s %s %s\n", check::failures() == before ? "ok  " : "FAIL", t.tag.c_str(), t.name.c_str());
    }
    std::printf("%d tests, %d failures\n", run, check::failures());
    return run > 0 && check::failures() == 0 ? 0 : 1;
}
