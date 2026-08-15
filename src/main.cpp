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
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollingAlgorithm.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/render/Renderer.hpp>
#undef private


static HANDLE PHANDLE = nullptr;

static Hyprutils::Signal::CHyprSignalListener activeWindowListener;
static Hyprutils::Signal::CHyprSignalListener activeWorkspaceListener;
static Hyprutils::Signal::CHyprSignalListener fullscreenListener;
static Hyprutils::Signal::CHyprSignalListener windowOpenListener;
static Hyprutils::Signal::CHyprSignalListener windowCloseListener;
static SP<CEventLoopTimer> pFullscreenRestoreTimer = nullptr;

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
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

static void alignActiveColumn(Layout::Tiled::CScrollingAlgorithm* pScrolling, SP<Layout::Tiled::SColumnData> pColumn, int64_t fit_method) {
    if (!pScrolling || !pColumn || !pScrolling->m_scrollingData || !pScrolling->m_scrollingData->controller) return;

    int active_idx = pScrolling->m_scrollingData->idx(pColumn);
    CBox usableArea = pScrolling->usableArea();

    if (fit_method == 0) {
        // Center mode: center active column directly on the tape controller
        pScrolling->m_scrollingData->controller->centerStrip(active_idx, usableArea);
    } else {
        // Fit mode: align to the page grid boundary
        double col_width = 0.5;
        auto reply = Config::mgr()->getConfigValue("scrolling:column_width");
        if (reply.dataptr && reply.type) {
            if (*reply.type == typeid(float)) {
                col_width = *(float*)(*reply.dataptr);
            } else if (*reply.type == typeid(double)) {
                col_width = *(double*)(*reply.dataptr);
            }
        }
        int P = std::max(1, (int)std::round(1.0 / col_width));

        int start_idx = (active_idx / P) * P;
        double target_offset = pScrolling->m_scrollingData->controller->calculateStripStart(start_idx, usableArea);
        pScrolling->m_scrollingData->controller->setOffset(target_offset);
    }

    pScrolling->m_scrollingData->recalculate(true);
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
    logFile << "  Active Workspace: " << ws_id << "\n";

    std::vector<PHLWINDOW> ws_windows;
    for (auto const& w : Desktop::windowState()->windows()) {
        if (w && w->m_workspace && w->m_workspace->m_id == ws_id && !w->isHidden() && w->m_isMapped && !w->m_isFloating) {
            ws_windows.push_back(w);
        }
    }

    std::sort(ws_windows.begin(), ws_windows.end(), [](const PHLWINDOW& a, const PHLWINDOW& b) {
        auto boxA = a->logicalBox(); auto boxB = b->logicalBox();
        return (boxA ? boxA->x : 0) < (boxB ? boxB->x : 0);
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

static int64_t getConfigValueInt(const std::string& name, int64_t defaultValue) {
    auto reply = Config::mgr()->getConfigValue(name);
    if (reply.dataptr && reply.type) {
        if (*reply.type == typeid(int)) {
            return *(int*)(*reply.dataptr);
        } else if (*reply.type == typeid(int64_t)) {
            return *(int64_t*)(*reply.dataptr);
        } else if (*reply.type == typeid(long)) {
            return *(long*)(*reply.dataptr);
        } else if (*reply.type == typeid(long long)) {
            return *(long long*)(*reply.dataptr);
        } else if (*reply.type == typeid(bool)) {
            return *(bool*)(*reply.dataptr);
        }
    }
    return defaultValue;
}

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


#include <hyprland/src/config/values/types/BoolValue.hpp>

#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>

static bool g_zenModeActive = false;
static int64_t g_globalFocusFitMethod = 1;

static SP<Config::Values::CBoolValue> g_zenModeGlobalConfig;
static SP<Config::Values::CBoolValue> g_centerModeGlobalConfig;

struct SWorkspaceSettings {
    bool zenActive = false;
    int64_t focusFitMethod = 1;
};

static std::unordered_map<WORKSPACEID, SWorkspaceSettings> g_workspaceSettings;
static WORKSPACEID g_lastWorkspaceID = -1;

static int64_t g_defaultFocusFitMethod = 1;

static bool isZenModeGlobal() {
    return g_zenModeGlobalConfig ? g_zenModeGlobalConfig->value() : false;
}

static bool isCenterModeGlobal() {
    return g_centerModeGlobalConfig ? g_centerModeGlobalConfig->value() : false;
}

static SWorkspaceSettings& getWorkspaceSettings(WORKSPACEID ws_id) {
    auto it = g_workspaceSettings.find(ws_id);
    if (it == g_workspaceSettings.end()) {
        SWorkspaceSettings settings;
        settings.zenActive = false;
        settings.focusFitMethod = g_defaultFocusFitMethod;
        g_workspaceSettings[ws_id] = settings;
    }
    return g_workspaceSettings[ws_id];
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

SDispatchResult handleToggleZen(std::string args) {
    logState("Dispatcher: togglezen " + args);

    PHLMONITOR pActiveMonitor = Desktop::focusState()->monitor();
    if (!pActiveMonitor || !pActiveMonitor->m_activeWorkspace) {
        return {false, false, "No active workspace found"};
    }
    WORKSPACEID activeWS = pActiveMonitor->m_activeWorkspace->m_id;

    // Toggle the tracking state for this workspace
    g_zenModeActive = !g_zenModeActive;
    if (!isZenModeGlobal()) {
        getWorkspaceSettings(activeWS).zenActive = g_zenModeActive;
    }

    logState("handleToggleZen: toggled activeWS=" + std::to_string(activeWS) + 
             " to active=" + std::to_string(g_zenModeActive));

    // Update fit method / centering
    int64_t target_fit_method = 1;
    if (g_zenModeActive) {
        target_fit_method = 0;
    } else {
        target_fit_method = isCenterModeGlobal() ? g_globalFocusFitMethod : getWorkspaceSettings(activeWS).focusFitMethod;
    }
    setConfigValueInt("scrolling:focus_fit_method", target_fit_method);

    // Apply the opacity changes directly and instantly
    syncWorkspaceZenOpacity(activeWS, g_zenModeActive);

    // Center active column if Zen mode is turned ON
    if (g_zenModeActive) {
        if (pActiveMonitor->m_activeWorkspace->m_space && pActiveMonitor->m_activeWorkspace->m_space->algorithm()) {
            auto pTiled = pActiveMonitor->m_activeWorkspace->m_space->algorithm()->tiledAlgo().get();
            Layout::Tiled::CScrollingAlgorithm* pScrolling = dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>(pTiled);
            if (pScrolling) {
                PHLWINDOW pActiveWindow = Desktop::focusState()->window();
                if (pActiveWindow) {
                    SP<Layout::ITarget> pTarget = getTargetForWindow(pActiveWindow);
                    if (pTarget) {
                        auto pTargetData = pScrolling->dataFor(pTarget);
                        if (pTargetData && pTargetData->column) {
                            auto pColumn = pTargetData->column.lock();
                            if (pColumn) {
                                alignActiveColumn(pScrolling, pColumn, 0);
                            }
                        }
                    }
                }
            }
        }
    }

    g_layoutManager->recalculateMonitor(pActiveMonitor);
    if (g_pHyprRenderer) {
        g_pHyprRenderer->damageMonitor(pActiveMonitor);
    }

    return {};
}

SDispatchResult handlePartitions(std::string args) {
    logState("Dispatcher: partitions " + args);
    int partitions = 2;
    try {
        partitions = std::stoi(args);
    } catch (...) {
        partitions = 2;
    }

    double width = 1.0;
    if (partitions > 1) {
        width = (1.0 / partitions) + 0.0001;
    }

    // Get current active monitor/workspace
    PHLMONITOR pActiveMonitor = Desktop::focusState()->monitor();
    if (!pActiveMonitor || !pActiveMonitor->m_activeWorkspace) {
        return {false, false, "No active workspace found"};
    }

    // Try to get CScrollingAlgorithm
    if (!pActiveMonitor->m_activeWorkspace->m_space || !pActiveMonitor->m_activeWorkspace->m_space->algorithm()) {
        return {};
    }

    auto pTiled = pActiveMonitor->m_activeWorkspace->m_space->algorithm()->tiledAlgo().get();
    Layout::Tiled::CScrollingAlgorithm* pScrolling = dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>(pTiled);
    if (!pScrolling) {
        return {}; // Not using scrolling layout
    }

    // Read current focus fit method
    int64_t current_fit = getConfigValueInt("scrolling:focus_fit_method", 1);

    // Apply column width to all columns in the scrolling data
    for (auto const& pColumn : pScrolling->m_scrollingData->columns) {
        if (pColumn) {
            pColumn->setColumnWidth((float)width);
        }
    }

    // Also update the config value so new windows get this width
    auto reply = Config::mgr()->getConfigValue("scrolling:column_width");
    if (reply.dataptr && reply.type) {
        if (*reply.type == typeid(float)) {
            *(float*)(*reply.dataptr) = (float)width;
        } else if (*reply.type == typeid(double)) {
            *(double*)(*reply.dataptr) = width;
        }
    }

    // Align the active column according to the focus fit method
    PHLWINDOW pActiveWindow = Desktop::focusState()->window();
    if (pActiveWindow) {
        SP<Layout::ITarget> pTarget = getTargetForWindow(pActiveWindow);
        if (pTarget) {
            auto pTargetData = pScrolling->dataFor(pTarget);
            if (pTargetData && pTargetData->column) {
                auto pColumn = pTargetData->column.lock();
                if (pColumn) {
                    alignActiveColumn(pScrolling, pColumn, current_fit);
                }
            }
        }
    }

    // Recalculate layout to apply changes
    g_layoutManager->recalculateMonitor(pActiveMonitor);

    return {};
}

SDispatchResult handleToggleCenter(std::string) {
    logState("Dispatcher: togglecenter");
    
    // Get active monitor/workspace
    PHLMONITOR pActiveMonitor = Desktop::focusState()->monitor();
    if (!pActiveMonitor || !pActiveMonitor->m_activeWorkspace) {
        return {false, false, "No active workspace found"};
    }
    WORKSPACEID activeWS = pActiveMonitor->m_activeWorkspace->m_id;

    // Read current focus fit method
    int64_t current_fit = isCenterModeGlobal() ? g_globalFocusFitMethod : getWorkspaceSettings(activeWS).focusFitMethod;
    int64_t new_fit = (current_fit == 0) ? 1 : 0;

    if (isCenterModeGlobal()) {
        g_globalFocusFitMethod = new_fit;
    } else {
        getWorkspaceSettings(activeWS).focusFitMethod = new_fit;
    }

    // Apply focus fit method config (if zen is not active)
    if (!g_zenModeActive) {
        setConfigValueInt("scrolling:focus_fit_method", new_fit);
    }

    // Try to get CScrollingAlgorithm
    if (!pActiveMonitor->m_activeWorkspace->m_space || !pActiveMonitor->m_activeWorkspace->m_space->algorithm()) {
        return {};
    }

    auto pTiled = pActiveMonitor->m_activeWorkspace->m_space->algorithm()->tiledAlgo().get();
    Layout::Tiled::CScrollingAlgorithm* pScrolling = dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>(pTiled);
    if (!pScrolling) {
        return {}; // Not using scrolling layout
    }

    // Get active window
    PHLWINDOW pActiveWindow = Desktop::focusState()->window();
    if (!pActiveWindow) {
        return {};
    }

    // Find ITarget for active window
    SP<Layout::ITarget> pTarget = getTargetForWindow(pActiveWindow);
    if (!pTarget) {
        return {};
    }

    auto pTargetData = pScrolling->dataFor(pTarget);
    if (!pTargetData || !pTargetData->column) {
        return {};
    }

    auto pColumn = pTargetData->column.lock();
    if (!pColumn) {
        return {};
    }

    alignActiveColumn(pScrolling, pColumn, new_fit);

    // Recalculate layout and damage monitor renderer
    g_layoutManager->recalculateMonitor(pActiveMonitor);
    if (g_pHyprRenderer) {
        g_pHyprRenderer->damageMonitor(pActiveMonitor);
    }

    return {};
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

    // Register our custom config values
    g_zenModeGlobalConfig = Hyprutils::Memory::makeShared<Config::Values::CBoolValue>("plugin:scrollingplus:zen_mode_global", "Whether zen mode is global across workspaces", false);
    g_centerModeGlobalConfig = Hyprutils::Memory::makeShared<Config::Values::CBoolValue>("plugin:scrollingplus:center_mode_global", "Whether center mode is global across workspaces", false);

    HyprlandAPI::addConfigValueV2(PHANDLE, g_zenModeGlobalConfig);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_centerModeGlobalConfig);


    g_defaultFocusFitMethod = getConfigValueInt("scrolling:focus_fit_method", 1);
    g_globalFocusFitMethod = g_defaultFocusFitMethod;

    PHLMONITOR pActiveMonitor = Desktop::focusState()->monitor();
    if (pActiveMonitor && pActiveMonitor->m_activeWorkspace) {
        g_lastWorkspaceID = pActiveMonitor->m_activeWorkspace->m_id;
    }

    // Register custom dispatchers (both colon and underscore versions for CLI compatibility)
    HyprlandAPI::addDispatcherV2(PHANDLE, "scrollingplus:partitions", handlePartitions);
    HyprlandAPI::addDispatcherV2(PHANDLE, "scrollingplus:togglecenter", handleToggleCenter);
    HyprlandAPI::addDispatcherV2(PHANDLE, "scrollingplus:togglezen", handleToggleZen);

    HyprlandAPI::addDispatcherV2(PHANDLE, "scrollingplus_partitions", handlePartitions);
    HyprlandAPI::addDispatcherV2(PHANDLE, "scrollingplus_togglecenter", handleToggleCenter);
    HyprlandAPI::addDispatcherV2(PHANDLE, "scrollingplus_togglezen", handleToggleZen);

    // Register event listeners for debug logging and focus warping
    activeWindowListener = Event::bus()->m_events.window.active.listen([](PHLWINDOW w, auto /* reason */) {
        logState("Focus changed to " + getAddressStr(w));
        if (w && w->m_workspace) {
            WORKSPACEID ws_id = w->m_workspace->m_id;
            bool zenGlobal = isZenModeGlobal();
            bool isZenActive = zenGlobal ? g_zenModeActive : getWorkspaceSettings(ws_id).zenActive;
            
            if (isZenActive) {
                // Instantly sync opacities to warp focus transition instantly
                syncWorkspaceZenOpacity(ws_id, true);
            }
        }
    });

    activeWorkspaceListener = Event::bus()->m_events.workspace.active.listen([](PHLWORKSPACE ws) {
        logState("Workspace changed to workspace " + std::to_string(ws ? ws->m_id : -1));

        WORKSPACEID newWS = ws ? ws->m_id : -1;
        if (newWS == g_lastWorkspaceID) return;

        bool zenGlobal = isZenModeGlobal();
        bool centerGlobal = isCenterModeGlobal();
        logState("Workspace switch info: newWS=" + std::to_string(newWS) + ", lastWS=" + std::to_string(g_lastWorkspaceID) +
                 ", zenGlobal=" + std::to_string(zenGlobal) + ", centerGlobal=" + std::to_string(centerGlobal) +
                 ", current_g_zenActive=" + std::to_string(g_zenModeActive));

        // 1. Save old workspace state
        if (g_lastWorkspaceID != -1) {
            if (!zenGlobal) {
                getWorkspaceSettings(g_lastWorkspaceID).zenActive = g_zenModeActive;
            }
            if (!centerGlobal) {
                bool isOldZenActive = zenGlobal ? g_zenModeActive : getWorkspaceSettings(g_lastWorkspaceID).zenActive;
                if (!isOldZenActive) {
                    getWorkspaceSettings(g_lastWorkspaceID).focusFitMethod = getConfigValueInt("scrolling:focus_fit_method", 1);
                }
            }
        }

        // 2. Load and apply new workspace state
        if (newWS != -1) {
            auto& newSettings = getWorkspaceSettings(newWS);
            
            if (!zenGlobal) {
                g_zenModeActive = newSettings.zenActive;
            }
            
            logState("Loaded WS" + std::to_string(newWS) + " Zen settings: active=" + std::to_string(g_zenModeActive));

            // Sync the window opacity states on the new workspace
            syncWorkspaceZenOpacity(newWS, g_zenModeActive);

            // Restore the focus fit method for this workspace
            int64_t target_fit_method = 1;
            if (g_zenModeActive) {
                target_fit_method = 0;
            } else {
                target_fit_method = centerGlobal ? g_globalFocusFitMethod : newSettings.focusFitMethod;
            }
            setConfigValueInt("scrolling:focus_fit_method", target_fit_method);

            // Recalculate layout and damage monitor to refresh the viewport
            auto pMonitor = ws->m_monitor.lock();
            if (pMonitor) {
                // Align active column on the new workspace if layout is scrolling layout
                if (ws->m_space && ws->m_space->algorithm()) {
                    auto pTiled = ws->m_space->algorithm()->tiledAlgo().get();
                    Layout::Tiled::CScrollingAlgorithm* pScrolling = dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>(pTiled);
                    if (pScrolling) {
                        PHLWINDOW pTargetWindow = nullptr;
                        // Find currently active window on this workspace
                        for (auto const& w : Desktop::windowState()->windows()) {
                            if (w && w->m_workspace == ws && !w->isHidden() && w->m_isMapped && !w->m_isFloating) {
                                if (Desktop::focusState()->isWindowActive(w)) {
                                    pTargetWindow = w;
                                    break;
                                }
                            }
                        }
                        if (!pTargetWindow) {
                            for (auto const& w : Desktop::windowState()->windows()) {
                                if (w && w->m_workspace == ws && !w->isHidden() && w->m_isMapped && !w->m_isFloating) {
                                    pTargetWindow = w;
                                    break;
                                }
                            }
                        }
                        if (pTargetWindow) {
                            SP<Layout::ITarget> pTarget = getTargetForWindow(pTargetWindow);
                            if (pTarget) {
                                auto pTargetData = pScrolling->dataFor(pTarget);
                                if (pTargetData && pTargetData->column) {
                                    auto pColumn = pTargetData->column.lock();
                                    if (pColumn) {
                                        alignActiveColumn(pScrolling, pColumn, target_fit_method);
                                    }
                                }
                            }
                        }
                    }
                }

                g_layoutManager->recalculateMonitor(pMonitor);
                if (g_pHyprRenderer) {
                    g_pHyprRenderer->damageMonitor(pMonitor);
                }
            }
        }

        g_lastWorkspaceID = newWS;
    });

    fullscreenListener = Event::bus()->m_events.window.fullscreen.listen([](PHLWINDOW w) {
        logState("Window fullscreen state changed on " + getAddressStr(w));
        
        if (w && !Fullscreen::controller()->isFullscreen(w) && w->m_workspace) {
            // Cancel previous timer if any was running to avoid overlaps
            if (pFullscreenRestoreTimer) {
                pFullscreenRestoreTimer->cancel();
                pFullscreenRestoreTimer.reset();
            }

            pFullscreenRestoreTimer = Hyprutils::Memory::makeShared<CEventLoopTimer>(std::chrono::milliseconds(100), [w](SP<CEventLoopTimer>, void*) {
                if (!w || w->isHidden() || !w->m_isMapped) {
                    pFullscreenRestoreTimer.reset();
                    return;
                }

                if (!w->m_workspace) {
                    pFullscreenRestoreTimer.reset();
                    return;
                }

                auto pWorkspace = w->m_workspace;
                auto pMonitor = pWorkspace->m_monitor.lock();
                if (!pMonitor) {
                    pFullscreenRestoreTimer.reset();
                    return;
                }

                if (!pWorkspace->m_space || !pWorkspace->m_space->algorithm()) {
                    pFullscreenRestoreTimer.reset();
                    return;
                }

                auto pTiled = pWorkspace->m_space->algorithm()->tiledAlgo().get();
                Layout::Tiled::CScrollingAlgorithm* pScrolling = dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>(pTiled);
                if (!pScrolling) {
                    pFullscreenRestoreTimer.reset();
                    return;
                }

                SP<Layout::ITarget> pTarget = getTargetForWindow(w);
                if (pTarget) {
                    auto pTargetData = pScrolling->dataFor(pTarget);
                    if (pTargetData && pTargetData->column) {
                        auto pColumn = pTargetData->column.lock();
                        if (pColumn) {
                            int64_t current_fit = getConfigValueInt("scrolling:focus_fit_method", 1);
                            alignActiveColumn(pScrolling, pColumn, current_fit);
                        }
                    }
                }

                g_layoutManager->recalculateMonitor(pMonitor);
                pFullscreenRestoreTimer.reset();
            }, nullptr);

            g_pEventLoopManager->addTimer(pFullscreenRestoreTimer);
        }
    });

    windowOpenListener = Event::bus()->m_events.window.open.listen([](PHLWINDOW w) {
        logState("Window opened: " + getAddressStr(w));
    });

    windowCloseListener = Event::bus()->m_events.window.close.listen([](PHLWINDOW w) {
        logState("Window closed: " + getAddressStr(w));
    });

    // HyprlandAPI::addNotification(PHANDLE, "[hyprscrolling-plus] Loaded successfully!",
    //                              CHyprColor{0.2, 1.0, 0.2, 1.0}, 5000);

    return {"hyprscrolling-plus", "Grid partitions and centering patches for scrolling layout", "Antigravity", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    logState("PLUGIN_EXIT called");
    HyprlandAPI::removeDispatcher(PHANDLE, "scrollingplus:partitions");
    HyprlandAPI::removeDispatcher(PHANDLE, "scrollingplus:togglecenter");
    HyprlandAPI::removeDispatcher(PHANDLE, "scrollingplus:togglezen");

    activeWindowListener.reset();
    activeWorkspaceListener.reset();
    fullscreenListener.reset();
    windowOpenListener.reset();
    windowCloseListener.reset();

    if (pFullscreenRestoreTimer) {
        pFullscreenRestoreTimer->cancel();
        pFullscreenRestoreTimer.reset();
    }

    g_zenModeGlobalConfig.reset();
    g_centerModeGlobalConfig.reset();
}
