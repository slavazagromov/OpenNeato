#include "nogo_config_parser.h"
#include <cctype>
#include <cmath>
#ifndef NOGO_CONFIG_MAX_BYTES
#include "config.h"
#endif

namespace {

    int skipWhitespace(const String& value, int pos) {
        int len = static_cast<int>(value.length());
        while (pos < len && isspace(static_cast<unsigned char>(value.charAt(pos))))
            pos++;
        return pos;
    }

    int findMatching(const String& value, int openPos) {
        if (openPos < 0 || openPos >= static_cast<int>(value.length()))
            return -1;
        char open = value.charAt(openPos);
        if (open != '[' && open != '{')
            return -1;
        char close = open == '[' ? ']' : '}';
        int depth = 0;
        bool inString = false;
        for (int i = openPos; i < static_cast<int>(value.length()); i++) {
            char c = value.charAt(i);
            if (inString) {
                if (c == '\\')
                    i++;
                else if (c == '"')
                    inString = false;
                continue;
            }
            if (c == '"') {
                inString = true;
            } else if (c == open) {
                depth++;
            } else if (c == close && --depth == 0) {
                return i;
            }
        }
        return -1;
    }

    int findFieldValue(const String& json, const char *key) {
        String pattern = String("\"") + key + "\"";
        int keyPos = json.indexOf(pattern);
        if (keyPos < 0)
            return -1;
        int colon = json.indexOf(':', keyPos + pattern.length());
        return colon < 0 ? -1 : skipWhitespace(json, colon + 1);
    }

    bool parseBoolField(const String& json, const char *key, bool defaultValue, bool& out) {
        int pos = findFieldValue(json, key);
        if (pos < 0) {
            out = defaultValue;
            return true;
        }
        if (json.substring(pos, pos + 4) == "true") {
            out = true;
            return true;
        }
        if (json.substring(pos, pos + 5) == "false") {
            out = false;
            return true;
        }
        return false;
    }

    bool parseFloatField(const String& json, const char *key, float defaultValue, float& out) {
        int pos = findFieldValue(json, key);
        if (pos < 0) {
            out = defaultValue;
            return true;
        }
        int end = pos;
        while (end < static_cast<int>(json.length())) {
            char c = json.charAt(end);
            if (!((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E'))
                break;
            end++;
        }
        if (end == pos)
            return false;
        out = json.substring(pos, end).toFloat();
        return std::isfinite(out);
    }

    bool parseStringField(const String& json, const char *key, String& out) {
        int pos = findFieldValue(json, key);
        if (pos < 0) {
            out = "";
            return true;
        }
        if (json.charAt(pos) != '"')
            return false;
        pos++;
        out = "";
        while (pos < static_cast<int>(json.length())) {
            char c = json.charAt(pos++);
            if (c == '"')
                return true;
            if (c == '\\' && pos < static_cast<int>(json.length()))
                c = json.charAt(pos++);
            out += c;
        }
        return false;
    }

    bool parsePoint(const String& object, NoGoPoint& point) {
        float x = 0.0f;
        float y = 0.0f;
        if (!parseFloatField(object, "x", 0.0f, x) || findFieldValue(object, "x") < 0)
            return false;
        if (!parseFloatField(object, "y", 0.0f, y) || findFieldValue(object, "y") < 0)
            return false;
        point = {x, y};
        return true;
    }

    bool parsePolyline(const String& object, std::vector<NoGoPoint>& points) {
        points.clear();
        int open = findFieldValue(object, "points");
        if (open < 0 || object.charAt(open) != '[')
            return false;
        int close = findMatching(object, open);
        if (close < 0)
            return false;

        int pos = open + 1;
        while (pos < close) {
            pos = skipWhitespace(object, pos);
            if (pos < close && object.charAt(pos) == ',') {
                pos++;
                continue;
            }
            if (pos >= close)
                break;
            if (object.charAt(pos) != '{')
                return false;
            int objectEnd = findMatching(object, pos);
            if (objectEnd < 0 || objectEnd > close)
                return false;
            NoGoPoint point;
            if (!parsePoint(object.substring(pos, objectEnd + 1), point))
                return false;
            points.push_back(point);
            if (points.size() > NOGO_MAX_POINTS_PER_LINE)
                return false;
            pos = objectEnd + 1;
        }
        return points.size() >= 2;
    }

    bool parseLines(const String& json, std::vector<NoGoSegment>& segments) {
        segments.clear();
        int open = findFieldValue(json, "noGoLines");
        if (open < 0 || json.charAt(open) != '[')
            return false;
        int close = findMatching(json, open);
        if (close < 0)
            return false;

        size_t lineCount = 0;
        int pos = open + 1;
        while (pos < close) {
            pos = skipWhitespace(json, pos);
            if (pos < close && json.charAt(pos) == ',') {
                pos++;
                continue;
            }
            if (pos >= close)
                break;
            if (json.charAt(pos) != '{')
                return false;
            int objectEnd = findMatching(json, pos);
            if (objectEnd < 0 || objectEnd > close)
                return false;
            std::vector<NoGoPoint> points;
            if (!parsePolyline(json.substring(pos, objectEnd + 1), points))
                return false;
            lineCount++;
            if (lineCount > NOGO_MAX_LINES)
                return false;
            for (size_t i = 1; i < points.size(); i++)
                segments.push_back({points[i - 1], points[i]});
            pos = objectEnd + 1;
        }
        return true;
    }

} // namespace

bool parseNoGoConfig(const String& json, NoGoParsedConfig& out, String& error) {
    error = "";
    out = NoGoParsedConfig();
    if (json.isEmpty() || json.length() > NOGO_CONFIG_MAX_BYTES) {
        error = "config is empty or too large";
        return false;
    }
    int first = skipWhitespace(json, 0);
    int rootClose = findMatching(json, first);
    if (first >= static_cast<int>(json.length()) || json.charAt(first) != '{' || rootClose < 0 ||
        skipWhitespace(json, rootClose + 1) != static_cast<int>(json.length())) {
        error = "config must be a JSON object";
        return false;
    }
    if (!parseBoolField(json, "enabled", false, out.enabled)) {
        error = "enabled must be true or false";
        return false;
    }
    if (!parseStringField(json, "referenceSession", out.referenceSession)) {
        error = "referenceSession must be a string";
        return false;
    }
    if (!parseFloatField(json, "warningDistance", 0.20f, out.warningDistanceM) || out.warningDistanceM < 0.05f ||
        out.warningDistanceM > 1.0f) {
        error = "warningDistance must be between 0.05 and 1.0 meters";
        return false;
    }
    if (!parseLines(json, out.segments)) {
        error = "noGoLines must contain valid point arrays";
        return false;
    }
    if (out.enabled && out.segments.empty()) {
        error = "enabled guard requires at least one no-go segment";
        return false;
    }
    return true;
}
