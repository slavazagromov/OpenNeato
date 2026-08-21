#include <cmath>
#include <cstdlib>
#include <iostream>
#include "nogo_geometry.h"

namespace {

int failures = 0;

void check(bool condition, const char *name) {
    if (condition) {
        std::cout << "PASS " << name << '\n';
    } else {
        std::cout << "FAIL " << name << '\n';
        failures++;
    }
}

bool closeTo(float actual, float expected) {
    return std::fabs(actual - expected) < 0.0001f;
}

} // namespace

int main() {
    NoGoPoint left{0.0f, 0.0f};
    NoGoPoint right{2.0f, 0.0f};

    check(noGoSegmentsIntersect({1.0f, -1.0f}, {1.0f, 1.0f}, left, right), "proper crossing");
    check(!noGoSegmentsIntersect({0.0f, 1.0f}, {2.0f, 1.0f}, left, right), "parallel clear");
    check(noGoSegmentsIntersect({-1.0f, 0.0f}, {0.0f, 0.0f}, left, right), "endpoint touch");
    check(noGoSegmentsIntersect({0.5f, 0.0f}, {1.5f, 0.0f}, left, right), "collinear overlap");

    NoGoSegment wall{left, right};
    check(closeTo(noGoPointToSegmentDistance({1.0f, 0.25f}, wall), 0.25f), "perpendicular distance");
    check(closeTo(noGoPointToSegmentDistance({3.0f, 0.0f}, wall), 1.0f), "endpoint distance");
    check(closeTo(noGoPointToSegmentDistance({1.0f, 0.0f}, wall), 0.0f), "point on wall");

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
