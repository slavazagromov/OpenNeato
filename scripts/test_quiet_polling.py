"""Behavior tests with minimal HA stubs, no HA installation required."""
import asyncio
import importlib.util
import sys
import types
import unittest
from datetime import datetime
from pathlib import Path
from unittest.mock import patch
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parents[1]
for name in ("homeassistant", "homeassistant.core", "homeassistant.components", "homeassistant.components.vacuum", "homeassistant.helpers",
             "homeassistant.helpers.update_coordinator", "homeassistant.exceptions",
             "homeassistant.util", "homeassistant.util.dt", "quiet"):
    sys.modules[name] = types.ModuleType(name)
sys.modules["homeassistant.core"].HomeAssistant = object
sys.modules["homeassistant.components.vacuum"].VacuumActivity = types.SimpleNamespace(CLEANING="cleaning", PAUSED="paused", RETURNING="returning")
sys.modules["homeassistant.exceptions"].HomeAssistantError = Exception
ha_coord = sys.modules["homeassistant.helpers.update_coordinator"]
ha_coord.UpdateFailed = type("UpdateFailed", (Exception,), {})

class CoordinatorStub:
    def __class_getitem__(cls, _):
        return cls
    def __init__(self, hass, logger, **kwargs):
        self.data = None
        self.update_interval = kwargs["update_interval"]
    async def async_request_refresh(self):
        self.data = await self._async_update_data()

ha_coord.DataUpdateCoordinator = CoordinatorStub
dt = sys.modules["homeassistant.util.dt"]
dt.now = lambda: datetime(2026, 9, 30, 8, 0, tzinfo=ZoneInfo("America/Chicago"))
def load(name):
    spec = importlib.util.spec_from_file_location(f"quiet.{name}", ROOT / "custom_components/openneato" / f"{name}.py")
    result = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = result
    spec.loader.exec_module(result)
    return result
load("const")
api_module = load("api")
coordinator = load("coordinator")

class FakeApi:
    def __init__(self):
        self.calls, self.fail = [], set()
        self.ui = "UIMGR_STATE_IDLE"
        self.settings = {"scheduleEnabled": True, "sched2On": True, "sched2Hour": 9, "sched2Min": 0}
    def __getattr__(self, name):
        async def call():
            self.calls.append(name)
            if name in self.fail:
                raise api_module.OpenNeatoConnectionError("offline")
            if name == "get_state":
                return {"uiState": self.ui}
            if name == "get_settings":
                return self.settings
            return [] if name == "get_history" else {}
        return call

class QuietPollingTests(unittest.IsolatedAsyncioTestCase):
    def make(self):
        api = FakeApi()
        hass = types.SimpleNamespace(bus=types.SimpleNamespace(async_fire=lambda *args: None))
        return api, coordinator.OpenNeatoCoordinator(hass, api, "test")
    async def test_bounded_startup_and_schedule_wake(self):
        api, c = self.make()
        c.data = await c._async_update_data()
        self.assertEqual(api.calls, ["get_state", "get_charger", "get_system", "get_settings", "get_history"])
        self.assertEqual(c.update_interval.total_seconds(), 3000)
    async def test_cleaning_active_then_dock_sleeps(self):
        api, c = self.make()
        c.data = await c._async_update_data()
        api.ui = "UIMGR_STATE_HOUSECLEANINGRUNNING"
        c.data = await c._async_update_data()
        self.assertEqual(c.update_interval.total_seconds(), 15)
        api.ui = "UIMGR_STATE_IDLE"
        api.settings["scheduleEnabled"] = False
        c.data = await c._async_update_data()
        self.assertIsNone(c.update_interval)
    async def test_unreachable_still_fails(self):
        api, c = self.make()
        api.fail = {"get_state", "get_charger", "get_system"}
        with self.assertRaises(ha_coord.UpdateFailed):
            await c._async_update_data()
        self.assertIsNone(c.update_interval)
    async def test_uart_failure_keeps_previous_state(self):
        api, c = self.make()
        c.data = await c._async_update_data()
        api.fail = {"get_state"}
        data = await c._async_update_data()
        self.assertEqual(data["state"]["uiState"], "UIMGR_STATE_IDLE")
    async def test_command_wakes_settings(self):
        api, c = self.make()
        c.data = await c._async_update_data()
        api.calls.clear()
        await c.async_request_refresh()
        self.assertIn("get_settings", api.calls)
    async def test_midnight_schedule(self):
        midnight = datetime(2026, 9, 30, 23, 55, tzinfo=ZoneInfo("America/Chicago"))
        with patch.object(dt, "now", return_value=midnight):
            settings = {"scheduleEnabled": True, "sched3On": True, "sched3Hour": 0, "sched3Min": 5}
            self.assertEqual(coordinator.scheduled_poll_delay(settings), 15)
        self.assertIsNone(coordinator.scheduled_poll_delay({"scheduleEnabled": False}))
    async def test_robot_timezone_controls_wake(self):
        settings = {"tz": "UTC0", "scheduleEnabled": True, "sched2On": True,
                    "sched2Hour": 14, "sched2Min": 0}
        self.assertEqual(coordinator.scheduled_poll_delay(settings), 3000)
    async def test_final_map_gets_bounded_settling_time(self):
        api, c = self.make()
        c.data = await c._async_update_data()
        api.ui = "UIMGR_STATE_HOUSECLEANINGRUNNING"
        c.data = await c._async_update_data()
        api.ui = "UIMGR_STATE_IDLE"
        api.settings["scheduleEnabled"] = False
        async def recording():
            return [{"name": "map", "recording": True}]
        api.get_history = recording
        c.data = await c._async_update_data()
        self.assertEqual(c.update_interval.total_seconds(), 15)
        c._settle_until = 0
        c.data = await c._async_update_data()
        self.assertIsNone(c.update_interval)
    async def test_real_http_limiter_caps_parallel_requests(self):
        session = types.SimpleNamespace(active=0, peak=0)
        class Response:
            status, content_type = 200, "application/json"
            async def __aenter__(self):
                session.active += 1
                session.peak = max(session.peak, session.active)
                await asyncio.sleep(0.01)
                return self
            async def __aexit__(self, *args):
                session.active -= 1
            def raise_for_status(self):
                pass
            async def read(self):
                return b'{}'
        session.get = lambda *args, **kwargs: Response()
        api = api_module.OpenNeatoApiClient("test", session)
        await asyncio.gather(*(api.get_state() for _ in range(12)))
        self.assertEqual(session.peak, 2)
        self.assertEqual(session.active, 0)

if __name__ == "__main__":
    unittest.main()
