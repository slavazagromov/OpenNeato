"""WebSocket API backing replay, session management, and no-go editing.

The card renders parsed sessions on a canvas at display refresh rate, so all
CPU work (downloading, decompressing, building the coverage grid) stays on the
server and happens exactly once per session. No-go commands convert geometry
between the accumulated map and the selected session's raw robot frame.
"""

from __future__ import annotations

import logging
import math
from typing import Any

import voluptuous as vol

from homeassistant.components import websocket_api
from homeassistant.core import HomeAssistant, callback

from .const import (
    CONF_MAP_ROTATION_OFFSET,
    DOMAIN,
    HISTORY_CELL_SIZE_M,
    MAP_DEFAULT_ROTATION_OFFSET,
)
from .replay import build_replay_session

_LOGGER = logging.getLogger(__name__)

# Parsed sessions are a few hundred KB each; keeping a handful means flipping
# back and forth in the session picker is instant instead of re-downloading
# from the robot every time.
CACHE_KEY = "replay_cache"
# Last session list seen from the robot, so the card still has something
# to show when the bridge is unreachable. A replay is history: it does
# not need the robot awake.
LAST_SESSIONS_KEY = "replay_last_sessions"
_CACHE_MAX = 4


@callback
def async_register(hass: HomeAssistant) -> None:
    """Register the replay WebSocket commands (idempotent)."""
    websocket_api.async_register_command(hass, ws_list_sessions)
    websocket_api.async_register_command(hass, ws_get_session)
    websocket_api.async_register_command(hass, ws_delete_session)
    websocket_api.async_register_command(hass, ws_get_nogo)
    websocket_api.async_register_command(hass, ws_set_nogo)


def _resolve_entry(hass: HomeAssistant, entry_id: str | None) -> tuple[str, dict[str, Any]] | None:
    """Return (entry_id, entry_data) for the requested or only OpenNeato entry."""
    entries: dict[str, Any] = hass.data.get(DOMAIN, {})
    # hass.data[DOMAIN] also holds our own module-level scratch keys.
    candidates = {
        key: value
        for key, value in entries.items()
        if isinstance(value, dict) and "api" in value
    }
    if entry_id:
        data = candidates.get(entry_id)
        return (entry_id, data) if data else None
    if len(candidates) == 1:
        return next(iter(candidates.items()))
    return None


def _floorplan_payload(hass: HomeAssistant, entry_id: str) -> dict[str, Any] | None:
    """Expose the background so the card draws the same plan the cameras do.

    A LIDAR-built map wins over a hand-calibrated image: it is drawn in the
    robot's own frame, so its origin and scale are exact rather than fitted,
    and it carries a view rotation derived from the walls, which stands it
    upright however the robot's frame happens to be oriented.

    Either way the stored image is a server-side path the browser can't load,
    so the card fetches it through one of our HTTP views.
    """
    entry = hass.config_entries.async_get_entry(entry_id)
    if entry is None:
        return None

    stored = hass.data.get(DOMAIN, {}).get(entry_id)
    mapper = stored.get("mapper") if isinstance(stored, dict) else None
    if mapper is not None and mapper.sessions:
        cal = mapper.calibration()
        if cal:
            offset = float(
                entry.options.get(CONF_MAP_ROTATION_OFFSET, MAP_DEFAULT_ROTATION_OFFSET)
            )
            return {
                # Busts the browser cache whenever the drawn plan changes --
                # not just when a cleaning is added, since the wall threshold
                # now moves with the map's own distribution.
                "url": f"/api/openneato/map/{entry_id}?v={mapper.render_signature()}",
                "originX": cal["origin_x"],
                "originY": cal["origin_y"],
                "rotation": 0.0,
                "scale": cal["scale"],
                "viewRotation": mapper.view_rotation(offset),
                "generated": True,
                "sessions": mapper.sessions,
            }

    # No hand-calibrated fallback any more: the plan the robot builds for
    # itself is the only one. A static image had to be aligned by hand and
    # went stale the moment the furniture moved.
    return None


