#include "nogo_guard.h"
#include <SPIFFS.h>
#include <cmath>
#include <utility>
#include "config.h"
#include "data_logger.h"
#include "neato_serial.h"
#include "nogo_config_parser.h"
#include "settings_manager.h"

namespace {

    constexpr const char *NOGO_CONFIG_FILE = "/nogo.json";
    constexpr unsigned long POSE_POLL_MS = 250;
    constexpr unsigned long REVERSE_MS = 2200;
    constexpr unsigned long TURN_MS = 1900;
    constexpr unsigned long COOLDOWN_MIN_MS = 3000;
    constexpr unsigned long COOLDOWN_MAX_MS = 30000;
    // Hold the PhotoMOS contacts long enough for the Neato control loop to
    // recognize them as a real bumper press while it is driving.  The first
    // moving-robot trial showed that 300 ms was electrically valid but too
    // short for the robot to begin its native avoidance maneuver.
    constexpr unsigned long PHYSICAL_BUMPER_PULSE_MS = 750;
    // Observation begins with the pulse and must outlive it by several pose
    // polls.  If native avoidance is still not visible, the guarded software
    // reverse/turn path remains the deterministic fallback.
    constexpr unsigned long PHYSICAL_BUMPER_OBSERVE_MS = 2000;
    constexpr unsigned long BUMPER_TEST_TIMEOUT_MS = 1500;
    constexpr float REARM_MARGIN_M = 0.15f;
    constexpr float LOOKAHEAD_DISTANCE_M = 0.45f;
    constexpr float PHYSICAL_AWAY_CONFIRM_M = 0.05f;
    constexpr int REVERSE_DISTANCE_MM = 200;
    constexpr int TURN_WHEEL_DISTANCE_MM = 170;
    constexpr int ESCAPE_SPEED_MM_S = 100;

#ifdef OPENNEATO_HAS_PHYSICAL_BUMPERS
    constexpr uint8_t LEFT_WHISKER_PIN = 25;
    constexpr uint8_t LEFT_FRONT_PIN = 26;
    constexpr uint8_t RIGHT_FRONT_PIN = 27;
    constexpr uint8_t RIGHT_WHISKER_PIN = 14;
#endif

    bool parsePose(const String& raw, float& x, float& y, float& theta) {
        int xPos = raw.indexOf("X=");
        int yPos = raw.indexOf("Y=");
        int thetaPos = raw.indexOf("Theta=");
        if (xPos < 0 || yPos < 0 || thetaPos < 0)
            return false;
        x = raw.substring(xPos + 2).toFloat();
        y = raw.substring(yPos + 2).toFloat();
        theta = raw.substring(thetaPos + 6).toFloat();
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(theta);
    }

    NoGoPoint closestPointOnSegment(const NoGoPoint& point, const NoGoSegment& segment) {
        float dx = segment.b.x - segment.a.x;
        float dy = segment.b.y - segment.a.y;
        float lengthSq = dx * dx + dy * dy;
        if (lengthSq <= 0.000001f)
            return segment.a;
        float t = ((point.x - segment.a.x) * dx + (point.y - segment.a.y) * dy) / lengthSq;
        t = constrain(t, 0.0f, 1.0f);
        return {segment.a.x + t * dx, segment.a.y + t * dy};
    }

} // namespace

NoGoGuard::NoGoGuard(NeatoSerial& serialRef, DataLogger& logger, SettingsManager& settings) :
    LoopTask(25), serial(serialRef), dataLogger(logger), settingsManager(settings) {
    TaskRegistry::add(this);
}

void NoGoGuard::begin() {
    // PhotoMOS inputs must be LOW before they become outputs. This keeps all
    // normally-open relay contacts released throughout boot and config errors.
    initializeBumperOutputs();
    configJson = R"({"enabled":false,"referenceSession":"","warningDistance":0.2,"noGoLines":[]})";
    if (!SPIFFS.exists(NOGO_CONFIG_FILE))
        return;

    File file = SPIFFS.open(NOGO_CONFIG_FILE, FILE_READ);
    if (!file)
        return;
    String stored = file.readString();
    file.close();

    NoGoParsedConfig parsed;
    String error;
    if (!parseNoGoConfig(stored, parsed, error)) {
        LOG("NOGO", "Ignoring invalid stored config: %s", error.c_str());
        return;
    }
    enabled = parsed.enabled;
    referenceSession = parsed.referenceSession;
    warningDistanceM = parsed.warningDistanceM;
    segments = std::move(parsed.segments);
    configJson = stored;
    LOG("NOGO", "Loaded active guard (%u segments, enabled=%s)", static_cast<unsigned int>(segments.size()),
        enabled ? "yes" : "no");
}

