#ifndef NOGO_GUARD_H
#define NOGO_GUARD_H

#include <Arduino.h>
#include <vector>
#include "json_fields.h"
#include "loop_task.h"
#include "nogo_geometry.h"

class DataLogger;
class NeatoSerial;
class SettingsManager;

// Active no-go guard. During an autonomous cleaning run it samples the
// dock-relative robot pose independently of history recording. On approach to
// a configured segment it preserves the cleaning motors, performs a
// bumper-style reverse and turn in TestMode, exits TestMode, and explicitly
// resumes the same clean.
class NoGoGuard : public LoopTask {
public:
    NoGoGuard(NeatoSerial& serial, DataLogger& logger, SettingsManager& settings);

    void begin();
    bool applyConfig(const String& json, String& error);
    const String& getConfigJson() const { return configJson; }
    String getStatusJson() const;

    void startRun();
    void endRun();
    bool isManeuverActive() const;

private:
    enum class Stage : uint8_t {
        IDLE,
        PAUSE_PENDING,
        TESTMODE_ON_PENDING,
        CLEANING_MOTORS_PENDING,
        STOP_PENDING,
        REVERSE_PENDING,
        REVERSE_WAIT,
        TURN_PENDING,
        TURN_WAIT,
        TESTMODE_OFF_PENDING,
        RESUME_PENDING,
        COOLDOWN,
        ABORT_TESTMODE_OFF_PENDING,
        ABORT_RESUME_PENDING,
    };

    NeatoSerial& serial;
    DataLogger& dataLogger;
    SettingsManager& settingsManager;
    std::vector<NoGoSegment> segments;
    String configJson;
    String referenceSession;
    bool enabled = false;
    bool runActive = false;
    bool posePending = false;
    bool hasPreviousPose = false;
    bool nearLatched = false;
    bool breachLatched = false;
    bool testModeEntered = false;
    NoGoPoint previousPose;
    NoGoPoint lastPose;
    float previousTheta = 0.0f;
    float warningDistanceM = 0.20f;
    float lastDistanceM = -1.0f;
    int closestSegment = -1;
    int turnDirection = 1;
    Stage stage = Stage::IDLE;
    String lastAction = "idle";
    String lastResult = "none";
    unsigned long lastPosePollMs = 0;
    unsigned long stageDeadlineMs = 0;
    unsigned long cooldownStartedMs = 0;
    unsigned long lastEventMs = 0;
    unsigned int nearCount = 0;
    unsigned int breachCount = 0;
    unsigned int escapeCount = 0;
    unsigned int failureCount = 0;
    uint8_t testModeOffAttempts = 0;
    uint8_t resumeAttempts = 0;

    void tick() override;
    bool saveConfig(const String& json, String& error) const;
    void resetRunState();
    void pollPose();
    void observePose(float x, float y, float theta);
    void triggerEscape(float distance, bool crossed);
    void sendPause();
    void sendTestModeOn();
    void sendVacuumOn();
    void sendBrushOn();
    void sendSideBrushOn();
    void sendStop();
    void sendReverse();
    void sendTurn();
    void sendTestModeOff(bool aborting);
    void sendResume(bool aborting);
    void failStage(const char *action);
    void logStep(const char *action, bool ok, const std::vector<Field>& extra = {});
    const char *stageName() const;
};

#endif // NOGO_GUARD_H
