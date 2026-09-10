#include <cmath>
#include <cstdlib>
#include <iostream>
#include "nogo_config_parser.h"

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

bool parse(const char *json, NoGoParsedConfig& config, String& error) {
    return parseNoGoConfig(String(json), config, error);
}

} // namespace

int main() {
    NoGoParsedConfig config;
    String error;

    check(parse(R"({"enabled":false,"referenceSession":"","warningDistance":0.2,"noGoLines":[]})", config,
                error) &&
              !config.enabled && config.segments.empty(),
          "disabled empty config");

    check(parse(R"({"enabled":true,"referenceSession":"sample.jsonl.hs","warningDistance":0.25,"noGoLines":[{"points":[{"x":0,"y":0},{"x":1,"y":0},{"x":1,"y":2}]}]})",
                config, error) &&
              config.enabled && config.referenceSession == "sample.jsonl.hs" && closeTo(config.warningDistanceM, 0.25f) &&
              config.segments.size() == 2,
          "enabled polyline config");

    check(!parse(R"({"enabled":true,"noGoLines":[]})", config, error), "enabled requires a segment");
    check(!parse(R"({"enabled":false,"warningDistance":0.01,"noGoLines":[]})", config, error),
          "reject warning distance below range");
    check(!parse(R"({"enabled":false,"warningDistance":1.01,"noGoLines":[]})", config, error),
          "reject warning distance above range");
    check(!parse(R"({"enabled":true,"noGoLines":[{"points":[{"x":0,"y":0},{"x":1}]}]})", config,
                 error),
          "reject point missing coordinate");
    check(!parse(R"({"enabled":false,"noGoLines":[]}) trailing)", config, error), "reject trailing content");
    check(!parse(R"({"enabled":"true","noGoLines":[]})", config, error), "reject non-boolean enabled");

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
