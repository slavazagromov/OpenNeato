#ifndef NOGO_GEOMETRY_H
#define NOGO_GEOMETRY_H

#include <cmath>

struct NoGoPoint {
    float x = 0.0f;
    float y = 0.0f;

    constexpr NoGoPoint() = default;
    constexpr NoGoPoint(float xValue, float yValue) : x(xValue), y(yValue) {}
};

struct NoGoSegment {
    NoGoPoint a;
    NoGoPoint b;

    constexpr NoGoSegment() = default;
    constexpr NoGoSegment(const NoGoPoint& start, const NoGoPoint& end) : a(start), b(end) {}
};

inline float noGoCross(const NoGoPoint& a, const NoGoPoint& b, const NoGoPoint& c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

inline bool noGoOnSegment(const NoGoPoint& a, const NoGoPoint& b, const NoGoPoint& p) {
    float minX = a.x < b.x ? a.x : b.x;
    float maxX = a.x > b.x ? a.x : b.x;
    float minY = a.y < b.y ? a.y : b.y;
    float maxY = a.y > b.y ? a.y : b.y;
    return minX <= p.x && p.x <= maxX && minY <= p.y && p.y <= maxY;
}

inline bool noGoSegmentsIntersect(const NoGoPoint& a1, const NoGoPoint& a2, const NoGoPoint& b1, const NoGoPoint& b2) {
    float d1 = noGoCross(b1, b2, a1);
    float d2 = noGoCross(b1, b2, a2);
    float d3 = noGoCross(a1, a2, b1);
    float d4 = noGoCross(a1, a2, b2);

    if (((d1 > 0.0f && d2 < 0.0f) || (d1 < 0.0f && d2 > 0.0f)) &&
        ((d3 > 0.0f && d4 < 0.0f) || (d3 < 0.0f && d4 > 0.0f))) {
        return true;
    }
    if (d1 == 0.0f && noGoOnSegment(b1, b2, a1))
        return true;
    if (d2 == 0.0f && noGoOnSegment(b1, b2, a2))
        return true;
    if (d3 == 0.0f && noGoOnSegment(a1, a2, b1))
        return true;
    if (d4 == 0.0f && noGoOnSegment(a1, a2, b2))
        return true;
    return false;
}

inline float noGoPointToSegmentDistance(const NoGoPoint& p, const NoGoSegment& segment) {
    float dx = segment.b.x - segment.a.x;
    float dy = segment.b.y - segment.a.y;
    float lengthSq = dx * dx + dy * dy;
    if (lengthSq == 0.0f) {
        float px = p.x - segment.a.x;
        float py = p.y - segment.a.y;
        return sqrtf(px * px + py * py);
    }

    float t = ((p.x - segment.a.x) * dx + (p.y - segment.a.y) * dy) / lengthSq;
    if (t < 0.0f)
        t = 0.0f;
    if (t > 1.0f)
        t = 1.0f;
    float px = p.x - (segment.a.x + t * dx);
    float py = p.y - (segment.a.y + t * dy);
    return sqrtf(px * px + py * py);
}

#endif // NOGO_GEOMETRY_H
