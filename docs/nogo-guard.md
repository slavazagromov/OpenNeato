# Passive No-Go Guard

This branch introduces the first hardware-safe milestone toward local no-go lines.
It observes the dock-relative pose already collected by cleaning history and reports
when a pose comes near or crosses configured geometry. It does not send wheel,
cleaning, pause, stop, or docking commands.

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

- `warningDistance` is in meters and must be between `0.05` and `1.0`.
- At most 16 polylines and 32 points per polyline are accepted.
- Configuration is written only when the user explicitly saves it.
- Pose observation and event latching do not write to flash.

Read the stored configuration with `GET /api/nogo/config` and runtime state with
`GET /api/nogo/status`. The status exposes `near`, `breached`, counters, last pose,
and distance to the closest configured line for Home Assistant polling.

## Validation stages

1. Upload with the guard disabled and confirm normal boot, web UI, UART, and cleaning.
2. Record several dock-started cleaning sessions and compare their coordinate frames.
3. Configure a harmless line in an open room and enable observation mode.
4. Confirm `near` and `breached` events match the real path.
5. Only after repeatable frame alignment should a later branch add pause/stop behavior.

The robot's cliff sensors and physical magnetic boundary strips remain the hard
safety layer. This experimental observer must not be treated as fall protection.
