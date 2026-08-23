#include <vector>
#include <algorithm>
#include <string>
#include <sstream>
#include <iomanip>
#include <any>
#include <chrono>
#include <memory>
#include <map>
#include <set>
#include <list>
#include <deque>
#include <array>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <thread>
#include <functional>
#include <variant>
#include <fstream>

#define private public
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollingAlgorithm.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/desktop/state/GlobalWindowController.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#undef private

#include <regex>
#include <cctype>


#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

static HANDLE PHANDLE = nullptr;

static Hyprutils::Signal::CHyprSignalListener activeWindowListener;
static Hyprutils::Signal::CHyprSignalListener activeWorkspaceListener;
static Hyprutils::Signal::CHyprSignalListener specialWorkspaceListener;
static Hyprutils::Signal::CHyprSignalListener fullscreenListener;
static Hyprutils::Signal::CHyprSignalListener windowOpenListener;
static Hyprutils::Signal::CHyprSignalListener windowCloseListener;
static Hyprutils::Signal::CHyprSignalListener windowMoveToWorkspaceListener;
static SP<CEventLoopTimer> pFullscreenRestoreTimer = nullptr;

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

// Global state structures
struct SWorkspaceSettings {
    bool zenActive = false;
    int64_t focusFitMethod = 1;
    int partitions = 2;
    int viewportStartIdx = 0;
};

static std::unordered_map<WORKSPACEID, SWorkspaceSettings> g_workspaceSettings;

static WORKSPACEID g_lastWorkspaceID = -1;
static int64_t g_defaultFocusFitMethod = 1;

static bool g_zenModeActive = false;
static int64_t g_globalFocusFitMethod = 1;

static SP<Config::Values::CBoolValue> g_zenModeGlobalConfig;
static SP<Config::Values::CBoolValue> g_centerModeGlobalConfig;

// Helper functions.
//
// Reads of native Hyprland config keys use Config::Values::CConfigValue<T> at
// each call site instead (the sanctioned, documented accessor — see
// ScrollingAlgorithm.cpp upstream for the same pattern). There is no
// equivalent safe write path: CConfigValue<Config::INTEGER>::ptr() casts
// blindly to int64_t*, but a key declared as INTEGER may be backed by either
// `bool` or `int64_t` storage at runtime (see operator*()'s own type branch
// in ConfigValue.hpp) - writing through ptr() would silently corrupt a
// bool-backed value. The manual dataptr+type branching below is what
// actually handles that correctly, so it stays for writes.
static void setConfigValueInt(const std::string& name, int64_t value) {
    auto reply = Config::mgr()->getConfigValue(name);
    if (reply.dataptr && reply.type) {
        if (*reply.type == typeid(int)) {
            *(int*)(*reply.dataptr) = (int)value;
        } else if (*reply.type == typeid(int64_t)) {
            *(int64_t*)(*reply.dataptr) = value;
        } else if (*reply.type == typeid(long)) {
            *(long*)(*reply.dataptr) = (long)value;
        } else if (*reply.type == typeid(long long)) {
            *(long long*)(*reply.dataptr) = (long long)value;
        } else if (*reply.type == typeid(bool)) {
            *(bool*)(*reply.dataptr) = (bool)value;
        }
    }
}

static void setConfigValueBool(const std::string& name, bool value) {
    auto reply = Config::mgr()->getConfigValue(name);
    if (reply.dataptr && reply.type) {
        if (*reply.type == typeid(bool)) {
            *(bool*)(*reply.dataptr) = value;
        } else if (*reply.type == typeid(int)) {
            *(int*)(*reply.dataptr) = value ? 1 : 0;
        } else if (*reply.type == typeid(int64_t)) {
            *(int64_t*)(*reply.dataptr) = value ? 1 : 0;
        }
    }
}

// Helper to get hex address string of a window
std::string getAddressStr(PHLWINDOW pWindow) {
    if (!pWindow) return "";
    std::stringstream ss;
    ss << "0x" << std::hex << (uint64_t)pWindow.get();
    return ss.str();
}

std::string getTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