bool NoGoGuard::saveConfig(const String& json, String& error) const {
    File file = SPIFFS.open(NOGO_CONFIG_FILE, FILE_WRITE);
    if (!file) {
        error = "failed to open no-go config";
        return false;
    }
    size_t written = file.write(reinterpret_cast<const uint8_t *>(json.c_str()), json.length());
    file.close();
    if (written != json.length()) {
        error = "failed to write complete no-go config";
        return false;
    }
    return true;
}

bool NoGoGuard::applyConfig(const String& json, String& error) {
    NoGoParsedConfig parsed;
    if (!parseNoGoConfig(json, parsed, error))
        return false;
    if (!saveConfig(json, error))
        return false;

    enabled = parsed.enabled;
    referenceSession = parsed.referenceSession;
    warningDistanceM = parsed.warningDistanceM;
    segments = std::move(parsed.segments);
    configJson = json;
    resetRunState();
    if (enabled)
        settingsManager.enableTemporaryInfoLogging();
    dataLogger.logGenericEvent("nogo_config", {{"enabled", enabled ? "true" : "false", FIELD_BOOL},
                                               {"segments", String(segments.size()), FIELD_INT},
                                               {"mode",
                                                physicalBumpersAvailable() ? "physical_bumper_with_testmode_fallback"
                                                                           : "lookahead_testmode_escape",
                                                FIELD_STRING}});
    return true;
}

void NoGoGuard::resetRunState() {
    posePending = false;
    hasPreviousPose = false;
    nearLatched = false;
    breachLatched = false;
    testModeEntered = false;
    releaseBumperPulse();
    lastDistanceM = -1.0f;
    lastProjectedDistanceM = -1.0f;
    emulationStartDistanceM = -1.0f;
    activeSegment = -1;
    stage = Stage::IDLE;
    lastAction = "idle";
    lastResult = "none";
    stageDeadlineMs = 0;
    cooldownStartedMs = 0;
    emulationStartedMs = 0;
    testModeOffAttempts = 0;
    resumeAttempts = 0;
}

void NoGoGuard::startRun() {
    if (runActive)
        return;
    runActive = true;
    resetRunState();
    lastPosePollMs = 0;
    if (enabled && !segments.empty()) {
        settingsManager.enableTemporaryInfoLogging();
        dataLogger.logGenericEvent("nogo_armed", {{"segments", String(segments.size()), FIELD_INT},
                                                  {"pollMs", String(POSE_POLL_MS), FIELD_INT},
                                                  {"lookahead", String(LOOKAHEAD_DISTANCE_M, 3), FIELD_FLOAT},
                                                  {"warningDistance", String(warningDistanceM, 3), FIELD_FLOAT}});
    }
}

void NoGoGuard::endRun() {
    if (!runActive)
        return;
    if (testModeEntered) {
        serial.setMotorWheels(0, 0, 0, nullptr);
        serial.testMode(false, nullptr);
    }
    if (enabled) {
        dataLogger.logGenericEvent("nogo_disarmed", {{"triggers", String(nearCount), FIELD_INT},
                                                     {"breaches", String(breachCount), FIELD_INT},
                                                     {"escapes", String(escapeCount), FIELD_INT},
                                                     {"failures", String(failureCount), FIELD_INT}});
    }
    runActive = false;
    releaseBumperPulse();
    posePending = false;
    hasPreviousPose = false;
    stage = Stage::IDLE;
}

bool NoGoGuard::isManeuverActive() const {
    return stage != Stage::IDLE && stage != Stage::COOLDOWN;
}

