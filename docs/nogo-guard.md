# Active No-Go Guard

This branch combines the Philou95 Home Assistant integration with robot-side
no-go enforcement. Lines drawn on a completed cleaning map are transformed into
the robot's dock-relative coordinate frame and stored in `/nogo.json`.

## Runtime behavior

When no lines are enabled, OpenNeato does not change the normal whole-house
cleaning or map-creation behavior.

When lines are enabled, the guard starts with each autonomous cleaning run and
polls fresh localization every 250 ms. Using a 45 cm projected look-ahead, it
starts the maneuver at the configured warning distance before the robot crosses
the line.

On the original ESP32 WROOM32 build, the guard first pulses the two PhotoMOS
relays on the side facing the no-go line for 300 ms. The relay contacts are wired
in parallel with the robot's mechanical bumper switches, so the native Neato
cleaner performs its own reverse and turn while cleaning motors and SLAM remain
active. A right-side pulse requests a left turn, and a left-side pulse requests
a right turn.

The guard watches the pose for 750 ms. If it cannot confirm that the robot moved
or turned away, it uses the existing fallback:

1. sends the authenticated cleaning pause event;
2. enters TestMode and immediately disables both wheel motors;
3. reverses 200 mm at 100 mm/s;
4. turns about 80 degrees toward the side away from the closest line;
5. exits TestMode and sends the authenticated resume event.

The guard then waits until the robot is outside the warning distance plus a
15 cm margin before re-arming. A 30-second ceiling prevents a permanently
disarmed guard if localization remains frozen.

The earlier `SetButton IRleft` / `IRright` experiment was removed. Robot logs
showed that those commands were acknowledged but did not change any physical
bumper sensor bit or steer the native cleaner. The PhotoMOS outputs create the
real electrical contacts those serial commands could not emulate.

Every transition is logged at info level as `nogo_trigger`, `nogo_step`,
`nogo_escape_complete`, `nogo_escape_failed`, or `nogo_rearmed`. Enabling a
guard or starting an armed run automatically opens the normal one-hour info-log
window without changing the saved logging preference. `/api/nogo/status` also
exposes the current stage, active output mask, last action/result, physical
attempt/success/fallback counts, and trigger/escape/failed-step counts.

## Physical bumper wiring

The four G3VM-61A1 PhotoMOS inputs use a 330 ohm series resistor. Pin 2 of every
PhotoMOS may share any ESP32 GND pin. The isolated output pins 3 and 4 go across
the two contacts of the matching original Neato switch; they have no polarity.

| Robot contact (robot driving forward) | ESP32 GPIO | Sensor bit |
| --- | ---: | --- |
| Left outer whisker | 25 | `lSideBit` |
| Left inner/front bumper | 26 | `lFrontBit` |
| Right inner/front bumper | 27 | `rFrontBit` |
| Right outer whisker | 14 | `rSideBit` |

All four outputs are explicitly written LOW before being configured as outputs
at boot. This leaves the normally-open PhotoMOS contacts released. The physical
bumper feature is compiled only for the original `esp32-*` environments; other
board builds retain the direct TestMode escape.

## Stationary wiring test

Charge the robot and leave it idle or docked. In OpenNeato, open **Settings →
Diagnostics → Physical bumper wiring** and test each channel separately. The
firmware pulses one relay, performs a fresh high-priority `GetDigitalSensors`
read, releases the relay, and reports whether the matching sensor bit appeared.
The endpoint is also available as:

```text
POST /api/nogo/bumper-test?channel=left_whisker
POST /api/nogo/bumper-test?channel=left_front
POST /api/nogo/bumper-test?channel=right_front
POST /api/nogo/bumper-test?channel=right_whisker
```

The test is rejected while an autonomous cleaning run is active. An independent
1.5-second timer releases the output even if the robot UART sensor read fails.

### Verified hardware result — September 10, 2026

The original ESP32 WROOM32 installation on a Botvac D5 passed all four stationary
tests independently:

| Channel | Requested mask | Detected mask | Matching bit | Result |
| --- | ---: | ---: | --- | --- |
| `left_whisker` / GPIO25 | 1 | 1 | `lSideBit` | PASS |
| `left_front` / GPIO26 | 2 | 2 | `lFrontBit` | PASS |
| `right_front` / GPIO27 | 4 | 4 | `rFrontBit` | PASS |
| `right_whisker` / GPIO14 | 8 | 8 | `rSideBit` | PASS |

Each endpoint returned HTTP 200, `ok: true`, and `detected: true`, with no
unrequested bumper bit asserted. The final sensor read showed all four bits false;
`/api/nogo/status` showed `activeBumperMask: 0` and
`bumperPulseActive: false`. This verifies GPIO, resistor, PhotoMOS, Neato switch
wiring, and robot sensor recognition for every channel.

The first moving square test also completed four fallback escape cycles with zero
failed steps. Its status showed `physicalAttemptCount: 5`,
`physicalSuccessCount: 0`, and `fallbackCount: 5`, so native cleaner avoidance
from the injected pulse remains a separate open validation item.

## Configuration

Store a configuration with `PUT /api/nogo/config`:

```json
{
  "enabled": true,
  "referenceSession": "1771683615.jsonl.hs",
  "warningDistance": 0.2,
  "noGoLines": [
    {
      "points": [
        { "x": 1.2, "y": -0.5 },
        { "x": 1.2, "y": 2.0 }
      ]
    }
  ]
}
```

- `warningDistance` is in metres and must be between `0.05` and `1.0`.
- At most 16 polylines and 32 points per polyline are accepted.
- Configuration is written only when the user explicitly saves it.
- Pose polling and maneuver state do not write to flash.

The Home Assistant replay card provides the map editor. During a no-go escape,
the vacuum entity remains `cleaning` so state-triggered automations do not emit
duplicate "started cleaning" notifications. The integration also
exposes armed/near/breached binary sensors plus stage, last action/result,
distance, trigger, breach, escape, and failed-step sensors.

## Test-run inspection

For the first moving run, wait until the battery is at least 20%, then draw a
line across a clear open area with room for a 20 cm reverse and in-place turn.
After the run, inspect both `/api/nogo/status` and the info log. A successful
native encounter contains `nogo_trigger`, `nogo_physical_bumper`, and
`nogo_physical_bumper_escape`. If native avoidance is not confirmed, the log
instead contains `nogo_physical_bumper_fallback`, followed by pause, TestMode on,
stop, reverse, turn, TestMode off, resume, and escape complete.

This is experimental motion-control firmware. Physical cliff sensors and
magnetic boundary strips remain the hard safety layer; do not use a software
line as the only barrier at stairs or another fall hazard.
