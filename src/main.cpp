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
#undef private


inline HANDLE PHANDLE = nullptr;

inline Hyprutils::Signal::CHyprSignalListener activeWindowListener;
inline Hyprutils::Signal::CHyprSignalListener activeWorkspaceListener;
inline Hyprutils::Signal::CHyprSignalListener fullscreenListener;
inline Hyprutils::Signal::CHyprSignalListener windowOpenListener;
inline Hyprutils::Signal::CHyprSignalListener windowCloseListener;

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
    
    // Format width with 6 decimal places
    std::stringstream ss;
    ss << std::fixed << std::setprecision(6) << width;
    std::string width_str = ss.str();

    // Get current active workspace
    PHLMONITOR pActiveMonitor = g_pCompositor->getMonitorFromCursor();
    if (!pActiveMonitor || !pActiveMonitor->m_activeWorkspace) {
        return {false, false, "No active workspace found"};
    }
    WORKSPACEID ws_id = pActiveMonitor->m_activeWorkspace->m_id;

    // Get all windows on the active workspace and sort them left-to-right
    std::vector<PHLWINDOW> ws_windows;
    for (auto const& w : g_pCompositor->m_windows) {
        if (w && w->m_workspace && w->m_workspace->m_id == ws_id && !w->isHidden() && w->m_isMapped && !w->m_isFloating) {
            ws_windows.push_back(w);
        }
    }
    
    if (ws_windows.empty()) {
        return {};
    }

    std::sort(ws_windows.begin(), ws_windows.end(), [](const PHLWINDOW& a, const PHLWINDOW& b) {
        return a->m_position.x < b->m_position.x;
    });

    PHLWINDOW pOriginalActive = nullptr;
    for (auto const& w : ws_windows) {
        if (g_pCompositor->isWindowActive(w)) {
            pOriginalActive = w;
            break;
        }
    }

    // Store original states to restore later
    std::string orig_animations = HyprlandAPI::invokeHyprctlCommand("getoption", "animations:enabled");
    bool animations_enabled = orig_animations.find("int: 1") != std::string::npos;

    std::string orig_fit = HyprlandAPI::invokeHyprctlCommand("getoption", "scrolling:focus_fit_method");
    std::string final_fit_method = "1";
    if (partitions > 1) {
        final_fit_method = (orig_fit.find("int: 0") != std::string::npos) ? "0" : "1";
    }

    // Set configuration values dynamically
    HyprlandAPI::invokeHyprctlCommand("keyword", "animations:enabled 0");
    HyprlandAPI::invokeHyprctlCommand("keyword", "scrolling:focus_fit_method " + final_fit_method);
    HyprlandAPI::invokeHyprctlCommand("keyword", "scrolling:column_width " + width_str);

    // Apply resizing sequentially to every window
    for (auto const& w : ws_windows) {
        std::string addrStr = getAddressStr(w);
        HyprlandAPI::invokeHyprctlCommand("dispatch", "focuswindow address:" + addrStr);
        HyprlandAPI::invokeHyprctlCommand("dispatch", "layoutmsg colresize " + width_str);
    }

    // Restore original active focus / alignment
    if (pOriginalActive) {
        if (final_fit_method == "1") {
            // Find active index and original address
            int active_idx = 0;
            std::string active_addr = getAddressStr(pOriginalActive);
            for (size_t i = 0; i < ws_windows.size(); ++i) {
                if (ws_windows[i] == pOriginalActive) {
                    active_idx = i;
                    break;
                }
            }

            int P = partitions;
            int away_idx = 0;
            if (active_idx < P) {
                away_idx = (int)ws_windows.size() - 1;
            }
            
            std::string away_addr = getAddressStr(ws_windows[away_idx]);
            HyprlandAPI::invokeHyprctlCommand("dispatch", "focuswindow address:" + away_addr);

            int start_idx = (active_idx / P) * P;
            if (start_idx < (int)ws_windows.size()) {
                std::string start_addr = getAddressStr(ws_windows[start_idx]);
                HyprlandAPI::invokeHyprctlCommand("dispatch", "focuswindow address:" + start_addr);
            }

            if (!active_addr.empty()) {
                HyprlandAPI::invokeHyprctlCommand("dispatch", "focuswindow address:" + active_addr);
            }
        } else {
            std::string origAddrStr = getAddressStr(pOriginalActive);
            HyprlandAPI::invokeHyprctlCommand("dispatch", "focuswindow address:" + origAddrStr);
        }
    }

    // Restore settings
    HyprlandAPI::invokeHyprctlCommand("keyword", "scrolling:focus_fit_method " + final_fit_method);
    HyprlandAPI::invokeHyprctlCommand("keyword", "animations:enabled " + std::to_string(animations_enabled ? 1 : 0));

    return {};
}