void NoGoGuard::tick() {
    unsigned long now = millis();
    if (activeBumperMask != 0 && static_cast<long>(now - bumperPulseDeadlineMs) >= 0)
        releaseBumperPulse();

    if (!runActive || !enabled || segments.empty())
        return;

    if (stage == Stage::REVERSE_WAIT && static_cast<long>(now - stageDeadlineMs) >= 0) {
        sendTurn();
        return;
    }
    if (stage == Stage::TURN_WAIT && static_cast<long>(now - stageDeadlineMs) >= 0) {
        sendTestModeOff(false);
        return;
    }
    if (stage == Stage::PHYSICAL_BUMPER_OBSERVE && static_cast<long>(now - stageDeadlineMs) >= 0) {
        beginTestModeFallback("physical_no_turn");
        return;
    }
    if ((stage == Stage::IDLE || stage == Stage::PHYSICAL_BUMPER_OBSERVE || stage == Stage::COOLDOWN) && !posePending &&
        (lastPosePollMs == 0 || now - lastPosePollMs >= POSE_POLL_MS)) {
        pollPose();
    }
}

void NoGoGuard::pollPose() {
    posePending = true;
    lastPosePollMs = millis();
    serial.getRobotPosFresh(true, [this](bool ok, const RobotPosData& pose) {
        posePending = false;
        if (!ok) {
            logStep("pose", false);
            return;
        }
        float x, y, theta;
        if (!parsePose(pose.raw, x, y, theta)) {
            logStep("pose_parse", false);
            return;
        }
        observePose(x, y, theta);
    });
}

void NoGoGuard::observePose(float x, float y, float theta) {
    NoGoPoint current{x, y};
    lastPose = current;
    NoGoPoint heading{cosf(theta * DEG_TO_RAD), sinf(theta * DEG_TO_RAD)};
    if (hasPreviousPose) {
        float dx = current.x - previousPose.x;
        float dy = current.y - previousPose.y;
        float length = sqrtf(dx * dx + dy * dy);
        if (length > 0.01f)
            heading = {dx / length, dy / length};
    }
    NoGoPoint projected{current.x + heading.x * LOOKAHEAD_DISTANCE_M, current.y + heading.y * LOOKAHEAD_DISTANCE_M};
    float closest = -1.0f;
    float predictedClosest = -1.0f;
    bool crossed = false;
    int nearest = -1;
    int crossedSegment = -1;
    int predictedSegment = -1;
    for (size_t i = 0; i < segments.size(); i++) {
        float distance = noGoPointToSegmentDistance(current, segments[i]);
        if (closest < 0.0f || distance < closest) {
            closest = distance;
            nearest = static_cast<int>(i);
        }
        if (hasPreviousPose && noGoSegmentsIntersect(previousPose, current, segments[i].a, segments[i].b)) {
            crossed = true;
            crossedSegment = static_cast<int>(i);
        }
        if (noGoProjectedApproach(current, projected, segments[i], warningDistanceM)) {
            float projectedDistance = noGoPointToSegmentDistance(projected, segments[i]);
            if (predictedClosest < 0.0f || projectedDistance < predictedClosest) {
                predictedClosest = projectedDistance;
                predictedSegment = static_cast<int>(i);
            }
        }
    }
    lastDistanceM = closest;
    lastProjectedDistanceM = predictedClosest;
    if (stage == Stage::PHYSICAL_BUMPER_OBSERVE) {
        observePhysicalBumperResponse(current, projected, crossed);
    } else if (stage == Stage::COOLDOWN) {
        unsigned long elapsed = millis() - cooldownStartedMs;
        if ((elapsed >= COOLDOWN_MIN_MS && closest > warningDistanceM + REARM_MARGIN_M) || elapsed >= COOLDOWN_MAX_MS) {
            stage = Stage::IDLE;
            nearLatched = false;
            breachLatched = false;
            dataLogger.logGenericEvent("nogo_rearmed", {{"distance", String(closest, 3), FIELD_FLOAT}});
        }
    } else if (stage == Stage::IDLE && closest >= 0.0f) {
        int triggerSegment = -1;
        bool predicted = false;
        if (crossed) {
            triggerSegment = crossedSegment;
        } else if (closest <= warningDistanceM) {
            triggerSegment = nearest;
        } else if (predictedSegment >= 0) {
            triggerSegment = predictedSegment;
            predicted = true;
        }
        if (triggerSegment >= 0) {
            float triggerDistance = noGoPointToSegmentDistance(current, segments[triggerSegment]);
            float projectedDistance = noGoPointToSegmentDistance(projected, segments[triggerSegment]);
            triggerEscape(triggerDistance, crossed, predicted, projectedDistance, triggerSegment);
        }
    }

    previousPose = current;
    previousTheta = theta;
    hasPreviousPose = true;
}

