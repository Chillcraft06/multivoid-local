// ui/world_rules_panel.h -- the F1 > World > Rules content pane (EVERYONE).
//
// A read-only view of the world rules THIS peer is running under, laid out as the game's own rules
// pane lays them out: its categories, its order, its names and descriptions, the enum values by
// their names. The layout is read from the game (ue_wrap::game_rules_pane), so a rule the game adds
// appears here too, under "Other" until the game's pane places it. Shown to host, clients and
// solo: a non-dev, non-host F1 category.
//
// The values are the per-process copy the game reads its rules from. The host's save sets it on
// every peer at load; the panel says so when that copy and the saved one differ. The reads are a
// game-thread snapshot taken on (re)open and on a world change, retried once a second, a bounded
// number of times, while a world is still bringing its save object up; the render paints the cache.
// Principle 7: the reflected reads live in ue_wrap; this file only renders.

#pragma once

namespace ui::world_rules_panel {

// Draw the World Rules pane content (called from dev_menu's content child).
void Render();

// Re-read the rules on the game thread (the next snapshot the render paints). The render calls
// this on (re)open and on world change itself; the rules sync calls it after a remote apply so a
// client re-renders without reopening the panel. Render-thread safe.
void RequestRefresh();

}  // namespace ui::world_rules_panel
