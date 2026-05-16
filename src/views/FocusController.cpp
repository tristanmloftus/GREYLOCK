// ---------------------------------------------------------------------------
// FocusController.cpp — Dashboard focus state machine.
// ---------------------------------------------------------------------------
// See FocusController.h for the public contract.  This file implements
// the state machine, the static 2x2 grid backing hjkl movement, and the
// wrap policy.
//
// WRAP POLICY
//   - Tab / Shift-Tab : wrap around declaration order.
//   - h / Left, l / Right : wrap within the focused widget's row.
//   - j / Down, k / Up    : wrap between the two rows on the same column.
//                            Empty cells fall back to the leftmost populated
//                            cell on the target row.
//
// ---------------------------------------------------------------------------

#include "FocusController.h"

#include <algorithm>
#include <array>

namespace tf::views {

namespace {

// ---------------------------------------------------------------------------
// Declaration order — the Tab / Shift-Tab cycle.
// ---------------------------------------------------------------------------
// Order MUST match the reading-order intent (left-to-right, top-to-
// bottom).  Indexed by the position of Tab presses from Dashboard.
// ---------------------------------------------------------------------------
constexpr std::array<WidgetId, 3> kTabOrder = {
    WidgetId::NetWorth,
    WidgetId::SyncStatus,
    WidgetId::CategoryTrends,
};

// ---------------------------------------------------------------------------
// Grid coordinates for the 2-row x 2-col Dashboard layout.
// ---------------------------------------------------------------------------
// row 0 ->  NetWorth         SyncStatus
// row 1 ->  CategoryTrends   (empty)
//
// kGrid[r][c] holds the widget at that cell, or WidgetId::None for the
// single empty cell.  hjkl movement consults this table; declaration-
// order (Tab) does not.
// ---------------------------------------------------------------------------
constexpr int kRows = 2;
constexpr int kCols = 2;

constexpr std::array<std::array<WidgetId, kCols>, kRows> kGrid = {{
    {WidgetId::NetWorth,        WidgetId::SyncStatus},
    {WidgetId::CategoryTrends,  WidgetId::None},
}};

// Find the (row, col) of a given widget.  Returns false if not on grid
// (e.g. WidgetId::None).
bool find_grid_position(WidgetId w, int& row, int& col) {
    for (int r = 0; r < kRows; ++r) {
        for (int c = 0; c < kCols; ++c) {
            if (kGrid[r][c] == w) {
                row = r;
                col = c;
                return true;
            }
        }
    }
    return false;
}

// Return the leftmost populated WidgetId in the given row.  Used when a
// vertical move lands on the single empty cell at (1, 2).
WidgetId leftmost_in_row(int row) {
    for (int c = 0; c < kCols; ++c) {
        if (kGrid[row][c] != WidgetId::None) {
            return kGrid[row][c];
        }
    }
    // Unreachable for the current grid (every row has at least one
    // populated cell); defensive fallback.
    return kGrid[row][0];
}

// Find the index of `w` in kTabOrder.  Returns -1 if not found.
int tab_index(WidgetId w) {
    for (std::size_t i = 0; i < kTabOrder.size(); ++i) {
        if (kTabOrder[i] == w) return static_cast<int>(i);
    }
    return -1;
}

// ---------------------------------------------------------------------------
// hjkl resolvers — each returns the WidgetId that should receive focus
// after the corresponding move.  Operates on the grid model described in
// the file header.  All four wrap; j/Down + k/Up apply the empty-cell
// fallback when the target cell is unpopulated.
// ---------------------------------------------------------------------------
WidgetId move_left(WidgetId current) {
    int r = 0, c = 0;
    if (!find_grid_position(current, r, c)) return current;
    // Wrap left within the row, skipping empty cells.
    for (int step = 1; step <= kCols; ++step) {
        const int nc = (c - step + kCols) % kCols;
        if (kGrid[r][nc] != WidgetId::None) return kGrid[r][nc];
    }
    return current;
}

WidgetId move_right(WidgetId current) {
    int r = 0, c = 0;
    if (!find_grid_position(current, r, c)) return current;
    for (int step = 1; step <= kCols; ++step) {
        const int nc = (c + step) % kCols;
        if (kGrid[r][nc] != WidgetId::None) return kGrid[r][nc];
    }
    return current;
}

WidgetId move_down(WidgetId current) {
    int r = 0, c = 0;
    if (!find_grid_position(current, r, c)) return current;
    const int nr = (r + 1) % kRows;
    if (kGrid[nr][c] != WidgetId::None) return kGrid[nr][c];
    // Empty target cell: fall back to leftmost populated cell on target row.
    return leftmost_in_row(nr);
}

WidgetId move_up(WidgetId current) {
    int r = 0, c = 0;
    if (!find_grid_position(current, r, c)) return current;
    const int nr = (r - 1 + kRows) % kRows;
    if (kGrid[nr][c] != WidgetId::None) return kGrid[nr][c];
    return leftmost_in_row(nr);
}

}  // namespace

// ---------------------------------------------------------------------------
// Constructor — initial state is Dashboard / None.  back_stack_ reserves
// kMaxBackStackDepth (4) slots up front so push/pop in v0.3-2+ is
// allocation-free.
// ---------------------------------------------------------------------------
FocusController::FocusController()
    : level_(FocusLevel::Dashboard),
      focused_(WidgetId::None),
      pre_modal_level_(FocusLevel::Dashboard),
      pre_modal_focused_(WidgetId::None) {
    back_stack_.reserve(kMaxBackStackDepth);
}

// ---------------------------------------------------------------------------
// handle_key — main dispatch.  See header for recognised events.  Returns
// true iff the event was consumed (state changed OR a deliberate no-op).
// ---------------------------------------------------------------------------
bool FocusController::handle_key(const ftxui::Event& event) {
    using ftxui::Event;

    // ---------------------------------------------------------------------
    // Esc — pop one level.  At Dashboard level Esc is an explicit no-op
    // per Q3 of the design proposal; we still return true so the App
    // does NOT propagate Esc to legacy handlers (where it would currently
    // exit the app).  This gates the v0.3 quit-via-q convention.
    // ---------------------------------------------------------------------
    if (event == Event::Escape) {
        // Drill -> Widget: pop the drill, restore widget focus on the
        // widget that owned it.  Task v0.3-2.
        if (level_ == FocusLevel::Drill) {
            level_    = FocusLevel::Widget;
            focused_  = drilled_;
            drilled_  = WidgetId::None;
            return true;
        }
        if (level_ == FocusLevel::Widget) {
            level_   = FocusLevel::Dashboard;
            focused_ = WidgetId::None;
            return true;
        }
        // Dashboard / Modal: no-op for this task.  Dashboard stays a hard
        // no-op (Q3 of the design); Modal handling lands in v0.3-4.
        if (level_ == FocusLevel::Dashboard) {
            return false;
        }
        return false;
    }

    // ---------------------------------------------------------------------
    // Tab / Shift-Tab — declaration-order cycle.
    // ---------------------------------------------------------------------
    if (event == Event::Tab) {
        if (level_ == FocusLevel::Dashboard) {
            level_   = FocusLevel::Widget;
            focused_ = kTabOrder.front();
            return true;
        }
        if (level_ == FocusLevel::Widget) {
            const int idx = tab_index(focused_);
            if (idx < 0) {
                focused_ = kTabOrder.front();
            } else {
                focused_ = kTabOrder[(idx + 1) % kTabOrder.size()];
            }
            return true;
        }
        return false;
    }
    if (event == Event::TabReverse) {
        if (level_ == FocusLevel::Dashboard) {
            level_   = FocusLevel::Widget;
            focused_ = kTabOrder.back();
            return true;
        }
        if (level_ == FocusLevel::Widget) {
            const int idx = tab_index(focused_);
            const int n   = static_cast<int>(kTabOrder.size());
            if (idx < 0) {
                focused_ = kTabOrder.back();
            } else {
                focused_ = kTabOrder[(idx - 1 + n) % n];
            }
            return true;
        }
        return false;
    }

    // ---------------------------------------------------------------------
    // hjkl + arrows — grid-resolved movement.  Only meaningful when a
    // widget is already focused; from Dashboard level, hjkl/arrows are
    // not claimed (the App's view-specific handlers see them).  From
    // Drill level, hjkl/arrows fall through here (return false) so the
    // App can route them to the drill view's own scroll handler.
    // ---------------------------------------------------------------------
    if (level_ != FocusLevel::Widget) return false;

    if (event == Event::Character('h') || event == Event::ArrowLeft) {
        focused_ = move_left(focused_);
        return true;
    }
    if (event == Event::Character('l') || event == Event::ArrowRight) {
        focused_ = move_right(focused_);
        return true;
    }
    if (event == Event::Character('j') || event == Event::ArrowDown) {
        focused_ = move_down(focused_);
        return true;
    }
    if (event == Event::Character('k') || event == Event::ArrowUp) {
        focused_ = move_up(focused_);
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Accessors — pure, allocation-free.
// ---------------------------------------------------------------------------
FocusLevel FocusController::level() const noexcept {
    return level_;
}

WidgetId FocusController::focused_widget() const noexcept {
    return focused_;
}

bool FocusController::is_widget_focused(WidgetId w) const noexcept {
    return level_ == FocusLevel::Widget && focused_ == w && w != WidgetId::None;
}

// ---------------------------------------------------------------------------
// Drill-level transitions (Task v0.3-2).
// ---------------------------------------------------------------------------
// enter_drill: Widget(w) -> Drill(w).  Only valid from Widget level,
// and only when w matches the currently focused widget (the App MUST
// only call enter_drill on Enter while a widget is focused).  Other
// shapes of caller error return false without mutating state.
//
// exit_drill: Drill(w) -> Widget(w).  Symmetric.
//
// drilled_widget: trivial accessor used by main.cpp render() to pick
// which Drill_* view to instantiate.
// ---------------------------------------------------------------------------
bool FocusController::enter_drill(WidgetId w) {
    if (level_ != FocusLevel::Widget) return false;
    if (w == WidgetId::None)          return false;
    // Defensive: only drill from the widget that's actually focused.
    // The App's wiring guarantees this, but checking here keeps the
    // state machine self-consistent under future re-wires.
    if (focused_ != w)                return false;
    drilled_ = w;
    level_   = FocusLevel::Drill;
    // focused_ stays set; render code can still read focused_widget()
    // while we're inside the drill if it wants the underlying widget.
    return true;
}

bool FocusController::exit_drill() {
    if (level_ != FocusLevel::Drill) return false;
    level_   = FocusLevel::Widget;
    focused_ = drilled_;
    drilled_ = WidgetId::None;
    return true;
}

WidgetId FocusController::drilled_widget() const noexcept {
    if (level_ != FocusLevel::Drill) return WidgetId::None;
    return drilled_;
}

// ---------------------------------------------------------------------------
// reset — restore initial state.  Called by the App when switching away
// from the Dashboard so a future return lands at Dashboard level rather
// than retaining a stale widget focus.
// ---------------------------------------------------------------------------
void FocusController::reset() {
    level_              = FocusLevel::Dashboard;
    focused_            = WidgetId::None;
    drilled_            = WidgetId::None;
    pre_modal_level_    = FocusLevel::Dashboard;
    pre_modal_focused_  = WidgetId::None;
    back_stack_.clear();
}

// ---------------------------------------------------------------------------
// Modal hooks (Task v0.3-4)
// ---------------------------------------------------------------------------
// enter_modal stashes the current (level, focused) so exit_modal can
// restore them exactly.  Re-entering while already in Modal is a no-op
// (the App is responsible for single-modal-open as a UI invariant).
//
// While in Modal level, handle_key returns false unconditionally -- the
// App routes events to the modal component directly via the path in
// main.cpp.  We do not need to muddy handle_key's logic since the App
// checks is_modal_open() before calling it.
// ---------------------------------------------------------------------------
void FocusController::enter_modal() {
    if (level_ == FocusLevel::Modal) return;
    pre_modal_level_   = level_;
    pre_modal_focused_ = focused_;
    level_   = FocusLevel::Modal;
    focused_ = WidgetId::None;
}

void FocusController::exit_modal() {
    if (level_ != FocusLevel::Modal) return;
    level_   = pre_modal_level_;
    focused_ = pre_modal_focused_;
}

bool FocusController::is_modal_open() const noexcept {
    return level_ == FocusLevel::Modal;
}

// ---------------------------------------------------------------------------
// context_hints — stub for Task v0.3-1.  v0.3-5 fills in per-focus hints
// read by main.cpp's status bar render.
// ---------------------------------------------------------------------------
std::vector<std::string> FocusController::context_hints() const {
    return {};
}

}  // namespace tf::views