@websocket_api.websocket_command(
    {
        vol.Required("type"): "openneato/sessions",
        vol.Optional("entry_id"): str,
    }
)
@websocket_api.async_response
async def ws_list_sessions(
    hass: HomeAssistant,
    connection: websocket_api.ActiveConnection,
    msg: dict[str, Any],
) -> None:
    """List the robot's cleaning sessions, newest first."""
    resolved = _resolve_entry(hass, msg.get("entry_id"))
    if resolved is None:
        connection.send_error(msg["id"], "not_found", "No OpenNeato config entry found")
        return
    entry_id, data = resolved

    coordinator = data["coordinator"]
    history = (coordinator.data or {}).get("history")
    store = hass.data.setdefault(DOMAIN, {}).setdefault(LAST_SESSIONS_KEY, {})

    if isinstance(history, list):
        sessions = [
            {
                "name": item.get("name"),
                "size": item.get("size"),
                "recording": bool(item.get("recording")),
                "session": item.get("session"),
                "summary": item.get("summary"),
            }
            for item in history
            if isinstance(item, dict) and item.get("name")
        ]
        # The firmware returns files in directory order; sort by session start
        # so the picker reads chronologically regardless of filesystem layout.
        sessions.sort(key=lambda s: _session_start(s), reverse=True)
        store[entry_id] = sessions
    else:
        # The robot is unreachable. A replay is history, though -- the parsed
        # sessions are cached here and the floor plan lives in Home Assistant's
        # own storage -- so serve the last list we saw rather than blanking the
        # card. Nothing here needs the robot to be awake.
        sessions = store.get(entry_id)
        if not sessions:
            connection.send_error(
                msg["id"], "unavailable", "No cleaning history available"
            )
            return
        _LOGGER.debug("Replay: robot unreachable, serving %d cached sessions", len(sessions))
        # Whatever was recording is not any more, as far as we can tell.
        sessions = [{**s, "recording": False} for s in sessions]

    connection.send_result(
        msg["id"],
        {
            "entry_id": entry_id,
            "sessions": sessions,
            "floorplan": _floorplan_payload(hass, entry_id),
        },
    )


def _session_start(session: dict[str, Any]) -> float:
    """Session start epoch, falling back to the numeric filename prefix."""
    info = session.get("session")
    if isinstance(info, dict) and info.get("time"):
        return float(info["time"])
    try:
        return float(str(session.get("name", "")).split(".", 1)[0])
    except ValueError:
        return 0.0


def _validate_nogo_lines(value: Any) -> list[list[dict[str, float]]]:
    """Validate map-frame polylines received from the replay card."""
    if not isinstance(value, list) or len(value) > 16:
        raise ValueError("no-go lines must be a list of at most 16 lines")

    lines: list[list[dict[str, float]]] = []
    for raw_line in value:
        if not isinstance(raw_line, list) or not 2 <= len(raw_line) <= 32:
            raise ValueError("each no-go line must contain 2 to 32 points")
        line: list[dict[str, float]] = []
        for raw_point in raw_line:
            if not isinstance(raw_point, dict):
                raise ValueError("each no-go point must contain x and y")
            try:
                x = float(raw_point["x"])
                y = float(raw_point["y"])
            except (KeyError, TypeError, ValueError) as err:
                raise ValueError("each no-go point must contain numeric x and y") from err
            if not math.isfinite(x) or not math.isfinite(y):
                raise ValueError("no-go coordinates must be finite")
            line.append({"x": round(x, 3), "y": round(y, 3)})
        lines.append(line)
    return lines


def _unpack_nogo_lines(value: Any) -> list[list[dict[str, float]]]:
    """Extract firmware ``[{points: [...]}]`` geometry for map conversion."""
    if not isinstance(value, list):
        raise ValueError("noGoLines must be a list")
    return _validate_nogo_lines(
        [line.get("points") if isinstance(line, dict) else None for line in value]
    )


def _transform_nogo_lines(
    lines: list[list[dict[str, float]]],
    align: tuple[int, int, int] | None,
    *,
    inverse: bool,
) -> list[list[dict[str, float]]]:
    """Convert lines between a session's raw frame and the accumulated map.

    Replay alignment is a quarter turn followed by a shift in 5 cm cells.
    The robot stores no-go geometry in the raw frame it reports while running;
    the card edits in the stable map frame, so this conversion must bracket
    every load/save.
    """
    if not align or not any(align):
        return lines

    quarter, dx, dy = align
    quarter %= 4
    shift_x = dx * HISTORY_CELL_SIZE_M
    shift_y = dy * HISTORY_CELL_SIZE_M
    converted: list[list[dict[str, float]]] = []
    for line in lines:
        converted_line: list[dict[str, float]] = []
        for point in line:
            x = point["x"]
            y = point["y"]
            if inverse:
                x -= shift_x
                y -= shift_y
                if quarter == 1:
                    x, y = y, -x
                elif quarter == 2:
                    x, y = -x, -y
                elif quarter == 3:
                    x, y = -y, x
            else:
                if quarter == 1:
                    x, y = -y, x
                elif quarter == 2:
                    x, y = -x, -y
                elif quarter == 3:
                    x, y = y, -x
                x += shift_x
                y += shift_y
            converted_line.append({"x": round(x, 3), "y": round(y, 3)})
        converted.append(converted_line)
    return converted


