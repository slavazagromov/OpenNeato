#ifndef NOGO_GUARD_H
#define NOGO_GUARD_H

#include <Arduino.h>
#include <vector>
#include "nogo_geometry.h"

class DataLogger;

// Passive first-stage no-go monitor. It observes the same dock-relative pose
// samples used by CleaningHistory and reports proximity/crossing events. It
// never sends motor or cleaning commands.
class NoGoGuard {
public:
    explicit NoGoGuard(DataLogger& logger);

    void begin();
    bool applyConfig(const String& json, String& error);
    const String& getConfigJson() const { return configJson; }
    String getStatusJson() const;

    void startRun();
    void endRun();
    void observePose(float x, float y);

private:
    DataLogger& dataLogger;
    std::vector<NoGoSegment> segments;
    String configJson;
    String referenceSession;
    bool enabled = false;
    bool runActive = false;
    bool hasPreviousPose = false;
    bool nearLatched = false;
    bool breachLatched = false;
    NoGoPoint previousPose;
    NoGoPoint lastPose;
    float warningDistanceM = 0.20f;
    float lastDistanceM = -1.0f;
    unsigned long lastEventMs = 0;
    unsigned int nearCount = 0;
    unsigned int breachCount = 0;

    bool saveConfig(const String& json, String& error) const;
    void resetRunState();
};

#endif // NOGO_GUARD_H
