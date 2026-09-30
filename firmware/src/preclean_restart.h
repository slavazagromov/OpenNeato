#ifndef PRECLEAN_RESTART_H
#define PRECLEAN_RESTART_H
#include <ctime>
inline bool preCleanRestartDue(time_t now, time_t cleanAt, time_t preparedAt, unsigned long uptimeMs) {
    return uptimeMs >= 120000UL && preparedAt != cleanAt && now >= cleanAt - 600 && now < cleanAt - 540;
}
#endif // PRECLEAN_RESTART_H