def _nogo_alignment(data: dict[str, Any], reference_session: str) -> tuple[int, int, int] | None:
    """Return the map alignment used for the no-go reference session."""
    mapper = data.get("mapper")
    return mapper.alignment(reference_session) if mapper and reference_session else None


@websocket_api.websocket_command(
    {
        vol.Required("type"): "openneato/nogo_get",
        vol.Optional("entry_id"): str,
    }
)
@websocket_api.async_response
async def ws_get_nogo(
    hass: HomeAssistant,
    connection: websocket_api.ActiveConnection,
    msg: dict[str, Any],
) -> None:
    """Return robot no-go geometry converted into the card's map frame."""
    resolved = _resolve_entry(hass, msg.get("entry_id"))
    if resolved is None:
        connection.send_error(msg["id"], "not_found", "No OpenNeato config entry found")
        return
    _entry_id, data = resolved

    try:
        config = await data["api"].get_nogo_config()
        reference = str(config.get("referenceSession") or "")
        raw_lines = _unpack_nogo_lines(config.get("noGoLines") or [])
        map_lines = _transform_nogo_lines(
            raw_lines,
            _nogo_alignment(data, reference),
            inverse=False,
        )
    except Exception as err:  # noqa: BLE001
        _LOGGER.debug("No-go editor: configuration unavailable: %s", err)
        connection.send_error(msg["id"], "nogo_unavailable", str(err))
        return

    connection.send_result(
        msg["id"],
        {
            "enabled": bool(config.get("enabled")),
            "referenceSession": reference,
            "warningDistance": float(config.get("warningDistance") or 0.2),
            "noGoLines": map_lines,
            "mode": "enforce",
            "frame": "map",
        },
    )


@websocket_api.websocket_command(
    {
        vol.Required("type"): "openneato/nogo_set",
        vol.Optional("entry_id"): str,
        vol.Required("enabled"): bool,
        vol.Required("reference_session"): str,
        vol.Required("warning_distance"): vol.Coerce(float),
        vol.Required("lines"): list,
    }
)
@websocket_api.async_response
async def ws_set_nogo(
    hass: HomeAssistant,
    connection: websocket_api.ActiveConnection,
    msg: dict[str, Any],
) -> None:
    """Validate map-frame lines, convert them, and store them on the robot."""
    resolved = _resolve_entry(hass, msg.get("entry_id"))
    if resolved is None:
        connection.send_error(msg["id"], "not_found", "No OpenNeato config entry found")
        return
    _entry_id, data = resolved

    reference = msg["reference_session"].strip()
    warning_distance = float(msg["warning_distance"])
    try:
        if not reference:
            raise ValueError("select a reference cleaning session")
        if not 0.05 <= warning_distance <= 1.0:
            raise ValueError("warning distance must be between 0.05 and 1.0 metres")
        map_lines = _validate_nogo_lines(msg["lines"])
        if msg["enabled"] and not map_lines:
            raise ValueError("draw at least one no-go line before enabling")
        raw_lines = _transform_nogo_lines(
            map_lines,
            _nogo_alignment(data, reference),
            inverse=True,
        )
        await data["api"].update_nogo_config(
            {
                "enabled": msg["enabled"],
                "referenceSession": reference,
                "warningDistance": round(warning_distance, 3),
                "noGoLines": [{"points": line} for line in raw_lines],
            }
        )
        await data["coordinator"].async_request_refresh()
    except Exception as err:  # noqa: BLE001
        _LOGGER.warning("No-go editor: failed to save configuration: %s", err)
        connection.send_error(msg["id"], "save_failed", str(err))
        return

    connection.send_result(
        msg["id"],
        {
            "saved": True,
            "enabled": msg["enabled"],
            "referenceSession": reference,
            "warningDistance": round(warning_distance, 3),
            "noGoLines": map_lines,
            "mode": "enforce",
            "frame": "map",
        },
    )


