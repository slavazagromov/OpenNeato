"""Data update coordinator for OpenNeato."""

from __future__ import annotations

import asyncio
from datetime import timedelta
import logging
from time import monotonic
from typing import Any
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

from dateutil.tz import tzstr

from homeassistant.core import HomeAssistant
from homeassistant.util import dt as dt_util
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed

from .api import OpenNeatoApiClient, OpenNeatoConnectionError
from .const import (
    DEFAULT_POLL_INTERVAL,
    DOMAIN,
    EVENT_NOGO_BREACHED,
    EVENT_NOGO_NEAR,
)

_LOGGER = logging.getLogger(__name__)


def scheduled_poll_delay(settings: dict[str, Any]) -> float | None:
    """Sleep until ten minutes before the next ESP32 cleaning slot."""
    if not settings.get("scheduleEnabled"):
        return None
    now = dt_util.now()
    robot_tz = settings.get("tz")
    if robot_tz:
        try:
            try:
                zone = ZoneInfo(robot_tz)
            except ZoneInfoNotFoundError:
                zone = tzstr(robot_tz, posix_offset=True)
            now = now.astimezone(zone)
        except (TypeError, ValueError):
            _LOGGER.warning("Invalid robot timezone %r; using HA timezone", robot_tz)
    delays = []
    for day_offset in range(8):
        date = now + timedelta(days=day_offset)
        for slot in range(2):
            prefix = f"sched{date.weekday()}" + ("Slot1" if slot else "")
            if not settings.get(f"{prefix}On"):
                continue
            try:
                target = date.replace(hour=int(settings[f"{prefix}Hour"]),
                                      minute=int(settings[f"{prefix}Min"]), second=0, microsecond=0)
            except (KeyError, TypeError, ValueError):
                continue
            seconds = target.timestamp() - now.timestamp()
            # Monitor warm-up and a five-minute grace period for startup.
            if -300 <= seconds <= 600:
                return 15
            if seconds > 600:
                delays.append(seconds - 600)
    return min(delays) if delays else None


def _rank_session_ts(session: dict[str, Any]) -> float:
    """Shared ranking key: prefer summary.time, fall back to filename epoch.

    Firmware directory iteration order isn't guaranteed, so we can't
    trust the list order. Summary.time is the clean's end timestamp;
    filenames are epoch seconds at session start.
    """
    summary = session.get("summary")
    if isinstance(summary, dict):
        raw = summary.get("time", 0)
        if isinstance(raw, (int, float)) and raw > 0:
            return float(raw)
    name = session.get("name") or ""
    try:
        return float(name.split(".", 1)[0])
    except ValueError:
        return 0.0


def latest_completed_session(history: Any) -> dict[str, Any] | None:
    """Return the most recent completed (non-recording) session entry."""
    if not isinstance(history, list):
        return None
    best: dict[str, Any] | None = None
    best_key = -1.0
    for session in history:
        if not isinstance(session, dict) or session.get("recording"):
            continue
        if not session.get("name"):
            continue
        key = _rank_session_ts(session)
        if key > best_key:
            best_key = key
            best = session
    return best