void logState(const std::string& action) {
    std::ofstream logFile("/tmp/hyprscrolling-plus.log", std::ios::app);
    if (!logFile.is_open()) return;

    logFile << "[" << getTimestamp() << "] ACTION: " << action << "\n";

    PHLMONITOR pActiveMonitor = Desktop::focusState()->monitor();
    if (!pActiveMonitor || !pActiveMonitor->m_activeWorkspace) {
        logFile << "  No active workspace\n\n";
        return;
    }
    WORKSPACEID ws_id = pActiveMonitor->m_activeWorkspace->m_id;
    auto itWS = g_workspaceSettings.find(ws_id);
    int wsPartitions = (itWS != g_workspaceSettings.end()) ? itWS->second.partitions : -1;
    bool wsZen = (itWS != g_workspaceSettings.end()) ? itWS->second.zenActive : false;
    int64_t wsFit = (itWS != g_workspaceSettings.end()) ? itWS->second.focusFitMethod : -1;

    logFile << "  Workspace ID: " << ws_id 
            << " | Stored Partitions: " << wsPartitions
            << " | Zen: " << (wsZen ? "true" : "false")
            << " | FitMethod: " << wsFit << "\n";

    static const auto PFITMETHOD = CConfigValue<Config::INTEGER>("scrolling:focus_fit_method");
    logFile << "  Hyprland Configs -> focus_fit_method: " << *PFITMETHOD << "\n";

    PHLWORKSPACE pWorkspace = pActiveMonitor->m_activeWorkspace;
    if (pWorkspace && pWorkspace->m_space && pWorkspace->m_space->algorithm()) {
        auto pTiled = pWorkspace->m_space->algorithm()->tiledAlgo().get();
        Layout::Tiled::CScrollingAlgorithm* pScrolling = dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>(pTiled);
        if (pScrolling && pScrolling->m_scrollingData) {
            if (pScrolling->m_scrollingData->controller) {
                logFile << "  Tape Controller -> offset: " << pScrolling->m_scrollingData->controller->getOffset()
                        << " | usableArea: (" << pScrolling->usableArea().width << "x" << pScrolling->usableArea().height << ")\n";
            }
            logFile << "  Columns count: " << pScrolling->m_scrollingData->columns.size() << ":\n";
            for (size_t i = 0; i < pScrolling->m_scrollingData->columns.size(); ++i) {
                auto col = pScrolling->m_scrollingData->columns[i];
                if (col) {
                    logFile << "    Col " << i << ": width=" << col->getColumnWidth()
                            << " | targetDatas=" << col->targetDatas.size() << "\n";
                }
            }
        }
    }

    std::vector<PHLWINDOW> ws_windows;
    for (auto const& w : Desktop::windowState()->windows()) {
        if (w && w->m_isMapped && !w->isHidden() && w->m_workspace && w->m_workspace->m_id == ws_id && !w->m_isFloating) {
            ws_windows.push_back(w);
        }
    }

    std::sort(ws_windows.begin(), ws_windows.end(), [](PHLWINDOW a, PHLWINDOW b) {
        auto boxA = a->logicalBox();
        auto boxB = b->logicalBox();
        int xA = boxA ? boxA->x : 0;
        int xB = boxB ? boxB->x : 0;
        return xA < xB;
    });

    logFile << "  Windows (sorted left-to-right):\n";
    for (size_t i = 0; i < ws_windows.size(); ++i) {
        auto w = ws_windows[i];
        bool active = Desktop::focusState()->isWindowActive(w);
        auto box = w->logicalBox();
        logFile << "    - Window " << i << " | " << getAddressStr(w)
                << " | class: " << w->m_class
                << " | title: " << w->m_title << "\n"
                << "      pos: (" << (int)(box ? box->x : 0) << ", " << (int)(box ? box->y : 0) << ")"
                << " | size: (" << (int)(box ? box->width : 0) << ", " << (int)(box ? box->height : 0) << ")"
                << (active ? " [FOCUSED]" : "")
                << "\n";
    }
    logFile << "\n";
}

static SP<Layout::ITarget> getTargetForWindow(PHLWINDOW pWindow) {
    if (!pWindow || !pWindow->m_workspace) return nullptr;
    auto space = pWindow->m_workspace->m_space;
    if (!space) return nullptr;
    for (auto const& wt : space->targets()) {
        auto t = wt.lock();
        if (t && t->window() == pWindow) return t;
    }
    return nullptr;
}

static bool isZenModeGlobal() {
    return g_zenModeGlobalConfig ? g_zenModeGlobalConfig->value() : false;
}

static bool isCenterModeGlobal() {
    return g_centerModeGlobalConfig ? g_centerModeGlobalConfig->value() : false;
}

static SWorkspaceSettings& getWorkspaceSettings(WORKSPACEID ws_id) {
    if (g_workspaceSettings.find(ws_id) == g_workspaceSettings.end()) {
        SWorkspaceSettings settings;
        settings.zenActive = false;
        settings.focusFitMethod = 1; // 1 = Grid Partition Mode (Hyprland fitCol)
        settings.partitions = 2;
        g_workspaceSettings[ws_id] = settings;
    }
    return g_workspaceSettings[ws_id];
}

static PHLWORKSPACE getWorkspaceByID(WORKSPACEID id) {
    if (id == -1) return nullptr;
    for (auto const& w : State::workspaceState()->workspacesCopy()) {
        if (w && w->m_id == id) return w;
    }
    return nullptr;
}

static void syncWorkspaceZenOpacity(WORKSPACEID ws_id, bool zenActive) {
    for (auto const& w : Desktop::windowState()->windows()) {
        if (w && w->m_isMapped && !w->isHidden() && w->m_workspace && w->m_workspace->m_id == ws_id && !w->m_isFloating) {
            bool isActive = Desktop::focusState()->isWindowActive(w);
            float targetAlpha = (zenActive && !isActive) ? 0.0f : 1.0f;
            
            if (w->alpha(Desktop::View::WINDOW_ALPHA_ACTIVE)) {
                w->alpha(Desktop::View::WINDOW_ALPHA_ACTIVE)->setValueAndWarp(targetAlpha);
            }
        }
    }
}