void NoGoGuard::triggerEscape(float distance, bool crossed, bool predicted, float projectedDistance, int segmentIndex) {
    nearLatched = true;
    nearCount++;
    if (crossed) {
        breachLatched = true;
        breachCount++;
    }
    lastEventMs = millis();

    activeSegment = segmentIndex;
    NoGoPoint heading{cosf(previousTheta * DEG_TO_RAD), sinf(previousTheta * DEG_TO_RAD)};
    if (hasPreviousPose) {
        float dx = lastPose.x - previousPose.x;
        float dy = lastPose.y - previousPose.y;
        float length = sqrtf(dx * dx + dy * dy);
        if (length > 0.01f)
            heading = {dx / length, dy / length};
    }
    NoGoPoint nearestPoint = closestPointOnSegment(lastPose, segments[activeSegment]);
    NoGoPoint away{lastPose.x - nearestPoint.x, lastPose.y - nearestPoint.y};
    float awayLength = sqrtf(away.x * away.x + away.y * away.y);
    if (awayLength > 0.001f)
        escapeAway = {away.x / awayLength, away.y / awayLength};
    else
        escapeAway = {-heading.x, -heading.y};
    turnDirection = heading.x * away.y - heading.y * away.x >= 0.0f ? 1 : -1;

    dataLogger.logGenericEvent("nogo_trigger", {{"x", String(lastPose.x, 3), FIELD_FLOAT},
                                                {"y", String(lastPose.y, 3), FIELD_FLOAT},
                                                {"distance", String(distance, 3), FIELD_FLOAT},
                                                {"projectedDistance", String(projectedDistance, 3), FIELD_FLOAT},
                                                {"segment", String(activeSegment), FIELD_INT},
                                                {"crossed", crossed ? "true" : "false", FIELD_BOOL},
                                                {"predicted", predicted ? "true" : "false", FIELD_BOOL},
                                                {"turn", turnDirection > 0 ? "left" : "right", FIELD_STRING}});
    emulationStartPose = lastPose;
    emulationStartHeading = heading;
    emulationStartDistanceM = distance;
    if (physicalBumpersAvailable())
        startPhysicalBumperEscape();
    else
        sendPause();
}

void NoGoGuard::initializeBumperOutputs() {
#ifdef OPENNEATO_HAS_PHYSICAL_BUMPERS
    digitalWrite(LEFT_WHISKER_PIN, LOW);
    pinMode(LEFT_WHISKER_PIN, OUTPUT);
    digitalWrite(LEFT_FRONT_PIN, LOW);
    pinMode(LEFT_FRONT_PIN, OUTPUT);
    digitalWrite(RIGHT_FRONT_PIN, LOW);
    pinMode(RIGHT_FRONT_PIN, OUTPUT);
    digitalWrite(RIGHT_WHISKER_PIN, LOW);
    pinMode(RIGHT_WHISKER_PIN, OUTPUT);
    LOG("NOGO", "Physical bumper relays ready (GPIO 25/26/27/14, boot LOW)");
#endif
    activeBumperMask = 0;
    bumperPulseDeadlineMs = 0;
}

bool NoGoGuard::physicalBumpersAvailable() const {
#ifdef OPENNEATO_HAS_PHYSICAL_BUMPERS
    return true;
#else
    return false;
#endif
}

bool NoGoGuard::parseBumperChannel(const String& channel, uint8_t& mask) const {
    if (channel == "left_whisker")
        mask = BUMPER_LEFT_WHISKER;
    else if (channel == "left_front")
        mask = BUMPER_LEFT_FRONT;
    else if (channel == "right_front")
        mask = BUMPER_RIGHT_FRONT;
    else if (channel == "right_whisker")
        mask = BUMPER_RIGHT_WHISKER;
    else if (channel == "left_side")
        mask = BUMPER_LEFT_WHISKER | BUMPER_LEFT_FRONT;
    else if (channel == "right_side")
        mask = BUMPER_RIGHT_FRONT | BUMPER_RIGHT_WHISKER;
    else
        return false;
    return true;
}

