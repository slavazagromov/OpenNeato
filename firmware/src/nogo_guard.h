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
// dock-relative robot pose independently of history recording. On the original
// ESP32 hardware build it first closes PhotoMOS relays wired across the robot's
// bumper switches so Neato's native avoidance can keep cleaning and SLAM active.
// If pose does not confirm avoidance, it falls back to a direct TestMode escape.
class NoGoGuard : public LoopTask {
public:
    static constexpr uint8_t BUMPER_LEFT_WHISKER = 1 << 0;
    static constexpr uint8_t BUMPER_LEFT_FRONT = 1 << 1;
    static constexpr uint8_t BUMPER_RIGHT_FRONT = 1 << 2;
    static constexpr uint8_t BUMPER_RIGHT_WHISKER = 1 << 3;

    NoGoGuard(NeatoSerial& serial, DataLogger& logger, SettingsManager& settings);

    void begin();
    bool applyConfig(const String& json, String& error);
    const String& getConfigJson() const { return configJson; }
    String getStatusJson() const;

    void startRun();
    void endRun();
    bool isManeuverActive() const;
    bool physicalBumpersAvailable() const;
    bool startBumperTest(const String& channel, uint8_t& requestedMask, String& error);
    void finishBumperTest(uint8_t requestedMask, uint8_t detectedMask, bool sensorReadOk);

private:
    enum class Stage : uint8_t {
        IDLE,
        PHYSICAL_BUMPER_OBSERVE,
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
    NoGoPoint emulationStartPose;
    NoGoPoint emulationStartHeading;
    NoGoPoint escapeAway;
    float previousTheta = 0.0f;
    float warningDistanceM = 0.20f;
    float lastDistanceM = -1.0f;
    float lastProjectedDistanceM = -1.0f;
    float emulationStartDistanceM = -1.0f;
    int activeSegment = -1;
    int turnDirection = 1;
    Stage stage = Stage::IDLE;
    String lastAction = "idle";
    String lastResult = "none";
    unsigned long lastPosePollMs = 0;
    unsigned long stageDeadlineMs = 0;
    unsigned long cooldownStartedMs = 0;
    unsigned long emulationStartedMs = 0;
    unsigned long bumperPulseDeadlineMs = 0;
    unsigned long lastEventMs = 0;
    unsigned int nearCount = 0;
    unsigned int breachCount = 0;
    unsigned int escapeCount = 0;
    unsigned int failureCount = 0;
    unsigned int physicalAttemptCount = 0;
    unsigned int physicalSuccessCount = 0;
    unsigned int fallbackCount = 0;
    unsigned int bumperTestCount = 0;
    uint8_t activeBumperMask = 0;
    uint8_t testModeOffAttempts = 0;
    uint8_t resumeAttempts = 0;

    void tick() override;
    bool saveConfig(const String& json, String& error) const;
    void resetRunState();
    void pollPose();
    void observePose(float x, float y, float theta);
    void triggerEscape(float distance, bool crossed, bool predicted, float projectedDistance, int segmentIndex);
    void initializeBumperOutputs();
    bool parseBumperChannel(const String& channel, uint8_t& mask) const;
    void setBumperMask(uint8_t mask, bool active);
    void startBumperPulse(uint8_t mask, unsigned long durationMs);
    void releaseBumperPulse();
    void startPhysicalBumperEscape();
    void observePhysicalBumperResponse(const NoGoPoint& current, const NoGoPoint& projected, bool crossed);
    void completePhysicalBumperEscape(float distance, float awayTravel, float headingDeltaDegrees);
    void beginTestModeFallback(const char *reason);
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