@websocket_api.websocket_command(
    {
        vol.Required("type"): "openneato/session",
        vol.Required("name"): str,
        vol.Optional("entry_id"): str,
    }
)
@websocket_api.async_response
async def ws_get_session(
    hass: HomeAssistant,
    connection: websocket_api.ActiveConnection,
    msg: dict[str, Any],
) -> None:
    """Download and parse one cleaning session for playback."""
    resolved = _resolve_entry(hass, msg.get("entry_id"))
    if resolved is None:
        connection.send_error(msg["id"], "not_found", "No OpenNeato config entry found")
        return
    entry_id, data = resolved
    name = msg["name"]

    cache: dict[tuple[str, str], dict[str, Any]] = hass.data.setdefault(DOMAIN, {}).setdefault(
        CACHE_KEY, {}
    )
    cached = cache.get((entry_id, name))
    if cached is not None:
        connection.send_result(msg["id"], {**cached, "floorplan": _floorplan_payload(hass, entry_id)})
        return

    try:
        raw = await data["api"].get_history_session(name)
    except Exception as err:  # noqa: BLE001 -- surface any fetch failure to the card
        _LOGGER.warning("Replay: failed to fetch session %s: %s", name, err)
        connection.send_error(msg["id"], "fetch_failed", str(err))
        return

    # Serve the run in the map's frame, not the robot's frame of the day.
    runner = data.get("mapper")
    align = runner.alignment(name) if runner is not None else None

    # Coverage-grid construction is CPU-bound; keep it off the event loop.
    try:
        parsed = await hass.async_add_executor_job(
            build_replay_session, raw, name, align
        )
    except Exception as err:  # noqa: BLE001
        _LOGGER.exception("Replay: failed to parse session %s", name)
        connection.send_error(msg["id"], "parse_failed", str(err))
        return

    if not parsed.get("path"):
        connection.send_error(msg["id"], "empty_session", "Session contains no pose data")
        return

    # Only completed sessions are worth caching -- a recording one grows.
    if not _is_recording(data["coordinator"], name):
        if len(cache) >= _CACHE_MAX:
            cache.pop(next(iter(cache)))
        cache[(entry_id, name)] = parsed

    connection.send_result(msg["id"], {**parsed, "floorplan": _floorplan_payload(hass, entry_id)})


@websocket_api.websocket_command(
    {
        vol.Required("type"): "openneato/delete_session",
        vol.Required("name"): str,
        vol.Optional("entry_id"): str,
    }
)
@websocket_api.async_response
async def ws_delete_session(
    hass: HomeAssistant,
    connection: websocket_api.ActiveConnection,
    msg: dict[str, Any],
) -> None:
    """Delete one recorded session from the robot.

    For a run that went wrong -- the robot was picked up, the LIDAR was
    blocked, the map came out half-drawn -- so the bad replay stops cluttering
    the picker.

    Note this does NOT unpick the session's contribution to the accumulated
    wall map: `LidarMap` merges hit counts, and once merged a session's cells
    are indistinguishable from every other session's. Deleting a bad session
    stops it being replayed; it does not un-draw its walls.
    """
    resolved = _resolve_entry(hass, msg.get("entry_id"))
    if resolved is None:
        connection.send_error(msg["id"], "not_found", "No OpenNeato config entry found")
        return
    entry_id, data = resolved
    name = msg["name"]

    # Refuse while the robot is still writing to it: the firmware would be
    # appending to a file we just unlinked.
    if _is_recording(data["coordinator"], name):
        connection.send_error(
            msg["id"], "recording", "That session is still being recorded"
        )
        return

    try:
        await data["api"].delete_history_session(name)
    except Exception as err:  # noqa: BLE001
        _LOGGER.warning("Replay: failed to delete session %s: %s", name, err)
        connection.send_error(msg["id"], "delete_failed", str(err))
        return

    # Drop it from the parsed-session cache so a later request cannot serve
    # a session the robot no longer has.
    cache = hass.data.setdefault(DOMAIN, {}).setdefault(CACHE_KEY, {})
    cache.pop((entry_id, name), None)

    # Refresh so the picker's next listing no longer offers it.
    await data["coordinator"].async_request_refresh()

    _LOGGER.info("Replay: deleted session %s", name)
    connection.send_result(msg["id"], {"deleted": name})


def _is_recording(coordinator: Any, name: str) -> bool:
    """True while the robot is still appending to this session file."""
    history = (coordinator.data or {}).get("history")
    if not isinstance(history, list):
        return False
    return any(
        isinstance(item, dict) and item.get("name") == name and item.get("recording")
        for item in history
    )
