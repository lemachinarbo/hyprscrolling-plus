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
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollingAlgorithm.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
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
    if (!pScrolling || !pColumn) return;

    if (fit_method == 0) {
        // Center mode: center active column
        pScrolling->m_scrollingData->centerCol(pColumn);
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

        int active_idx = pScrolling->m_scrollingData->idx(pColumn);
        int start_idx = (active_idx / P) * P;

        CBox usableArea = pScrolling->usableArea();
        double target_offset = pScrolling->m_scrollingData->controller->calculateStripStart(start_idx, usableArea);
        pScrolling->m_scrollingData->controller->setOffset(target_offset);
    }
}

void logState(const std::string& action) {
    std::ofstream logFile("/tmp/hyprscrolling-plus.log", std::ios::app);
    if (!logFile.is_open()) return;

    logFile << "[" << getTimestamp() << "] ACTION: " << action << "\n";

    PHLMONITOR pActiveMonitor = g_pCompositor->getMonitorFromCursor();
    if (!pActiveMonitor || !pActiveMonitor->m_activeWorkspace) {
        logFile << "  No active workspace\n\n";
        return;
    }
    WORKSPACEID ws_id = pActiveMonitor->m_activeWorkspace->m_id;
    logFile << "  Active Workspace: " << ws_id << "\n";

    std::vector<PHLWINDOW> ws_windows;
    for (auto const& w : g_pCompositor->m_windows) {
        if (w && w->m_workspace && w->m_workspace->m_id == ws_id && !w->isHidden() && w->m_isMapped && !w->m_isFloating) {
            ws_windows.push_back(w);
        }
    }

    std::sort(ws_windows.begin(), ws_windows.end(), [](const PHLWINDOW& a, const PHLWINDOW& b) {
        return a->m_position.x < b->m_position.x;
    });

    logFile << "  Windows (sorted left-to-right):\n";
    for (size_t i = 0; i < ws_windows.size(); ++i) {
        auto w = ws_windows[i];
        bool active = g_pCompositor->isWindowActive(w);
        logFile << "    - Window " << i << " | " << getAddressStr(w)
                << " | class: " << w->m_class
                << " | title: " << w->m_title << "\n"
                << "      pos: (" << (int)w->m_position.x << ", " << (int)w->m_position.y << ")"
                << " | size: (" << (int)w->m_size.x << ", " << (int)w->m_size.y << ")"
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

static double getConfigValueFloat(const std::string& name, double defaultValue) {
    auto reply = Config::mgr()->getConfigValue(name);
    if (reply.dataptr && reply.type) {
        if (*reply.type == typeid(float)) {
            return *(float*)(*reply.dataptr);
        } else if (*reply.type == typeid(double)) {
            return *(double*)(*reply.dataptr);
        }
    }
    return defaultValue;
}

static void setConfigValueFloat(const std::string& name, double value) {
    auto reply = Config::mgr()->getConfigValue(name);
    if (reply.dataptr && reply.type) {
        if (*reply.type == typeid(float)) {
            *(float*)(*reply.dataptr) = (float)value;
        } else if (*reply.type == typeid(double)) {
            *(double*)(*reply.dataptr) = value;
        }
    }
}

static bool g_zenModeActive = false;
static double g_savedInactiveOpacity = 1.0;
static bool g_savedDimInactive = false;
static int64_t g_savedFocusFitMethod = 1;

SDispatchResult handleToggleZen(std::string args) {
    logState("Dispatcher: togglezen " + args);

    double target_opacity = 0.0;
    if (!args.empty()) {
        try {
            target_opacity = std::stod(args);
        } catch (...) {
            target_opacity = 0.0;
        }
    }

    PHLMONITOR pActiveMonitor = g_pCompositor->getMonitorFromCursor();
    if (!pActiveMonitor || !pActiveMonitor->m_activeWorkspace) {
        return {false, false, "No active workspace found"};
    }

    if (g_zenModeActive) {
        int64_t target_fit_method = g_savedFocusFitMethod;
        
        if (pActiveMonitor->m_activeWorkspace->m_space && pActiveMonitor->m_activeWorkspace->m_space->algorithm()) {
            auto pTiled = pActiveMonitor->m_activeWorkspace->m_space->algorithm()->tiledAlgo().get();
            Layout::Tiled::CScrollingAlgorithm* pScrolling = dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>(pTiled);
            if (pScrolling) {
                double col_width = 0.5;
                auto reply = Config::mgr()->getConfigValue("scrolling:column_width");
                if (reply.dataptr && reply.type) {
                    if (*reply.type == typeid(float)) {
                        col_width = *(float*)(*reply.dataptr);
                    } else if (*reply.type == typeid(double)) {
                        col_width = *(double*)(*reply.dataptr);
                    }
                }
                if (col_width < 0.9) {
                    target_fit_method = 0;
                } else {
                    target_fit_method = 1;
                }
            }
        }

        setConfigValueFloat("decoration:inactive_opacity", g_savedInactiveOpacity);
        setConfigValueInt("decoration:dim_inactive", g_savedDimInactive);
        setConfigValueInt("scrolling:focus_fit_method", target_fit_method);

        g_zenModeActive = false;
        
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
                                alignActiveColumn(pScrolling, pColumn, target_fit_method);
                            }
                        }
                    }
                }
            }
        }
    } else {
        g_savedInactiveOpacity = getConfigValueFloat("decoration:inactive_opacity", 1.0);
        g_savedDimInactive = getConfigValueInt("decoration:dim_inactive", 0) != 0;
        g_savedFocusFitMethod = getConfigValueInt("scrolling:focus_fit_method", 1);

        setConfigValueFloat("decoration:inactive_opacity", target_opacity);
        setConfigValueInt("decoration:dim_inactive", 1);
        setConfigValueInt("scrolling:focus_fit_method", 0);

        g_zenModeActive = true;

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
    PHLMONITOR pActiveMonitor = g_pCompositor->getMonitorFromCursor();
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
    // Read current focus fit method
    int64_t current_fit = getConfigValueInt("scrolling:focus_fit_method", 1);
    int64_t new_fit = (current_fit == 0) ? 1 : 0;

    // Apply focus fit method config
    setConfigValueInt("scrolling:focus_fit_method", new_fit);

    // Get active monitor/workspace
    PHLMONITOR pActiveMonitor = g_pCompositor->getMonitorFromCursor();
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

    // Recalculate layout
    g_layoutManager->recalculateMonitor(pActiveMonitor);

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

    // Register our custom dispatchers
    HyprlandAPI::addDispatcherV2(PHANDLE, "scrollingplus:partitions", handlePartitions);
    HyprlandAPI::addDispatcherV2(PHANDLE, "scrollingplus:togglecenter", handleToggleCenter);
    HyprlandAPI::addDispatcherV2(PHANDLE, "scrollingplus:togglezen", handleToggleZen);

    // Register event listeners for debug logging
    activeWindowListener = Event::bus()->m_events.window.active.listen([](PHLWINDOW w, auto reason) {
        logState("Focus changed to " + getAddressStr(w));
    });

    activeWorkspaceListener = Event::bus()->m_events.workspace.active.listen([](PHLWORKSPACE ws) {
        logState("Workspace changed to workspace " + std::to_string(ws ? ws->m_id : -1));
    });

    fullscreenListener = Event::bus()->m_events.window.fullscreen.listen([](PHLWINDOW w) {
        logState("Window fullscreen state changed on " + getAddressStr(w));
        
        if (w && !w->isFullscreen() && w->m_workspace) {
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

    HyprlandAPI::addNotification(PHANDLE, "[hyprscrolling-plus] Loaded successfully!",
                                 CHyprColor{0.2, 1.0, 0.2, 1.0}, 5000);

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
}
