#include "preclean_restart.h"
#include <cassert>
int main() {
    const time_t clean = 1790776800;
    assert(!preCleanRestartDue(clean - 601, clean, 0, 120000));
    assert(preCleanRestartDue(clean - 600, clean, 0, 120000));
    assert(preCleanRestartDue(clean - 541, clean, 0, 120000));
    assert(!preCleanRestartDue(clean - 540, clean, 0, 120000));
    assert(!preCleanRestartDue(clean - 600, clean, clean, 120000));
    assert(!preCleanRestartDue(clean - 600, clean, 0, 119999));
    assert(preCleanRestartDue(86400, 87000, 0, 120000));
    assert(preCleanRestartDue(clean + 86400 - 600, clean + 86400, clean, 120000));
}
