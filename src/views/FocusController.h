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
// SCOPE — Task v0.3-1 / v0.3-2
//   - Dashboard <-> Widget transitions via Tab / Shift-Tab / hjkl / arrows.
//   - Esc collapses Drill -> Widget -> Dashboard; Esc at Dashboard is a
//     no-op.
//   - Drill level: enter_drill(WidgetId) / exit_drill() / drilled_widget()
//     are exercised by the NetWorth drill view; Modal stays inert
//     until v0.3-4.
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
// WidgetId — stable enumeration of the three Dashboard widgets.
// ---------------------------------------------------------------------------
// Declaration order is the Tab cycle order.  Numeric values are stable
// and used as indices into the static grid lookup table inside
// FocusController.cpp.
//
// The grid is a 2-row x 2-col virtual layout:
//
//   row 0:  NetWorth      SyncStatus
//   row 1:  CategoryTrends (empty)
//
// hjkl uses this geometry; Tab/Shift-Tab uses pure declaration order.
// ---------------------------------------------------------------------------
enum class WidgetId : int {
    None               = 0,
    NetWorth           = 1,
    SyncStatus         = 2,
    CategoryTrends     = 3,
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
    Drill     = 2,  // Drill view active (full-screen replacement).  Esc
                    // pops back to Widget level on the same widget.
                    // Currently wired for NetWorth only; the other
                    // widgets' drills are pending.
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

    // ---------------------------------------------------------------
    // Drill-level transitions (Task v0.3-2).
    // ---------------------------------------------------------------
    // enter_drill(w) takes the controller from FocusLevel::Widget
    // (focused on w) into FocusLevel::Drill and remembers w so Esc
    // can return to it.  Calling enter_drill from any other level is
    // a no-op (returns false) — the App is expected to only invoke
    // this when a widget is focused.
    //
    // exit_drill() pops back to FocusLevel::Widget on the same widget
    // that was drilled.  Calling outside Drill level is a no-op.
    //
    // drilled_widget() returns the widget currently drilled into, or
    // WidgetId::None when level() != FocusLevel::Drill.  The App reads
    // this to pick which Drill_* view to render.
    bool     enter_drill(WidgetId w);
    bool     exit_drill();
    WidgetId drilled_widget() const noexcept;

    // Reset to the initial state (Dashboard / None).  Called by the App
    // on view-switch so that switching from Dashboard to e.g. Accounts
    // does not leave a stale "Widget(NetWorth)" focus around to be
    // surprising on return.
    void reset();

    // ---------------------------------------------------------------------
    // Modal hooks (Task v0.3-4).
    // ---------------------------------------------------------------------
    // The App calls enter_modal() when it opens a modal overlay (command
    // palette via ":", help overlay via "?") and exit_modal() when the
    // overlay closes (Esc, or successful command dispatch).
    //
    // The controller stashes the pre-modal (level, focused_widget) pair
    // so exit_modal() restores it exactly.  Two-level nesting is NOT
    // supported -- entering a modal while another is open is treated as
    // a no-op (the controller stays in its already-modal state).  In
    // practice the App enforces single-modal-open as a UI invariant.
    //
    // Modal-level events (typing into the palette, j/k to move selection,
    // Esc to dismiss) are routed by the modal component itself, NOT by
    // the FocusController -- so handle_key() in this controller is a
    // no-op while is_modal_open() is true.  The App's event loop checks
    // is_modal_open() first and dispatches to the modal directly.
    void enter_modal();
    void exit_modal();
    bool is_modal_open() const noexcept;

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
    WidgetId                 drilled_ = WidgetId::None;
    std::vector<WidgetId>    back_stack_;  // capped at kMaxBackStackDepth.

    // Pre-modal state stash.  Populated by enter_modal() and consumed by
    // exit_modal().  Defaulted to Dashboard/None for the first
    // enter-then-exit cycle so a modal opened directly from the initial
    // app state restores cleanly.
    FocusLevel               pre_modal_level_;
    WidgetId                 pre_modal_focused_;

    static constexpr std::size_t kMaxBackStackDepth = 4;
};

}  // namespace tf::views
