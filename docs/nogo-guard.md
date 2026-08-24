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
the line. It:

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
bumper sensor bit or steer the native cleaner; waiting for them only delayed
the proven reverse/turn maneuver and could produce a false success from a
stale heading estimate.

Every transition is logged at info level as `nogo_trigger`, `nogo_step`,
`nogo_escape_complete`, `nogo_escape_failed`, or `nogo_rearmed`. Enabling a
guard or starting an armed run automatically opens the normal one-hour info-log
window without changing the saved logging preference. `/api/nogo/status` also
exposes the current stage, last action/result, and trigger/escape/failed-step counts.

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

For the first run, draw a line across a clear open area with room for a 20 cm
reverse and in-place turn. After the run, inspect both `/api/nogo/status` and the
info log. A successful encounter must contain, in order: trigger, pause,
TestMode on, stop, reverse, turn, TestMode off, resume, and escape complete.

This is experimental motion-control firmware. Physical cliff sensors and
magnetic boundary strips remain the hard safety layer; do not use a software
line as the only barrier at stairs or another fall hazard.
