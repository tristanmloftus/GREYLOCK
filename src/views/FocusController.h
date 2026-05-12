#pragma once

// ---------------------------------------------------------------------------
// FocusController — Dashboard focus state machine (Task v0.3-1).
// ---------------------------------------------------------------------------
// Owns the "what is focused right now" answer for the Dashboard view.  The
// state machine is intentionally small: it tracks a level (Dashboard /
// Widget / Drill / Modal) and, when at Widget level, which widget on the
// 3-up + 2-up Dashboard grid is currently active.
//
// The App in src/main.cpp routes raw FTXUI events through
// FocusController::handle_key() BEFORE the existing per-view handlers.  If
// the controller consumes the event (returned true) the App stops; else
// the App falls through to legacy global / view-specific handlers.
//
// Views do NOT mutate the controller.  DashboardView reads
// is_widget_focused(w) to decorate each panel; widgets receive a `bool
// focused` parameter to toggle border / title styling.  This keeps the
// focus state in one place and makes it trivially testable with no FTXUI
// instance involved (see tests/test_focus_controller.cpp).
//
// SCOPE — Task v0.3-1
//   - Dashboard <-> Widget transitions via Tab / Shift-Tab / hjkl / arrows.
//   - Esc collapses Widget -> Dashboard; Esc at Dashboard is a no-op.
//   - Drill and Modal levels are declared but inert this task; v0.3-2/4
//     wire them in.
//   - context_hints() returns empty for now; v0.3-5 populates per-focus
//     status-bar hints.
//
// SEE ALSO
//   docs/UI_REDESIGN_V0.3.md §3a (focus model), §3f (keybinding map),
//   §4 Task v0.3-1.
// ---------------------------------------------------------------------------

#include <ftxui/component/event.hpp>

#include <string>
#include <vector>

namespace tf::views {

// ---------------------------------------------------------------------------
// WidgetId — stable enumeration of the five Dashboard widgets.
// ---------------------------------------------------------------------------
// Declaration order is the Tab cycle order.  Numeric values are stable
// and used as indices into the static grid lookup table inside
// FocusController.cpp.
//
// The grid (3-up + 2-up) is mapped onto a 2-row x 3-col virtual layout:
//
//   row 0:  NetWorth      ShovelScore       SyncStatus
//   row 1:  ShovelIntel   CategoryTrends    (empty)
//
// hjkl uses this geometry; Tab/Shift-Tab uses pure declaration order.
// See docs/UI_REDESIGN_V0.3.md §3a "Widget IDs and reading order".
// ---------------------------------------------------------------------------
enum class WidgetId : int {
    None               = 0,
    NetWorth           = 1,
    ShovelScore        = 2,
    SyncStatus         = 3,
    ShovelIntelligence = 4,
    CategoryTrends     = 5,
};

// ---------------------------------------------------------------------------
// FocusLevel — coarse-grained focus state.
// ---------------------------------------------------------------------------
// Only Dashboard and Widget are exercised in Task v0.3-1.  Drill and
// Modal are reserved values so the App can stub them today without an
// ABI break when v0.3-2/4 land.
// ---------------------------------------------------------------------------
enum class FocusLevel : int {
    Dashboard = 0,  // No widget focused; Tab/Shift-Tab enter the first/last.
    Widget    = 1,  // One widget focused; arrows/hjkl move between widgets.
    Drill     = 2,  // Reserved for v0.3-2/3 (no behavior here).
    Modal     = 3,  // Reserved for v0.3-4 (no behavior here).
};

// ---------------------------------------------------------------------------
// FocusController — single-owner state machine.
// ---------------------------------------------------------------------------
// Thread safety: NOT thread-safe.  Caller (the TUI event loop) must
// serialize all method calls.  This matches the v0.2 single-threaded
// event loop.
//
// Construction is cheap; no allocation beyond the back_stack_ vector
// which reserves space for the documented cap of 4 entries.
// ---------------------------------------------------------------------------
class FocusController {
public:
    // Construct in Dashboard / None state.  See class header for state
    // machine semantics.
    FocusController();

    // Route a single FTXUI event through the focus machine.
    //
    // Returns true iff the event was consumed (state changed OR an
    // explicit no-op was recognised, e.g. Esc at Dashboard level).
    // Returning false signals the App to pass the event to its
    // legacy global / view-specific handler chain.
    //
    // Recognised events (Task v0.3-1 scope):
    //   - Event::Tab            advance to next widget (or first from
    //                            Dashboard); wraps after the last.
    //   - Event::TabReverse     reverse of Tab; wraps after the first.
    //   - Event::Character('h') / Event::ArrowLeft   left in row, wraps.
    //   - Event::Character('l') / Event::ArrowRight  right in row, wraps.
    //   - Event::Character('j') / Event::ArrowDown   down a row; empty
    //                            cell falls back to that row's leftmost.
    //   - Event::Character('k') / Event::ArrowUp     up a row.
    //   - Event::Escape         Widget -> Dashboard; no-op at Dashboard.
    //
    // All other events return false unchanged.
    bool handle_key(const ftxui::Event& event);

    // Current coarse state.
    FocusLevel level() const noexcept;

    // The widget currently focused.  Returns WidgetId::None whenever
    // level() != FocusLevel::Widget.
    WidgetId focused_widget() const noexcept;

    // Convenience predicate used by DashboardView to decide whether
    // a given panel should render in its focused style.
    bool is_widget_focused(WidgetId w) const noexcept;

    // Reset to the initial state (Dashboard / None).  Called by the App
    // on view-switch so that switching from Dashboard to e.g. Accounts
    // does not leave a stale "Widget(NetWorth)" focus around to be
    // surprising on return.
    void reset();

    // Status-bar contextual hints for the current focus state.
    //
    // Returns empty in Task v0.3-1; v0.3-5 populates per-focus hint
    // strings like "[Enter] Show accounts breakdown".  Stub kept on the
    // public surface now to lock in the call site in main.cpp's status
    // bar render.
    std::vector<std::string> context_hints() const;

private:
    FocusLevel               level_;
    WidgetId                 focused_;
    std::vector<WidgetId>    back_stack_;  // capped at kMaxBackStackDepth.

    static constexpr std::size_t kMaxBackStackDepth = 4;
};

}  // namespace tf::views