void NoGoGuard::setBumperMask(uint8_t mask, bool active) {
#ifdef OPENNEATO_HAS_PHYSICAL_BUMPERS
    digitalWrite(LEFT_WHISKER_PIN, active && (mask & BUMPER_LEFT_WHISKER) ? HIGH : LOW);
    digitalWrite(LEFT_FRONT_PIN, active && (mask & BUMPER_LEFT_FRONT) ? HIGH : LOW);
    digitalWrite(RIGHT_FRONT_PIN, active && (mask & BUMPER_RIGHT_FRONT) ? HIGH : LOW);
    digitalWrite(RIGHT_WHISKER_PIN, active && (mask & BUMPER_RIGHT_WHISKER) ? HIGH : LOW);
#else
    (void) mask;
    (void) active;
#endif
    activeBumperMask = active ? mask : 0;
}

void NoGoGuard::startBumperPulse(uint8_t mask, unsigned long durationMs) {
    setBumperMask(mask, true);
    bumperPulseDeadlineMs = millis() + durationMs;
}

void NoGoGuard::releaseBumperPulse() {
    if (activeBumperMask == 0)
        return;
    uint8_t releasedMask = activeBumperMask;
    setBumperMask(releasedMask, false);
    bumperPulseDeadlineMs = 0;
    dataLogger.logGenericEvent("nogo_bumper_release", {{"mask", String(releasedMask), FIELD_INT}});
}

bool NoGoGuard::startBumperTest(const String& channel, uint8_t& requestedMask, String& error) {
    if (!physicalBumpersAvailable()) {
        error = "physical bumper outputs are not available on this board";
        return false;
    }
    if (runActive || isManeuverActive()) {
        error = "bumper test is blocked while a cleaning run is active";
        return false;
    }
    if (activeBumperMask != 0) {
        error = "another bumper pulse is active";
        return false;
    }
    if (!parseBumperChannel(channel, requestedMask)) {
        error = "unknown channel; use left_whisker, left_front, right_front, or right_whisker";
        return false;
    }

    bumperTestCount++;
    startBumperPulse(requestedMask, BUMPER_TEST_TIMEOUT_MS);
    dataLogger.logGenericEvent("nogo_bumper_test", {{"channel", channel, FIELD_STRING},
                                                    {"mask", String(requestedMask), FIELD_INT},
                                                    {"count", String(bumperTestCount), FIELD_INT}});
    return true;
}

void NoGoGuard::finishBumperTest(uint8_t requestedMask, uint8_t detectedMask, bool sensorReadOk) {
    releaseBumperPulse();
    dataLogger.logGenericEvent(
            "nogo_bumper_test_result",
            {{"requestedMask", String(requestedMask), FIELD_INT},
             {"detectedMask", String(detectedMask), FIELD_INT},
             {"sensorRead", sensorReadOk ? "true" : "false", FIELD_BOOL},
             {"matched", sensorReadOk && (detectedMask & requestedMask) == requestedMask ? "true" : "false",
              FIELD_BOOL}});
}

void NoGoGuard::startPhysicalBumperEscape() {
    stage = Stage::PHYSICAL_BUMPER_OBSERVE;
    physicalAttemptCount++;
    // A right-side contact makes the native cleaner turn left, and vice versa.
    uint8_t mask =
            turnDirection > 0 ? BUMPER_RIGHT_FRONT | BUMPER_RIGHT_WHISKER : BUMPER_LEFT_WHISKER | BUMPER_LEFT_FRONT;
    lastAction = turnDirection > 0 ? "physical_right_side" : "physical_left_side";
    lastResult = "pending";
    startBumperPulse(mask, PHYSICAL_BUMPER_PULSE_MS);
    emulationStartedMs = millis();
    stageDeadlineMs = emulationStartedMs + PHYSICAL_BUMPER_OBSERVE_MS;
    lastPosePollMs = 0;
    dataLogger.logGenericEvent("nogo_physical_bumper", {{"side", turnDirection > 0 ? "right" : "left", FIELD_STRING},
                                                        {"mask", String(mask), FIELD_INT},
                                                        {"attempt", String(physicalAttemptCount), FIELD_INT}});
}

