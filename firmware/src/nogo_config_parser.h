#ifndef NOGO_CONFIG_PARSER_H
#define NOGO_CONFIG_PARSER_H

#include <Arduino.h>
#include <vector>
#include "nogo_geometry.h"

struct NoGoParsedConfig {
    bool enabled = false;
    String referenceSession;
    float warningDistanceM = 0.20f;
    std::vector<NoGoSegment> segments;
};

bool parseNoGoConfig(const String& json, NoGoParsedConfig& out, String& error);

#endif // NOGO_CONFIG_PARSER_H
