/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
/* SlopFin - real Jellyfin response shapes and conservative intro boundaries.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "intro_metadata.hpp"
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

namespace
{
int failures = 0;
void check(bool condition, const char *message)
{
    if (!condition)
    {
        std::printf("FAIL: %s\n", message);
        ++failures;
    }
}
slopfin::intro::Markers chapters(const char *body, double runtime = 1200.0)
{
    const auto root = slopfin::json::parse(body);
    check(root != nullptr, "chapter fixture parses");
    return slopfin::intro::chapters(root.get(), runtime);
}
slopfin::intro::Markers segments(const char *body, double runtime = 1200.0)
{
    const auto root = slopfin::json::parse(body);
    check(root != nullptr, "segment fixture parses");
    return root ? slopfin::intro::segments(*root, runtime) : slopfin::intro::Markers{};
}
} // namespace
int main()
{
    using namespace slopfin::intro;
    const double infinity = std::numeric_limits<double>::infinity();
    check(!bounds(0, infinity) && !bounds(NAN, 60) && !bounds(-1, 60) && !bounds(30, 30) &&
              !bounds(60, 30),
          "reject invalid bounds");
    check(bounds(0, 60) && bounds(30, 60, 60) && !bounds(30, 60.1, 60), "exact runtime limit");
    for (const char *name : {" Intro ", "OPENING  CREDITS", "Opening Theme", "Title Sequence", "OP",
                             "OP1", "OP 2", "Opening Theme: Song"})
        check(intro_name(normalized(name)), name);
    for (const char *name : {"Chapter 1", "Chapter 1 opening", "Cold Open", "Recap",
                             "opening scene", "introspection", "opinion", "Opening End",
                             "Intro End", "Intro: End", "Opening - End", "introductory scene"})
        check(!intro_name(normalized(name)), name);
    const auto cold_open = chapters(R"([
        {"Name":"Cold Open","StartPositionTicks":0},
        {"Name":" Opening Credits ","StartPositionTicks":545420000},
        {"Name":"Chapter 3","StartPositionTicks":1240000000},
        {"Name":"Ending","StartPositionTicks":11000000000}])");
    check(std::abs(cold_open.intro_start - 54.542) < 0.000001 && cold_open.intro_end == 124,
          "cold open retained; generic chapter bounds intro");
    check(cold_open.outro_start == 1100, "preserve credits marker");
    const auto duplicates = chapters(R"([
        {"Name":"Chapter 2","StartPositionTicks":900000000},
        {"Name":"OP2","StartPositionTicks":0},
        {"Name":"Chapter 1","StartPositionTicks":0}])");
    check(duplicates.intro_start == 0 && duplicates.intro_end == 90,
          "unsorted and duplicate boundaries");
    check(chapters(R"([{"Name":"Intro","StartPositionTicks":0}])").intro_start < 0,
          "missing end is not guessed");
    check(
        chapters(
            R"([{"Name":"Intro","StartPositionTicks":0},{"Name":"Scene","StartPositionTicks":3010000000}])")
                .intro_start < 0,
        "reject implausibly long chapter intro");
    check(
        chapters(
            R"([{"Name":"Intro","StartPositionTicks":0},{"Name":"Scene","StartPositionTicks":610000000}])",
            60.5)
                .intro_start < 0,
        "no rounded runtime bypass");
    check(
        chapters(
            R"([{"Name":"Intro","StartPositionTicks":0},{"Name":"Scene","StartPositionTicks":600000000}])",
            0)
                .intro_end == 60,
        "unknown runtime does not erase trustworthy chapter bounds");
    check(chapters(R"([{"Name":"Intro"},{"Name":"Scene","StartPositionTicks":900000000}])")
                  .intro_start < 0,
          "missing start rejected");
    check(chapters("[]").intro_start < 0 && chapters("null").intro_start < 0,
          "absent chapter metadata");
    const auto detected = segments(R"({"Items":[
        {"Type":"Recap","StartTicks":0,"EndTicks":200000000},
        {"Type":"Intro","StartTicks":300000000,"EndTicks":1200000000},
        {"Type":"Outro","StartTicks":11000000000,"EndTicks":12000000000}]})");
    check(detected.intro_start == 30 && detected.intro_end == 120 && detected.outro_start == 1100,
          "official Items wrapper and 100ns ticks");
    check(segments(R"([{"Type":"Intro","StartTicks":0,"EndTicks":900000000}])").intro_end == 90,
          "bare segment array");
    check(segments(R"({"Items":[{"Type":"Intro","StartTicks":0,"EndTicks":13000000000}]})")
                  .intro_start < 0,
          "server segment beyond runtime rejected");
    check(segments(R"({"Items":[{"Type":"Intro","StartTicks":0}]})").intro_start < 0,
          "missing server end rejected");
    check(segments(R"({"Items":[{"Type":"Intro","StartTicks":0,"EndTicks":1e309}]})").intro_start <
              0,
          "nonfinite server end rejected");
    check(segments(R"({"Items":[{"Type":"Intro","StartTicks":600000000,"EndTicks":500000000}]})")
                  .intro_start < 0,
          "reversed server bounds rejected");
    const auto earliest = segments(
        R"([{"Type":"Intro","StartTicks":100000000,"EndTicks":700000000},{"Type":"Intro","StartTicks":0,"EndTicks":500000000}])");
    check(earliest.intro_start == 0 && earliest.intro_end == 50,
          "earliest valid authoritative intro");
    const auto merged = with_fallback(detected, cold_open);
    check(merged.intro_start == 30 && merged.intro_end == 120,
          "detected markers override chapters");
    const auto fallback = with_fallback(segments("{}"), cold_open);
    check(fallback.intro_end == 124 && fallback.outro_start == 1100,
          "missing server markers retain chapter fallback");
    check(with_fallback({30, 120, -1}, cold_open).outro_start == 1100,
          "intro override preserves fallback credits");
    if (failures)
        return 1;
    std::puts("intro metadata: all checks passed");
}