void NoGoGuard::observePhysicalBumperResponse(const NoGoPoint& current, const NoGoPoint& projected, bool crossed) {
    if (activeSegment < 0 || activeSegment >= static_cast<int>(segments.size())) {
        beginTestModeFallback("physical_segment");
        return;
    }
    float distance = noGoPointToSegmentDistance(current, segments[activeSegment]);
    float dx = current.x - emulationStartPose.x;
    float dy = current.y - emulationStartPose.y;
    float moved = sqrtf(dx * dx + dy * dy);
    float awayTravel = dx * escapeAway.x + dy * escapeAway.y;
    NoGoPoint currentHeading{(projected.x - current.x) / LOOKAHEAD_DISTANCE_M,
                             (projected.y - current.y) / LOOKAHEAD_DISTANCE_M};
    float headingDot = constrain(
            emulationStartHeading.x * currentHeading.x + emulationStartHeading.y * currentHeading.y, -1.0f, 1.0f);
    float headingDeltaDegrees = acosf(headingDot) * RAD_TO_DEG;
    bool stillApproaching = noGoProjectedApproach(current, projected, segments[activeSegment], warningDistanceM);

    if (crossed || distance <= 0.03f) {
        beginTestModeFallback(crossed ? "physical_crossed" : "physical_too_close");
        return;
    }
    if (awayTravel >= PHYSICAL_AWAY_CONFIRM_M || (!stillApproaching && distance >= emulationStartDistanceM - 0.02f &&
                                                  (moved >= PHYSICAL_AWAY_CONFIRM_M || headingDeltaDegrees >= 15.0f))) {
        completePhysicalBumperEscape(distance, awayTravel, headingDeltaDegrees);
    }
}

void NoGoGuard::completePhysicalBumperEscape(float distance, float awayTravel, float headingDeltaDegrees) {
    releaseBumperPulse();
    physicalSuccessCount++;
    escapeCount++;
    lastAction = "physical_bumper_escape_complete";
    lastResult = "ok";
    lastEventMs = millis();
    dataLogger.logGenericEvent("nogo_physical_bumper_escape",
                               {{"distance", String(distance, 3), FIELD_FLOAT},
                                {"awayTravel", String(awayTravel, 3), FIELD_FLOAT},
                                {"headingDelta", String(headingDeltaDegrees, 1), FIELD_FLOAT},
                                {"elapsedMs", String(lastEventMs - emulationStartedMs), FIELD_INT},
                                {"count", String(physicalSuccessCount), FIELD_INT}});
    stage = Stage::COOLDOWN;
    cooldownStartedMs = millis();
    lastPosePollMs = 0;
}

void NoGoGuard::beginTestModeFallback(const char *reason) {
    if (stage != Stage::PHYSICAL_BUMPER_OBSERVE)
        return;
    releaseBumperPulse();
    fallbackCount++;
    dataLogger.logGenericEvent("nogo_physical_bumper_fallback",
                               {{"reason", reason, FIELD_STRING}, {"count", String(fallbackCount), FIELD_INT}});
    sendPause();
}

void NoGoGuard::sendPause() {
    stage = Stage::PAUSE_PENDING;
    lastAction = "pause";
    serial.clean("pause", [this](bool ok) {
        logStep("pause", ok);
        if (!ok) {
            failStage("pause");
            return;
        }
        sendTestModeOn();
    });
}

void NoGoGuard::sendTestModeOn() {
    stage = Stage::TESTMODE_ON_PENDING;
    lastAction = "testmode_on";
    serial.testMode(true, [this](bool ok) {
        logStep("testmode_on", ok);
        if (!ok) {
            failStage("testmode_on");
            return;
        }
        testModeEntered = true;
        // TestMode takes ownership away from the native cleaner and its
        // cleaning motors may stop. Restore them before moving so a virtual
        // barrier feels like a bumper hit instead of a complete clean stop.
        sendVacuumOn();
    });
}

void NoGoGuard::sendVacuumOn() {
    stage = Stage::CLEANING_MOTORS_PENDING;
    lastAction = "vacuum_on";
    serial.setMotorVacuum(true, settingsManager.get().vacuumSpeed, [this](bool ok) {
        logStep("vacuum_on", ok);
        // Motor restoration is best-effort. Boundary avoidance remains the
        // safety priority even if an optional cleaning motor rejects a command.
        sendBrushOn();
    });
}

void NoGoGuard::sendBrushOn() {
    stage = Stage::CLEANING_MOTORS_PENDING;
    lastAction = "brush_on";
    serial.setMotorBrush(settingsManager.get().brushRpm, [this](bool ok) {
        logStep("brush_on", ok);
        sendSideBrushOn();
    });
}