// Align viewport tape offset without mutating column widths!
static void alignActiveColumn(Layout::Tiled::CScrollingAlgorithm* pScrolling, SP<Layout::Tiled::SColumnData> pColumn, int64_t fit_method, WORKSPACEID ws_id, bool forceInstant = false) {
    if (!pScrolling || !pScrolling->m_scrollingData || !pScrolling->m_scrollingData->controller) return;

    // Always keep Hyprland's native focus_fit_method in sync with fit_method (1 = Grid, 0 = Center):
    setConfigValueInt("scrolling:focus_fit_method", fit_method);

    CBox usableArea = pScrolling->usableArea();

    if (fit_method == 0) {
        // 0 = Center mode (Zen Mode / Focused Column Centering): center focused column squarely
        if (pColumn) {
            int active_idx = pScrolling->m_scrollingData->idx(pColumn);
            pScrolling->m_scrollingData->controller->centerStrip(active_idx, usableArea);
        }
    } else {
        // 1 = Grid mode: calculate tape offset according to partition grid
        double totalWidth = 0.0;
        for (auto const& col : pScrolling->m_scrollingData->columns) {
            if (col) {
                totalWidth += col->getColumnWidth();
            }
        }

        if (totalWidth > 0.0 && totalWidth < 0.999) {
            // Entire tape group fits on screen -> center the whole group with equal outer margins
            double totalPixelWidth = totalWidth * usableArea.width;
            double marginPixels = (usableArea.width - totalPixelWidth) / 2.0;
            pScrolling->m_scrollingData->controller->setOffset(-marginPixels);
        } else if (pColumn) {
            int active_idx = pScrolling->m_scrollingData->idx(pColumn);
            double col_width = pColumn->getColumnWidth();
            if (col_width <= 0.0) col_width = 0.5;
            int P = std::max(1, (int)std::round(1.0 / col_width));
            int total_cols = (int)pScrolling->m_scrollingData->columns.size();

            auto& wsSettings = getWorkspaceSettings(ws_id);
            int start_idx = wsSettings.viewportStartIdx;

            // Anchor-Relative / Continuous Viewport Clamping:
            // "Focus is what user sees":
            // - If active_idx is ALREADY within the visible viewport [start_idx, start_idx + P - 1], KEEP IT. ZERO CAMERA DRIFT.
            // - If active_idx moved to the right (>= start_idx + P), slide viewport right so active_idx is the rightmost column.
            // - If active_idx moved to the left (< start_idx), slide viewport left so active_idx is the leftmost column.
            if (active_idx >= start_idx + P) {
                start_idx = active_idx - P + 1;
            } else if (active_idx < start_idx) {
                start_idx = active_idx;
            }

            int max_start = std::max(0, total_cols - P);
            if (start_idx > max_start) start_idx = max_start;
            if (start_idx < 0) start_idx = 0;

            wsSettings.viewportStartIdx = start_idx;

            double target_offset = pScrolling->m_scrollingData->controller->calculateStripStart(start_idx, usableArea);
            pScrolling->m_scrollingData->controller->setOffset(target_offset);
        }
    }

    // forceInstant=false allows smooth sliding animations so the user maintains
    // clear spatial awareness when scrolling left and right across the tape.
    pScrolling->m_scrollingData->recalculate(forceInstant);
}


static SP<Layout::Tiled::SColumnData> findColumnForWindow(Layout::Tiled::CScrollingAlgorithm* pScrolling, PHLWINDOW pWindow) {
    if (!pScrolling || !pScrolling->m_scrollingData || !pWindow) return nullptr;
    for (auto const& col : pScrolling->m_scrollingData->columns) {
        if (!col) continue;
        for (auto const& td : col->targetDatas) {
            if (td && td->target) {
                auto t = td->target.lock();
                if (t && t->window() == pWindow) return col;
            }
        }
    }
    return nullptr;
}

// Re-align viewport tape for a workspace without overwriting existing column widths or scrolling:column_width
static void refreshWorkspaceTape(WORKSPACEID ws_id, PHLWINDOW pExplicitTargetWindow = nullptr, bool forceInstant = false) {
    if (ws_id == -1) return;
    auto& settings = getWorkspaceSettings(ws_id);

    PHLWORKSPACE pWorkspace = getWorkspaceByID(ws_id);
    if (!pWorkspace || !pWorkspace->m_space || !pWorkspace->m_space->algorithm()) return;

    auto pTiled = pWorkspace->m_space->algorithm()->tiledAlgo().get();
    Layout::Tiled::CScrollingAlgorithm* pScrolling = dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>(pTiled);
    if (!pScrolling || !pScrolling->m_scrollingData) return;

    // Find active window or first visible window on workspace
    PHLWINDOW pTargetWindow = pExplicitTargetWindow;
    if (!pTargetWindow) {
        for (auto const& w : Desktop::windowState()->windows()) {
            if (w && w->m_workspace && w->m_workspace->m_id == ws_id && !w->isHidden() && w->m_isMapped && !w->m_isFloating) {
                if (Desktop::focusState()->isWindowActive(w)) {
                    pTargetWindow = w;
                    break;
                }
                if (!pTargetWindow) pTargetWindow = w;
            }
        }
    }

    int64_t current_fit = settings.zenActive ? 0 : settings.focusFitMethod;

    SP<Layout::Tiled::SColumnData> pColumn = nullptr;
    if (pTargetWindow) {
        pColumn = findColumnForWindow(pScrolling, pTargetWindow);
    }

    alignActiveColumn(pScrolling, pColumn, current_fit, ws_id, forceInstant);

    auto pMonitor = pWorkspace->m_monitor.lock();
    if (pMonitor) {
        g_layoutManager->recalculateMonitor(pMonitor);
        if (g_pHyprRenderer) {
            g_pHyprRenderer->damageMonitor(pMonitor);
        }
    }
}

