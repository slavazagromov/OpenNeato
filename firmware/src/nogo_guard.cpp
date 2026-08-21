#include "nogo_guard.h"
#include <SPIFFS.h>
#include <utility>
#include "nogo_config_parser.h"
#include "config.h"
#include "data_logger.h"
#include "json_fields.h"

namespace {

    constexpr const char *NOGO_CONFIG_FILE = "/nogo.json";

} // namespace

NoGoGuard::NoGoGuard(DataLogger& logger) : dataLogger(logger) {}

void NoGoGuard::begin() {
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
    LOG("NOGO", "Loaded passive guard (%u segments, enabled=%s)", static_cast<unsigned int>(segments.size()),
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
    dataLogger.logGenericEvent("nogo_config", {{"enabled", enabled ? "true" : "false", FIELD_BOOL},
                                               {"segments", String(segments.size()), FIELD_INT}});
    return true;
}

void NoGoGuard::resetRunState() {
    hasPreviousPose = false;
    nearLatched = false;
    breachLatched = false;
    lastDistanceM = -1.0f;
}

void NoGoGuard::startRun() {
    runActive = true;
    resetRunState();
    if (enabled)
        dataLogger.logGenericEvent("nogo_armed", {{"segments", String(segments.size()), FIELD_INT}});
}

void NoGoGuard::endRun() {
    if (runActive && enabled) {
        dataLogger.logGenericEvent("nogo_disarmed", {{"near", nearLatched ? "true" : "false", FIELD_BOOL},
                                                     {"breached", breachLatched ? "true" : "false", FIELD_BOOL}});
    }
    runActive = false;
    hasPreviousPose = false;
}

void NoGoGuard::observePose(float x, float y) {
    lastPose = {x, y};
    if (!runActive || !enabled || segments.empty())
        return;

    NoGoPoint current{x, y};
    float closest = -1.0f;
    bool crossed = false;
    for (const auto& segment: segments) {
        float distance = noGoPointToSegmentDistance(current, segment);
        if (closest < 0.0f || distance < closest)
            closest = distance;
        if (hasPreviousPose && noGoSegmentsIntersect(previousPose, current, segment.a, segment.b))
            crossed = true;
    }
    lastDistanceM = closest;

    if (!nearLatched && closest >= 0.0f && closest <= warningDistanceM) {
        nearLatched = true;
        nearCount++;
        lastEventMs = millis();
        dataLogger.logGenericEvent("nogo_near", {{"x", String(x, 3), FIELD_FLOAT},
                                                 {"y", String(y, 3), FIELD_FLOAT},
                                                 {"distance", String(closest, 3), FIELD_FLOAT}});
    }
    if (!breachLatched && crossed) {
        breachLatched = true;
        breachCount++;
        lastEventMs = millis();
        dataLogger.logGenericEvent("nogo_crossed",
                                   {{"x", String(x, 3), FIELD_FLOAT}, {"y", String(y, 3), FIELD_FLOAT}});
    }

    previousPose = current;
    hasPreviousPose = true;
}

String NoGoGuard::getStatusJson() const {
    return fieldsToJson({{"mode", "observe", FIELD_STRING},
                         {"enabled", enabled ? "true" : "false", FIELD_BOOL},
                         {"runActive", runActive ? "true" : "false", FIELD_BOOL},
                         {"armed", enabled && runActive && !segments.empty() ? "true" : "false", FIELD_BOOL},
                         {"near", nearLatched ? "true" : "false", FIELD_BOOL},
                         {"breached", breachLatched ? "true" : "false", FIELD_BOOL},
                         {"nearCount", String(nearCount), FIELD_INT},
                         {"breachCount", String(breachCount), FIELD_INT},
                         {"segments", String(segments.size()), FIELD_INT},
                         {"warningDistance", String(warningDistanceM, 3), FIELD_FLOAT},
                         {"lastDistance", String(lastDistanceM, 3), FIELD_FLOAT},
                         {"lastX", String(lastPose.x, 3), FIELD_FLOAT},
                         {"lastY", String(lastPose.y, 3), FIELD_FLOAT},
                         {"lastEventMs", String(lastEventMs), FIELD_INT},
                         {"referenceSession", referenceSession, FIELD_STRING}});
}