void NoGoGuard::sendSideBrushOn() {
    stage = Stage::CLEANING_MOTORS_PENDING;
    lastAction = "side_brush_on";
    serial.setMotorSideBrush(true, settingsManager.get().sideBrushPower, [this](bool ok) {
        logStep("side_brush_on", ok);
        sendStop();
    });
}

void NoGoGuard::sendStop() {
    stage = Stage::STOP_PENDING;
    lastAction = "stop";
    serial.setMotorWheels(0, 0, 0, [this](bool ok) {
        logStep("stop", ok);
        if (!ok) {
            failStage("stop");
            return;
        }
        sendReverse();
    });
}

void NoGoGuard::sendReverse() {
    stage = Stage::REVERSE_PENDING;
    lastAction = "reverse";
    serial.setMotorWheels(-REVERSE_DISTANCE_MM, -REVERSE_DISTANCE_MM, ESCAPE_SPEED_MM_S, [this](bool ok) {
        logStep("reverse", ok, {{"mm", String(REVERSE_DISTANCE_MM), FIELD_INT}});
        if (!ok) {
            failStage("reverse");
            return;
        }
        stage = Stage::REVERSE_WAIT;
        stageDeadlineMs = millis() + REVERSE_MS;
    });
}

void NoGoGuard::sendTurn() {
    stage = Stage::TURN_PENDING;
    lastAction = "turn";
    int left = turnDirection > 0 ? -TURN_WHEEL_DISTANCE_MM : TURN_WHEEL_DISTANCE_MM;
    int right = -left;
    serial.setMotorWheels(left, right, ESCAPE_SPEED_MM_S, [this](bool ok) {
        logStep("turn", ok,
                {{"direction", turnDirection > 0 ? "left" : "right", FIELD_STRING},
                 {"wheelMm", String(TURN_WHEEL_DISTANCE_MM), FIELD_INT}});
        if (!ok) {
            failStage("turn");
            return;
        }
        stage = Stage::TURN_WAIT;
        stageDeadlineMs = millis() + TURN_MS;
    });
}

void NoGoGuard::sendTestModeOff(bool aborting) {
    stage = aborting ? Stage::ABORT_TESTMODE_OFF_PENDING : Stage::TESTMODE_OFF_PENDING;
    lastAction = "testmode_off";
    testModeOffAttempts++;
    serial.testMode(false, [this, aborting](bool ok) {
        logStep("testmode_off", ok,
                {{"aborting", aborting ? "true" : "false", FIELD_BOOL},
                 {"attempt", String(testModeOffAttempts), FIELD_INT}});
        if (!ok && testModeOffAttempts < 2) {
            sendTestModeOff(aborting);
            return;
        }
        if (!ok) {
            failureCount++;
            lastAction = "testmode_off";
            lastResult = "fail";
            stage = Stage::COOLDOWN;
            cooldownStartedMs = millis();
            return;
        }
        testModeEntered = false;
        sendResume(aborting);
    });
}

void NoGoGuard::sendResume(bool aborting) {
    stage = aborting ? Stage::ABORT_RESUME_PENDING : Stage::RESUME_PENDING;
    lastAction = "resume";
    resumeAttempts++;
    serial.clean("resume", [this, aborting](bool ok) {
        logStep("resume", ok,
                {{"aborting", aborting ? "true" : "false", FIELD_BOOL},
                 {"attempt", String(resumeAttempts), FIELD_INT}});
        if (!ok && resumeAttempts < 2) {
            sendResume(aborting);
            return;
        }
        if (ok && !aborting) {
            escapeCount++;
            lastAction = "escape_complete";
            lastResult = "ok";
            dataLogger.logGenericEvent("nogo_escape_complete", {{"count", String(escapeCount), FIELD_INT}});
        } else if (!ok) {
            failureCount++;
        }
        stage = Stage::COOLDOWN;
        cooldownStartedMs = millis();
        lastPosePollMs = 0;
    });
}

void NoGoGuard::failStage(const char *action) {
    failureCount++;
    lastAction = action;
    lastResult = "fail";
    dataLogger.logGenericEvent("nogo_escape_failed", {{"action", action, FIELD_STRING},
                                                      {"testMode", testModeEntered ? "true" : "false", FIELD_BOOL}});
    if (testModeEntered) {
        serial.setMotorWheels(0, 0, 0, [this](bool) { sendTestModeOff(true); });
    } else {
        sendResume(true);
    }
}