// Explicit dispatcher command: re-grids current workspace columns to requested partition fractions
SDispatchResult handlePartitions(std::string args) {
    logState("Dispatcher: partitions " + args);
    int partitions = 2;
    try {
        partitions = std::stoi(args);
    } catch (...) {
        partitions = 2;
    }
    if (partitions < 1) partitions = 1;
    if (partitions > 4) partitions = 4;

    PHLMONITOR pActiveMonitor = Desktop::focusState()->monitor();
    if (!pActiveMonitor || !pActiveMonitor->m_activeWorkspace) {
        return {false, false, "No active workspace found"};
    }

    WORKSPACEID activeWS = pActiveMonitor->m_activeWorkspace->m_id;
    getWorkspaceSettings(activeWS).partitions = partitions;
    getWorkspaceSettings(activeWS).focusFitMethod = 1;
    setConfigValueInt("scrolling:focus_fit_method", 1);

    double width = (partitions > 1) ? (1.0 / partitions) : 1.0;

    // Update global config value ONLY when user explicitly dispatches scrollingplus:partitions
    auto reply = Config::mgr()->getConfigValue("scrolling:column_width");
    if (reply.dataptr && reply.type) {
        if (*reply.type == typeid(float)) {
            *(float*)(*reply.dataptr) = (float)width;
        } else if (*reply.type == typeid(double)) {
            *(double*)(*reply.dataptr) = width;
        }
    }

    setConfigValueBool("scrolling:fullscreen_on_one_column", partitions == 1);

    PHLWORKSPACE pWorkspace = getWorkspaceByID(activeWS);
    if (pWorkspace && pWorkspace->m_space && pWorkspace->m_space->algorithm()) {
        auto pTiled = pWorkspace->m_space->algorithm()->tiledAlgo().get();
        Layout::Tiled::CScrollingAlgorithm* pScrolling = dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>(pTiled);
        if (pScrolling && pScrolling->m_scrollingData) {
            // Apply requested partition width to existing columns ONLY when dispatcher is explicitly invoked
            for (auto const& pColumn : pScrolling->m_scrollingData->columns) {
                if (pColumn) {
                    pColumn->setColumnWidth((float)width);
                }
            }
        }
    }

    refreshWorkspaceTape(activeWS);
    return {};
}

SDispatchResult handleToggleZen(std::string args) {
    logState("Dispatcher: togglezen " + args);

    PHLMONITOR pActiveMonitor = Desktop::focusState()->monitor();
    if (!pActiveMonitor || !pActiveMonitor->m_activeWorkspace) {
        return {false, false, "No active workspace found"};
    }
    WORKSPACEID activeWS = pActiveMonitor->m_activeWorkspace->m_id;

    g_zenModeActive = !g_zenModeActive;
    if (!isZenModeGlobal()) {
        getWorkspaceSettings(activeWS).zenActive = g_zenModeActive;
    }

    int64_t target_fit_method = g_zenModeActive ? 1 : (isCenterModeGlobal() ? g_globalFocusFitMethod : getWorkspaceSettings(activeWS).focusFitMethod);
    setConfigValueInt("scrolling:focus_fit_method", target_fit_method);

    std::string msg = g_zenModeActive ? "Zen Mode: ENABLED" : "Zen Mode: DISABLED";
    logState(msg);

    syncWorkspaceZenOpacity(activeWS, g_zenModeActive);
    refreshWorkspaceTape(activeWS);
    return {};
}

SDispatchResult handleToggleCenter(std::string) {
    logState("Dispatcher: togglecenter");

    PHLMONITOR pActiveMonitor = Desktop::focusState()->monitor();
    if (!pActiveMonitor || !pActiveMonitor->m_activeWorkspace) {
        return {false, false, "No active workspace found"};
    }
    WORKSPACEID activeWS = pActiveMonitor->m_activeWorkspace->m_id;

    auto& settings = getWorkspaceSettings(activeWS);
    int64_t new_fit = (settings.focusFitMethod == 0) ? 1 : 0;
    settings.focusFitMethod = new_fit;

    if (!g_zenModeActive) {
        setConfigValueInt("scrolling:focus_fit_method", new_fit);
    }

    std::string msg = (new_fit == 0) ? "Center Mode: ENABLED" : "Center Mode: DISABLED";
    logState(msg);

    refreshWorkspaceTape(activeWS);
    return {};
}

SDispatchResult handleFocusLeft(std::string) {
    logState("Dispatcher: focusleft");
    PHLMONITOR pActiveMonitor = Desktop::focusState()->monitor();
    if (!pActiveMonitor || !pActiveMonitor->m_activeWorkspace) return {};

    PHLWORKSPACE pWorkspace = pActiveMonitor->m_activeWorkspace;
    if (!pWorkspace || !pWorkspace->m_space || !pWorkspace->m_space->algorithm()) return {};

    auto pTiled = pWorkspace->m_space->algorithm()->tiledAlgo().get();
    Layout::Tiled::CScrollingAlgorithm* pScrolling = dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>(pTiled);
    if (!pScrolling || !pScrolling->m_scrollingData) return {};

    PHLWINDOW pActiveWindow = Desktop::focusState()->window();
    if (!pActiveWindow) return {};

    auto pColumn = findColumnForWindow(pScrolling, pActiveWindow);
    if (!pColumn) return {};

    int current_idx = pScrolling->m_scrollingData->idx(pColumn);
    if (current_idx <= 0) {
        logState("FocusLeft: Boundary reached at col 0 - no looping");
        return {};
    }

    auto targetCol = pScrolling->m_scrollingData->columns[current_idx - 1];
    PHLWINDOW targetWin = nullptr;
    if (targetCol) {
        for (auto const& td : targetCol->targetDatas) {
            if (td && td->target) {
                auto t = td->target.lock();
                if (t && t->window()) {
                    targetWin = t->window();
                    break;
                }
            }
        }
    }

    if (targetWin) {
        logState("FocusLeft: Moving focus to column " + std::to_string(current_idx - 1) + " window " + getAddressStr(targetWin));
        Desktop::focusState()->fullWindowFocus(targetWin, Desktop::FOCUS_REASON_KEYBIND);
        refreshWorkspaceTape(pWorkspace->m_id, targetWin);
    }

    return {};
}