class OpenNeatoCoordinator(DataUpdateCoordinator[dict[str, Any]]):
    """Single coordinator for all OpenNeato data."""

    def __init__(
        self, hass: HomeAssistant, api: OpenNeatoApiClient, serial: str
    ) -> None:
        """Initialize the coordinator."""
        super().__init__(
            hass,
            _LOGGER,
            name=DOMAIN,
            update_interval=timedelta(seconds=DEFAULT_POLL_INTERVAL),
        )
        self.api = api
        self.serial = serial
        self._last_full_refresh = 0.0
        self._was_active = False
        self._force_full_refresh = False
        self._settle_until = 0.0

    async def async_request_refresh(self) -> None:
        """Commands/settings changes wake monitoring without idle polling."""
        self._force_full_refresh = True
        await super().async_request_refresh()

    async def _async_update_data(self) -> dict[str, Any]:
        """Watch state lightly; refresh telemetry only when useful."""
        try:
            state = await self.api.get_state()
        except Exception as err:  # Preserve the existing critical-endpoint fallback.
            state = err
        ui = state.get("uiState", "") if isinstance(state, dict) else ""
        active = any(word in ui for word in ("CLEANING", "DOCKING", "TESTMODE"))
        now = monotonic()
        keep_monitoring_on_failure = (
            self._was_active
            or scheduled_poll_delay((self.data or {}).get("settings", {})) == 15
        )
        if keep_monitoring_on_failure:
            # Keep retrying through the pre-clean reboot's brief outage.
            self.update_interval = timedelta(seconds=15)
        full = self.data is None or self._force_full_refresh or now - self._last_full_refresh >= 3600
        self._force_full_refresh = False
        transition = active != self._was_active and not isinstance(state, Exception)
        if transition and not active:
            self._settle_until = now + 120
        getters = {
            "charger": self.api.get_charger,
            "error": self.api.get_error,
            "user_settings": self.api.get_user_settings,
            "system": self.api.get_system,
            "settings": self.api.get_settings,
            "history": self.api.get_history,
            "sensors": self.api.get_sensors,
            "analog": self.api.get_battery_analog,
            "warranty": self.api.get_battery_warranty,
            "nogo": self.api.get_nogo_status,
        }
        selected = list(getters) if full or transition else (
            ["charger", "error", "system", "history", "sensors", "nogo"] if active else []
        )
        if self.data is None:
            # Keep first setup bounded even when each serial endpoint is slow.
            # Settings are needed to schedule the next monitoring wake-up.
            selected = ["charger", "system", "settings", "history"]
        elif not active and now < self._settle_until and "history" not in selected:
            selected.append("history")
        # If a UART state read fails, test the bridge itself before declaring
        # it offline. /api/system does not depend on robot serial responses.
        if isinstance(state, Exception):
            for key in ("charger", "system"):
                if key not in selected:
                    selected.append(key)
        results = [state, *await asyncio.gather(
            *(getters[key]() for key in selected), return_exceptions=True
        )]
        keys = ("state", *selected)
        # Critical endpoints — if ALL of these fail we consider the robot
        # unreachable. Non-critical endpoints (like /api/error, which can hang
        # if the robot's serial interface is stuck) are allowed to fail
        # individually without breaking the integration.
        critical_keys = {"state", "charger", "system"}

        data: dict[str, Any] = dict(self.data or {})
        for key in getters:
            data.setdefault(key, [] if key == "history" else {})
        failures: list[str] = []
        critical_failures: list[str] = []

        for key, result in zip(keys, results):
            if isinstance(result, Exception):
                if isinstance(result, OpenNeatoConnectionError):
                    _LOGGER.warning("Timeout/connection error on %s: %s", key, result)
                else:
                    _LOGGER.warning("Failed to fetch %s: %s", key, result)
                failures.append(key)
                if key in critical_keys:
                    critical_failures.append(key)
                # Fall back to previous value if we have one
                if self.data and key in self.data:
                    data[key] = self.data[key]
                else:
                    data[key] = {} if key != "history" else []
            else:
                data[key] = result

        # Only fail the whole coordinator if ALL critical endpoints failed.
        # This means a single hung endpoint (e.g. /api/error when the robot's
        # serial interface gets stuck) doesn't break the rest of the
        # integration.
        if critical_failures and len(critical_failures) == len(critical_keys):
            if not keep_monitoring_on_failure:
                self.update_interval = None
            raise UpdateFailed(
                f"All critical endpoints failed: {', '.join(critical_failures)}"
            )

        if full and not failures:
            self._last_full_refresh = now
        # A partial hourly refresh must not retry every minute forever.
        elif full and self.data is not None:
            self._last_full_refresh = now
        if not isinstance(state, Exception):
            self._was_active = active
        delay = 15 if self._was_active else scheduled_poll_delay(data.get("settings", {}))
        if now < self._settle_until and any(
            isinstance(item, dict) and item.get("recording") for item in data.get("history", [])
        ):
            delay = 15
        self.update_interval = timedelta(seconds=delay) if delay is not None else None

        if failures:
            _LOGGER.debug(
                "Coordinator update succeeded with %d failed endpoints: %s",
                len(failures), ", ".join(failures),
            )

        previous_nogo = self.data.get("nogo", {}) if self.data else None
        current_nogo = data.get("nogo", {})
        if previous_nogo is not None and not isinstance(previous_nogo, dict):
            previous_nogo = {}
        if previous_nogo is not None and isinstance(current_nogo, dict):
            for field, event_type in (
                ("near", EVENT_NOGO_NEAR),
                ("breached", EVENT_NOGO_BREACHED),
            ):
                if current_nogo.get(field) and not previous_nogo.get(field):
                    self.hass.bus.async_fire(
                        event_type,
                        {
                            "serial": self.serial,
                            "host": self.api.base_url,
                            "x": current_nogo.get("lastX"),
                            "y": current_nogo.get("lastY"),
                            "distance": current_nogo.get("lastDistance"),
                            "reference_session": current_nogo.get(
                                "referenceSession"
                            ),
                        },
                    )

        return data