void NoGoGuard::logStep(const char *action, bool ok, const std::vector<Field>& extra) {
    lastAction = action;
    lastResult = ok ? "ok" : "fail";
    lastEventMs = millis();
    std::vector<Field> fields = {{"action", action, FIELD_STRING}, {"status", ok ? "ok" : "fail", FIELD_STRING}};
    fields.insert(fields.end(), extra.begin(), extra.end());
    dataLogger.logGenericEvent("nogo_step", fields);
}

const char *NoGoGuard::stageName() const {
    switch (stage) {
        case Stage::IDLE:
            return "idle";
        case Stage::PHYSICAL_BUMPER_OBSERVE:
            return "physical_bumper_observe";
        case Stage::PAUSE_PENDING:
            return "pause";
        case Stage::TESTMODE_ON_PENDING:
            return "testmode_on";
        case Stage::CLEANING_MOTORS_PENDING:
            return "cleaning_motors";
        case Stage::STOP_PENDING:
            return "stop";
        case Stage::REVERSE_PENDING:
            return "reverse";
        case Stage::REVERSE_WAIT:
            return "reverse_wait";
        case Stage::TURN_PENDING:
            return "turn";
        case Stage::TURN_WAIT:
            return "turn_wait";
        case Stage::TESTMODE_OFF_PENDING:
            return "testmode_off";
        case Stage::RESUME_PENDING:
            return "resume";
        case Stage::COOLDOWN:
            return "cooldown";
        case Stage::ABORT_TESTMODE_OFF_PENDING:
            return "abort_testmode_off";
        case Stage::ABORT_RESUME_PENDING:
            return "abort_resume";
    }
    return "unknown";
}

String NoGoGuard::getStatusJson() const {
    const char *mode =
            physicalBumpersAvailable() ? "physical_bumper_with_testmode_fallback" : "lookahead_testmode_escape";
    return fieldsToJson({{"mode", mode, FIELD_STRING},
                         {"physicalBumpers", physicalBumpersAvailable() ? "true" : "false", FIELD_BOOL},
                         {"bumperPulseActive", activeBumperMask != 0 ? "true" : "false", FIELD_BOOL},
                         {"activeBumperMask", String(activeBumperMask), FIELD_INT},
                         {"enabled", enabled ? "true" : "false", FIELD_BOOL},
                         {"runActive", runActive ? "true" : "false", FIELD_BOOL},
                         {"armed", enabled && runActive && !segments.empty() ? "true" : "false", FIELD_BOOL},
                         {"near", nearLatched ? "true" : "false", FIELD_BOOL},
                         {"breached", breachLatched ? "true" : "false", FIELD_BOOL},
                         {"nearCount", String(nearCount), FIELD_INT},
                         {"breachCount", String(breachCount), FIELD_INT},
                         {"escapeCount", String(escapeCount), FIELD_INT},
                         {"failureCount", String(failureCount), FIELD_INT},
                         {"physicalAttemptCount", String(physicalAttemptCount), FIELD_INT},
                         {"physicalSuccessCount", String(physicalSuccessCount), FIELD_INT},
                         {"fallbackCount", String(fallbackCount), FIELD_INT},
                         {"bumperTestCount", String(bumperTestCount), FIELD_INT},
                         {"segments", String(segments.size()), FIELD_INT},
                         {"stage", stageName(), FIELD_STRING},
                         {"lastAction", lastAction, FIELD_STRING},
                         {"lastResult", lastResult, FIELD_STRING},
                         {"warningDistance", String(warningDistanceM, 3), FIELD_FLOAT},
                         {"lastDistance", String(lastDistanceM, 3), FIELD_FLOAT},
                         {"lastProjectedDistance", String(lastProjectedDistanceM, 3), FIELD_FLOAT},
                         {"lastX", String(lastPose.x, 3), FIELD_FLOAT},
                         {"lastY", String(lastPose.y, 3), FIELD_FLOAT},
                         {"lastEventMs", String(lastEventMs), FIELD_INT},
                         {"referenceSession", referenceSession, FIELD_STRING}});
}