SDispatchResult handleToggleCenter(std::string) {
    logState("Dispatcher: togglecenter");
    // Read current focus fit method
    std::string option_info = HyprlandAPI::invokeHyprctlCommand("getoption", "scrolling:focus_fit_method");
    bool is_centered = option_info.find("int: 0") != std::string::npos;
    std::string new_fit = is_centered ? "1" : "0";

    // Get active workspace ID
    PHLMONITOR pActiveMonitor = g_pCompositor->getMonitorFromCursor();
    if (!pActiveMonitor || !pActiveMonitor->m_activeWorkspace) {
        return {false, false, "No active workspace found"};
    }
    WORKSPACEID ws_id = pActiveMonitor->m_activeWorkspace->m_id;

    // Get windows on the active workspace and sort them left-to-right
    std::vector<PHLWINDOW> ws_windows;
    for (auto const& w : g_pCompositor->m_windows) {
        if (w && w->m_workspace && w->m_workspace->m_id == ws_id && !w->isHidden() && w->m_isMapped && !w->m_isFloating) {
            ws_windows.push_back(w);
        }
    }
    
    if (ws_windows.empty()) {
        return {};
    }

    std::sort(ws_windows.begin(), ws_windows.end(), [](const PHLWINDOW& a, const PHLWINDOW& b) {
        return a->m_position.x < b->m_position.x;
    });

    PHLWINDOW pActiveWindow = nullptr;
    int active_idx = 0;
    std::string active_addr = "";
    for (size_t i = 0; i < ws_windows.size(); ++i) {
        if (g_pCompositor->isWindowActive(ws_windows[i])) {
            pActiveWindow = ws_windows[i];
            active_idx = i;
            active_addr = getAddressStr(pActiveWindow);
            break;
        }
    }

    // Read column width
    std::string width_info = HyprlandAPI::invokeHyprctlCommand("getoption", "scrolling:column_width");
    double col_width = 0.5;
    size_t float_pos = width_info.find("float: ");
    if (float_pos != std::string::npos) {
        try {
            col_width = std::stod(width_info.substr(float_pos + 7));
        } catch (...) {}
    }
    int P = std::max(1, (int)std::round(1.0 / col_width));

    std::string orig_animations = HyprlandAPI::invokeHyprctlCommand("getoption", "animations:enabled");
    bool animations_enabled = orig_animations.find("int: 1") != std::string::npos;

    // Apply focus fit method
    HyprlandAPI::invokeHyprctlCommand("keyword", "animations:enabled 0");
    HyprlandAPI::invokeHyprctlCommand("keyword", "scrolling:focus_fit_method " + new_fit);

    if (new_fit == "1") {
        // Toggle OFF (fit mode): Scroll away to break centered state, then align
        int away_idx = 0;
        if (active_idx < P) {
            away_idx = (int)ws_windows.size() - 1;
        }
        
        std::string away_addr = getAddressStr(ws_windows[away_idx]);
        HyprlandAPI::invokeHyprctlCommand("dispatch", "focuswindow address:" + away_addr);

        int start_idx = (active_idx / P) * P;
        if (start_idx < (int)ws_windows.size()) {
            std::string start_addr = getAddressStr(ws_windows[start_idx]);
            HyprlandAPI::invokeHyprctlCommand("dispatch", "focuswindow address:" + start_addr);
        }

        if (!active_addr.empty()) {
            HyprlandAPI::invokeHyprctlCommand("dispatch", "focuswindow address:" + active_addr);
        }
    } else {
        // Toggle ON (center mode): Jitter focus change to center active window
        int jitter_idx = (active_idx == 0 && ws_windows.size() > 1) ? 1 : 0;
        std::string jitter_addr = getAddressStr(ws_windows[jitter_idx]);
        HyprlandAPI::invokeHyprctlCommand("dispatch", "focuswindow address:" + jitter_addr);
        
        if (!active_addr.empty()) {
            HyprlandAPI::invokeHyprctlCommand("dispatch", "focuswindow address:" + active_addr);
        }
    }

    // Restore animations
    HyprlandAPI::invokeHyprctlCommand("keyword", "animations:enabled " + std::to_string(animations_enabled ? 1 : 0));

    return {};
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

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
            // Defer the first focus change to the next idle cycle
            g_pEventLoopManager->doLater([w]() {
                if (!w || w->isHidden() || !w->m_isMapped) return;

                // Find all windows on this workspace to see if we need to refocus
                std::vector<PHLWINDOW> ws_windows;
                for (auto const& win : g_pCompositor->m_windows) {
                    if (win && win->m_workspace && win->m_workspace->m_id == w->m_workspace->m_id && !win->isHidden() && win->m_isMapped && !win->m_isFloating) {
                        ws_windows.push_back(win);
                    }
                }

                if (ws_windows.size() > 1) {
                    // Sort left-to-right to find physical adjacencies
                    std::sort(ws_windows.begin(), ws_windows.end(), [](const PHLWINDOW& a, const PHLWINDOW& b) {
                        return a->m_position.x < b->m_position.x;
                    });

                    // Find the index of the fullscreen window w in the sorted list
                    int w_idx = -1;
                    for (size_t i = 0; i < ws_windows.size(); ++i) {
                        if (ws_windows[i] == w) {
                            w_idx = i;
                            break;
                        }
                    }

                    PHLWINDOW pOther = nullptr;
                    if (w_idx != -1) {
                        if (w_idx > 0) {
                            pOther = ws_windows[w_idx - 1];
                        } else if (w_idx < (int)ws_windows.size() - 1) {
                            pOther = ws_windows[w_idx + 1];
                        }
                    }

                    // Fallback to first non-self window if index was not found
                    if (!pOther) {
                        for (auto const& win : ws_windows) {
                            if (win != w) {
                                pOther = win;
                                break;
                            }
                        }
                    }

                    if (pOther) {
                        // Temporarily disable animations for a seamless layout refresh
                        std::string orig_animations = HyprlandAPI::invokeHyprctlCommand("getoption", "animations:enabled");
                        bool animations_enabled = orig_animations.find("int: 1") != std::string::npos;

                        HyprlandAPI::invokeHyprctlCommand("keyword", "animations:enabled 0");

                        // Focus the other window in this tick to force a layout resize
                        HyprlandAPI::invokeHyprctlCommand("dispatch", "focuswindow address:" + getAddressStr(pOther));

                        // Defer focusing back to w to the NEXT idle cycle
                        g_pEventLoopManager->doLater([w, animations_enabled]() {
                            if (w && !w->isHidden() && w->m_isMapped) {
                                HyprlandAPI::invokeHyprctlCommand("dispatch", "focuswindow address:" + getAddressStr(w));
                            }
                            // Restore animations
                            HyprlandAPI::invokeHyprctlCommand("keyword", "animations:enabled " + std::to_string(animations_enabled ? 1 : 0));
                        });
                    }
                }
            });
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
    HyprlandAPI::removeDispatcher(PHANDLE, "scrollingplus:partitions");
    HyprlandAPI::removeDispatcher(PHANDLE, "scrollingplus:togglecenter");

    activeWindowListener.reset();
    activeWorkspaceListener.reset();
    fullscreenListener.reset();
    windowOpenListener.reset();
    windowCloseListener.reset();
}