SDispatchResult handleFocusRight(std::string) {
    logState("Dispatcher: focusright");
    PHLMONITOR pActiveMonitor = Desktop::focusState()->monitor();
    if (!pActiveMonitor || !pActiveMonitor->m_activeWorkspace) return {};

    PHLWORKSPACE pWorkspace = pActiveMonitor->m_activeWorkspace;
    if (!pWorkspace || !pWorkspace->m_space || !pWorkspace->m_space->algorithm()) return {};

    auto pTiled = pWorkspace->m_space->algorithm()->tiledAlgo().get();
    Layout::Tiled::CScrollingAlgorithm* pScrolling = dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>(pTiled);
    if (!pScrolling || !pScrolling->m_scrollingData) return {};

    PHLWINDOW pActiveWindow = Desktop::focusState()->window();
    if (!pActiveWindow) return {};

    auto pColumn = findColumnForWindow(pScrolling, pActiveWindow);
    if (!pColumn) return {};

    int current_idx = pScrolling->m_scrollingData->idx(pColumn);
    int max_idx = (int)pScrolling->m_scrollingData->columns.size() - 1;

    if (current_idx >= max_idx) {
        logState("FocusRight: Boundary reached at last col - no looping");
        return {};
    }

    auto targetCol = pScrolling->m_scrollingData->columns[current_idx + 1];
    PHLWINDOW targetWin = nullptr;
    if (targetCol) {
        for (auto const& td : targetCol->targetDatas) {
            if (td && td->target) {
                auto t = td->target.lock();
                if (t && t->window()) {
                    targetWin = t->window();
                    break;
                }
            }
        }
    }

    if (targetWin) {
        logState("FocusRight: Moving focus to column " + std::to_string(current_idx + 1) + " window " + getAddressStr(targetWin));
        Desktop::focusState()->fullWindowFocus(targetWin, Desktop::FOCUS_REASON_KEYBIND);
        refreshWorkspaceTape(pWorkspace->m_id, targetWin);
    }

    return {};
}

SDispatchResult handleMoveLeft(std::string args) {
    logState("Dispatcher: moveleft " + args);
    PHLMONITOR pActiveMonitor = Desktop::focusState()->monitor();
    if (!pActiveMonitor || !pActiveMonitor->m_activeWorkspace) return {};

    PHLWORKSPACE pWorkspace = pActiveMonitor->m_activeWorkspace;
    if (!pWorkspace || !pWorkspace->m_space || !pWorkspace->m_space->algorithm()) return {};

    auto pTiled = pWorkspace->m_space->algorithm()->tiledAlgo().get();
    Layout::Tiled::CScrollingAlgorithm* pScrolling = dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>(pTiled);
    if (!pScrolling || !pScrolling->m_scrollingData) return {};

    PHLWINDOW pActiveWindow = nullptr;
    std::string clean = args;
    if (clean.rfind("address:", 0) == 0) clean = clean.substr(8);
    while (!clean.empty() && isspace(clean.front())) clean.erase(clean.begin());
    while (!clean.empty() && isspace(clean.back())) clean.pop_back();

    if (!clean.empty()) {
        for (auto const& wt : pWorkspace->m_space->targets()) {
            auto t = wt.lock();
            if (t && t->window() && getAddressStr(t->window()) == clean) {
                pActiveWindow = t->window();
                break;
            }
        }
    }
    if (!pActiveWindow) {
        pActiveWindow = Desktop::focusState()->window();
    }
    if (!pActiveWindow) return {};

    SP<Layout::ITarget> pTarget = getTargetForWindow(pActiveWindow);
    if (!pTarget) return {};

    auto pTargetData = pScrolling->dataFor(pTarget);
    if (!pTargetData || !pTargetData->column) return {};

    auto pColumn = pTargetData->column.lock();
    if (!pColumn) return {};

    int current_idx = pScrolling->m_scrollingData->idx(pColumn);
    if (current_idx <= 0) {
        logState("MoveLeft: Already at left boundary (col 0)");
        return {};
    }

    std::swap(pScrolling->m_scrollingData->columns[current_idx], pScrolling->m_scrollingData->columns[current_idx - 1]);
    if (pScrolling->m_scrollingData->controller && current_idx < (int)pScrolling->m_scrollingData->controller->stripCount()) {
        pScrolling->m_scrollingData->controller->swapStrips(current_idx, current_idx - 1);
    }

    bool isProgrammatic = !clean.empty();
    pScrolling->m_scrollingData->recalculate(isProgrammatic);
    refreshWorkspaceTape(pWorkspace->m_id, nullptr, isProgrammatic);
    return {};
}

SDispatchResult handleMoveRight(std::string args) {
    logState("Dispatcher: moveright " + args);
    PHLMONITOR pActiveMonitor = Desktop::focusState()->monitor();
    if (!pActiveMonitor || !pActiveMonitor->m_activeWorkspace) return {};

    PHLWORKSPACE pWorkspace = pActiveMonitor->m_activeWorkspace;
    if (!pWorkspace || !pWorkspace->m_space || !pWorkspace->m_space->algorithm()) return {};

    auto pTiled = pWorkspace->m_space->algorithm()->tiledAlgo().get();
    Layout::Tiled::CScrollingAlgorithm* pScrolling = dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>(pTiled);
    if (!pScrolling || !pScrolling->m_scrollingData) return {};

    PHLWINDOW pActiveWindow = nullptr;
    std::string clean = args;
    if (clean.rfind("address:", 0) == 0) clean = clean.substr(8);
    while (!clean.empty() && isspace(clean.front())) clean.erase(clean.begin());
    while (!clean.empty() && isspace(clean.back())) clean.pop_back();

    if (!clean.empty()) {
        for (auto const& wt : pWorkspace->m_space->targets()) {
            auto t = wt.lock();
            if (t && t->window() && getAddressStr(t->window()) == clean) {
                pActiveWindow = t->window();
                break;
            }
        }
    }
    if (!pActiveWindow) {
        pActiveWindow = Desktop::focusState()->window();
    }
    if (!pActiveWindow) return {};

    SP<Layout::ITarget> pTarget = getTargetForWindow(pActiveWindow);
    if (!pTarget) return {};

    auto pTargetData = pScrolling->dataFor(pTarget);
    if (!pTargetData || !pTargetData->column) return {};

    auto pColumn = pTargetData->column.lock();
    if (!pColumn) return {};

    int current_idx = pScrolling->m_scrollingData->idx(pColumn);
    int max_idx = (int)pScrolling->m_scrollingData->columns.size() - 1;

    if (current_idx >= max_idx) {
        logState("MoveRight: Already at rightmost column");
        return {};
    }

    std::swap(pScrolling->m_scrollingData->columns[current_idx], pScrolling->m_scrollingData->columns[current_idx + 1]);
    if (pScrolling->m_scrollingData->controller && current_idx + 1 < (int)pScrolling->m_scrollingData->controller->stripCount()) {
        pScrolling->m_scrollingData->controller->swapStrips(current_idx, current_idx + 1);
    }

    bool isProgrammatic = !clean.empty();
    pScrolling->m_scrollingData->recalculate(isProgrammatic);
    refreshWorkspaceTape(pWorkspace->m_id, nullptr, isProgrammatic);
    return {};
}

