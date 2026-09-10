# Home Assistant dashboard example

[`examples/home-assistant-dashboard.yaml`](../examples/home-assistant-dashboard.yaml)
is a complete, optional Sections dashboard for one OpenNeato robot. It uses
Home Assistant's built-in Heading and Tile cards plus the replay/no-go card
installed by this integration. It does not require `button-card`,
`browser_mod`, or another dashboard add-on.

The example groups the controls that are useful during a real cleaning run:

- normal vacuum commands, fan mode, battery, charging, and last-clean data;
- navigation mode and the adjustable 100–400 cm spot-clean dimensions;
- no-go guard state, the last avoidance result, error clearing, and a guarded
  robot-restart button;
- the full map/replay/no-go editor; and
- bridge Wi-Fi, storage, uptime, and a guarded bridge-restart button.

## Install the example

1. In Home Assistant, open **Settings > Dashboards**, create a dashboard, and
   choose **Take control** if asked.
2. Open the dashboard's raw configuration editor and paste the example.
3. Replace every `my_neato` entity suffix with the entity IDs shown on your
   robot's Home Assistant device page. Entity IDs are suggestions made by Home
   Assistant, so names can differ if the robot or an entity was renamed.
4. Replace `REPLACE_WITH_CONFIG_ENTRY_ID` with the robot's OpenNeato config
   entry ID. Open the integration entry and copy the value after
   `/config/integrations/integration/` in the browser URL.
5. Save, then use **Edit dashboard** to rearrange or hide cards.

For two robots, duplicate the view, change its title/path, and replace both the
entity IDs and `entry_id`. Supplying `entry_id` is important when more than one
OpenNeato entry exists; it ensures that the map editor saves lines to the
intended robot.

## Spot-clean behavior

Set **Spot-clean width** and **Spot-clean height** before pressing the Spot
Clean command on the vacuum tile. The integration saves those dimensions to
the ESP32; firmware then sends `Clean Spot Width <cm> Height <cm>` to the
robot. The Number entities appear only when the connected firmware advertises
the new settings, so older firmware remains compatible without showing dead
controls.

Restart actions intentionally open Home Assistant's more-info panel instead of
firing immediately from the dashboard tile. No-go lines remain stored on the
ESP32 and the guard continues running when the dashboard is closed.