SDispatchResult handleToggleMaximize(std::string) {
    logState("Dispatcher: togglemaximize");
    PHLMONITOR pActiveMonitor = Desktop::focusState()->monitor();
    if (!pActiveMonitor || !pActiveMonitor->m_activeWorkspace) return {};

    PHLWORKSPACE pWorkspace = pActiveMonitor->m_activeWorkspace;
    if (!pWorkspace || !pWorkspace->m_space || !pWorkspace->m_space->algorithm()) return {};

    auto pTiled = pWorkspace->m_space->algorithm()->tiledAlgo().get();
    Layout::Tiled::CScrollingAlgorithm* pScrolling = dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>(pTiled);
    if (!pScrolling || !pScrolling->m_scrollingData) return {};

    PHLWINDOW pActiveWindow = Desktop::focusState()->window();
    if (!pActiveWindow) return {};

    auto pColumn = findColumnForWindow(pScrolling, pActiveWindow);
    if (!pColumn) return {};

    double currentWidth = pColumn->getColumnWidth();
    double targetWidth = (currentWidth > 0.8) ? 0.5 : 1.0;
    pColumn->setColumnWidth(targetWidth);

    refreshWorkspaceTape(pWorkspace->m_id, pActiveWindow, true);
    return {};
}







// Hyprland 0.56's Lua config engine never resolves plugin-registered
// (addDispatcherV2) dispatchers - hyprctl dispatch / raw IPC unconditionally
// routes through hl.dispatch() Lua evaluation when a Lua config is active,
// and that path never consults the dispatcher registry these are added to.
// Confirmed upstream gap (github.com/hyprwm/Hyprland discussion #14451);
// the maintainer's own stated workaround is to also expose each dispatcher
// as a plugin Lua function, callable as hl.plugin.scrollingplus.<name>(...).
// This wrapper adapts the existing SDispatchResult(std::string) handlers to
// that calling convention without duplicating their logic.
template <SDispatchResult (*Handler)(std::string)> static int luaDispatchWrapper(lua_State* L) {
    std::string args;
    int nargs = lua_gettop(L);
    for (int i = 1; i <= nargs; ++i) {
        if (i > 1) args += " ";
        if (lua_isinteger(L, i))
            args += std::to_string(lua_tointeger(L, i));
        else if (lua_isnumber(L, i))
            args += std::to_string(lua_tonumber(L, i));
        else if (lua_isstring(L, i))
            args += lua_tostring(L, i);
    }
    Handler(args);
    return 0;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;
    logState("PLUGIN_INIT called");

    const std::string COMPOSITOR_HASH = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (COMPOSITOR_HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(PHANDLE, "[hyprscrolling-plus] Mismatched headers! Can't proceed.",
                                     CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hyprscrolling-plus] Version mismatch");
    }

    // Register custom config values
    g_zenModeGlobalConfig = Hyprutils::Memory::makeShared<Config::Values::CBoolValue>("plugin:scrollingplus:zen_mode_global", "Whether zen mode is global across workspaces", false);
    g_centerModeGlobalConfig = Hyprutils::Memory::makeShared<Config::Values::CBoolValue>("plugin:scrollingplus:center_mode_global", "Whether center mode is global across workspaces", false);

    HyprlandAPI::addConfigValueV2(PHANDLE, g_zenModeGlobalConfig);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_centerModeGlobalConfig);

    setConfigValueInt("scrolling:focus_fit_method", 1);
    g_defaultFocusFitMethod = 1;
    g_globalFocusFitMethod = 1;

    PHLMONITOR pActiveMonitor = Desktop::focusState()->monitor();
    if (pActiveMonitor && pActiveMonitor->m_activeWorkspace) {
        g_lastWorkspaceID = pActiveMonitor->m_activeWorkspace->m_id;
    }

    // Register custom dispatchers (both colon and underscore versions for CLI compatibility)
    HyprlandAPI::addDispatcherV2(PHANDLE, "scrollingplus:partitions", handlePartitions);
    HyprlandAPI::addDispatcherV2(PHANDLE, "scrollingplus:togglecenter", handleToggleCenter);
    HyprlandAPI::addDispatcherV2(PHANDLE, "scrollingplus:togglezen", handleToggleZen);
    HyprlandAPI::addDispatcherV2(PHANDLE, "scrollingplus:focusleft", handleFocusLeft);
    HyprlandAPI::addDispatcherV2(PHANDLE, "scrollingplus:focusright", handleFocusRight);
    HyprlandAPI::addDispatcherV2(PHANDLE, "scrollingplus:moveleft", handleMoveLeft);
    HyprlandAPI::addDispatcherV2(PHANDLE, "scrollingplus:moveright", handleMoveRight);
    HyprlandAPI::addDispatcherV2(PHANDLE, "scrollingplus:togglemaximize", handleToggleMaximize);

    HyprlandAPI::addDispatcherV2(PHANDLE, "scrollingplus_partitions", handlePartitions);
    HyprlandAPI::addDispatcherV2(PHANDLE, "scrollingplus_togglecenter", handleToggleCenter);
    HyprlandAPI::addDispatcherV2(PHANDLE, "scrollingplus_togglezen", handleToggleZen);
    HyprlandAPI::addDispatcherV2(PHANDLE, "scrollingplus_focusleft", handleFocusLeft);
    HyprlandAPI::addDispatcherV2(PHANDLE, "scrollingplus_focusright", handleFocusRight);
    HyprlandAPI::addDispatcherV2(PHANDLE, "scrollingplus_moveleft", handleMoveLeft);
    HyprlandAPI::addDispatcherV2(PHANDLE, "scrollingplus_moveright", handleMoveRight);
    HyprlandAPI::addDispatcherV2(PHANDLE, "scrollingplus_togglemaximize", handleToggleMaximize);

    // Also register as Lua-callable plugin functions (hl.plugin.scrollingplus.<name>(...))
    HyprlandAPI::addLuaFunction(PHANDLE, "scrollingplus", "partitions", luaDispatchWrapper<handlePartitions>);
    HyprlandAPI::addLuaFunction(PHANDLE, "scrollingplus", "togglecenter", luaDispatchWrapper<handleToggleCenter>);
    HyprlandAPI::addLuaFunction(PHANDLE, "scrollingplus", "togglezen", luaDispatchWrapper<handleToggleZen>);
    HyprlandAPI::addLuaFunction(PHANDLE, "scrollingplus", "focusleft", luaDispatchWrapper<handleFocusLeft>);
    HyprlandAPI::addLuaFunction(PHANDLE, "scrollingplus", "focusright", luaDispatchWrapper<handleFocusRight>);
    HyprlandAPI::addLuaFunction(PHANDLE, "scrollingplus", "moveleft", luaDispatchWrapper<handleMoveLeft>);
    HyprlandAPI::addLuaFunction(PHANDLE, "scrollingplus", "moveright", luaDispatchWrapper<handleMoveRight>);
    HyprlandAPI::addLuaFunction(PHANDLE, "scrollingplus", "togglemaximize", luaDispatchWrapper<handleToggleMaximize>);




    // Register event listeners - NONE of these event listeners mutate column widths or scrolling:column_width!
    activeWindowListener = Event::bus()->m_events.window.active.listen([](PHLWINDOW w, auto /* reason */) {
        logState("Focus changed to " + getAddressStr(w));
        if (w && w->m_workspace) {
            WORKSPACEID ws_id = w->m_workspace->m_id;
            bool zenGlobal = isZenModeGlobal();
            bool isZenActive = zenGlobal ? g_zenModeActive : getWorkspaceSettings(ws_id).zenActive;
            
            if (isZenActive) {
                syncWorkspaceZenOpacity(ws_id, true);
            }
            refreshWorkspaceTape(ws_id, w);
        }
    });

    activeWorkspaceListener = Event::bus()->m_events.workspace.active.listen([](PHLWORKSPACE ws) {
        logState("Workspace changed to workspace " + std::to_string(ws ? ws->m_id : -1));

        WORKSPACEID newWS = ws ? ws->m_id : -1;
        if (newWS == g_lastWorkspaceID) return;

        bool zenGlobal = isZenModeGlobal();
        bool centerGlobal = isCenterModeGlobal();

        // 1. Save old workspace state
        if (g_lastWorkspaceID != -1) {
            if (!zenGlobal) {
                getWorkspaceSettings(g_lastWorkspaceID).zenActive = g_zenModeActive;
            }
            if (!centerGlobal) {
                bool isOldZenActive = zenGlobal ? g_zenModeActive : getWorkspaceSettings(g_lastWorkspaceID).zenActive;
                if (!isOldZenActive) {
                    static const auto PFITMETHOD = CConfigValue<Config::INTEGER>("scrolling:focus_fit_method");
                    getWorkspaceSettings(g_lastWorkspaceID).focusFitMethod = *PFITMETHOD;
                }
            }
        }

        // 2. Load and apply new workspace state
        if (newWS != -1) {
            auto& newSettings = getWorkspaceSettings(newWS);
            
            if (!zenGlobal) {
                g_zenModeActive = newSettings.zenActive;
            }

            syncWorkspaceZenOpacity(newWS, g_zenModeActive);

            int64_t target_fit_method = g_zenModeActive ? 0 : (centerGlobal ? g_globalFocusFitMethod : newSettings.focusFitMethod);
            setConfigValueInt("scrolling:focus_fit_method", target_fit_method);

            refreshWorkspaceTape(newWS);
        }

        g_lastWorkspaceID = newWS;
    });

    specialWorkspaceListener = Event::bus()->m_events.workspace.specialActive.listen([](PHLWORKSPACE ws, PHLMONITOR mon) {
        logState("Special workspace changed to " + std::to_string(ws ? ws->m_id : -1));
        if (ws) {
            refreshWorkspaceTape(ws->m_id);
        }
        if (mon && mon->m_activeWorkspace) {
            refreshWorkspaceTape(mon->m_activeWorkspace->m_id);
        }
    });

    fullscreenListener = Event::bus()->m_events.window.fullscreen.listen([](PHLWINDOW w) {
        logState("Window fullscreen state changed on " + getAddressStr(w));
        
        if (w && !Fullscreen::controller()->isFullscreen(w) && w->m_workspace) {
            if (pFullscreenRestoreTimer) {
                pFullscreenRestoreTimer->cancel();
                pFullscreenRestoreTimer.reset();
            }

            pFullscreenRestoreTimer = Hyprutils::Memory::makeShared<CEventLoopTimer>(std::chrono::milliseconds(100), [w](SP<CEventLoopTimer>, void*) {
                if (w && w->m_workspace) {
                    refreshWorkspaceTape(w->m_workspace->m_id);
                }
                pFullscreenRestoreTimer.reset();
            }, nullptr);

            g_pEventLoopManager->addTimer(pFullscreenRestoreTimer);
        }
    });

    windowOpenListener = Event::bus()->m_events.window.open.listen([](PHLWINDOW w) {
        logState("Window opened: " + getAddressStr(w));
        if (w) {
            w->positionAnimation()->warp(true);
            w->sizeAnimation()->warp(true);
            w->finishAnimation();
        }
        if (w && w->m_workspace) {
            refreshWorkspaceTape(w->m_workspace->m_id, w, true);
        }
    });

    windowCloseListener = Event::bus()->m_events.window.close.listen([](PHLWINDOW w) {
        logState("Window closed: " + getAddressStr(w));
        if (w && w->m_workspace) {
            refreshWorkspaceTape(w->m_workspace->m_id);
        } else {
            PHLMONITOR pMon = Desktop::focusState()->monitor();
            if (pMon && pMon->m_activeWorkspace) {
                refreshWorkspaceTape(pMon->m_activeWorkspace->m_id);
            }
        }
    });

    windowMoveToWorkspaceListener = Event::bus()->m_events.window.moveToWorkspace.listen([](PHLWINDOW w, PHLWORKSPACE ws) {
        logState("Window moved to workspace: " + getAddressStr(w));
        if (w) {
            w->positionAnimation()->warp(true);
            w->sizeAnimation()->warp(true);
            w->finishAnimation();
        }
        if (w && w->m_workspace) {
            refreshWorkspaceTape(w->m_workspace->m_id);
        }
        if (ws) {
            refreshWorkspaceTape(ws->m_id);
        }
        PHLMONITOR pMon = Desktop::focusState()->monitor();
        if (pMon && pMon->m_activeWorkspace) {
            refreshWorkspaceTape(pMon->m_activeWorkspace->m_id);
        }
    });

    return {"hyprscrolling-plus", "Grid partitions and centering patches for scrolling layout", "Antigravity", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    logState("PLUGIN_EXIT called");
    HyprlandAPI::removeDispatcher(PHANDLE, "scrollingplus:partitions");
    HyprlandAPI::removeDispatcher(PHANDLE, "scrollingplus:togglecenter");
    HyprlandAPI::removeDispatcher(PHANDLE, "scrollingplus:togglezen");
    HyprlandAPI::removeDispatcher(PHANDLE, "scrollingplus:focusleft");
    HyprlandAPI::removeDispatcher(PHANDLE, "scrollingplus:focusright");
    HyprlandAPI::removeDispatcher(PHANDLE, "scrollingplus:moveleft");
    HyprlandAPI::removeDispatcher(PHANDLE, "scrollingplus:moveright");
    HyprlandAPI::removeDispatcher(PHANDLE, "scrollingplus:togglemaximize");

    HyprlandAPI::removeDispatcher(PHANDLE, "scrollingplus_partitions");
    HyprlandAPI::removeDispatcher(PHANDLE, "scrollingplus_togglecenter");
    HyprlandAPI::removeDispatcher(PHANDLE, "scrollingplus_togglezen");
    HyprlandAPI::removeDispatcher(PHANDLE, "scrollingplus_focusleft");
    HyprlandAPI::removeDispatcher(PHANDLE, "scrollingplus_focusright");
    HyprlandAPI::removeDispatcher(PHANDLE, "scrollingplus_moveleft");
    HyprlandAPI::removeDispatcher(PHANDLE, "scrollingplus_moveright");
    HyprlandAPI::removeDispatcher(PHANDLE, "scrollingplus_togglemaximize");

    HyprlandAPI::removeLuaFunction(PHANDLE, "scrollingplus", "partitions");
    HyprlandAPI::removeLuaFunction(PHANDLE, "scrollingplus", "togglecenter");
    HyprlandAPI::removeLuaFunction(PHANDLE, "scrollingplus", "togglezen");
    HyprlandAPI::removeLuaFunction(PHANDLE, "scrollingplus", "focusleft");
    HyprlandAPI::removeLuaFunction(PHANDLE, "scrollingplus", "focusright");
    HyprlandAPI::removeLuaFunction(PHANDLE, "scrollingplus", "moveleft");
    HyprlandAPI::removeLuaFunction(PHANDLE, "scrollingplus", "moveright");
    HyprlandAPI::removeLuaFunction(PHANDLE, "scrollingplus", "togglemaximize");



    activeWindowListener.reset();
    activeWorkspaceListener.reset();
    specialWorkspaceListener.reset();
    fullscreenListener.reset();
    windowOpenListener.reset();
    windowCloseListener.reset();
    windowMoveToWorkspaceListener.reset();

    if (pFullscreenRestoreTimer) {
        pFullscreenRestoreTimer->cancel();
        pFullscreenRestoreTimer.reset();
    }

    g_zenModeGlobalConfig.reset();
    g_centerModeGlobalConfig.reset();
}
