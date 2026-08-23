# Centralized Agents Project: hyprscrolling-plus

## Project Identity

- Name: hyprscrolling-plus
- Description: Native C++ plugin patch for Hyprland to extend the scrolling layout with layout partition grids and instant centering toggles.
- Purpose: Frame-perfect viewport alignment and column management inside Hyprland's rendering loop.
- Audience: Users of Omarchy Linux and the scrolling layout.
- Goals: Zero-jitter partition layout switching and seamless center-mode toggling.
- Name: hyprsummon
- Description: Universal Window Summoner & Instance Manager for Hyprland on Omarchy Linux.
- Purpose: Out-of-process window summoning, MRU multi-instance cycling, and silent background parking.
- Audience: Users of Omarchy Linux and Hyprland.

---

## The Core Architecture & Philosophy

### 1. Separation of Concerns (The Two Projects)

| Layer | Project | Role & Strict Scope |
|---|---|---|
| **Layout & Render Engine** | `hyprscrolling-plus` (Native C++ Plugin) | Frame-perfect scrolling layout only.<br>• Grid partitions (1, 2, 3, 4 columns)<br>• Instant Centering & Zen Mode<br>• Card-preserving column navigation & movement<br>• Continuous Anchor-Relative Viewport Clamping<br>• Instant programmatic window slotting (`forceInstant=true`)<br>• **Rule: Zero app lifecycle, zero process spawning, zero regex matching.** |
| **Window & Process Orchestrator** | `hyprsummon` (IPC Daemon / CLI) | Universal window summoning & lifecycle.<br>• "Don't Search, Just Summon"<br>• Multi-instance MRU cycling (1 ➔ 2 ➔ 3 ➔ 1)<br>• Side-by-side tape expansion beside active anchor (`focus_idx + 1`)<br>• Silent Background Parking (`Super+W`)<br>• Destructive Process Quit (`Super+Q`) |

---

### 2. How We Work WITH Omarchy (No Fragile Monkey-Patching)

- **Never create wrapper scripts in `~/.local/bin/`** (e.g. overriding `gtk-launch`, `omarchy-launch-or-focus`, `xdg-open`).
- **Never create per-app `.desktop` overrides in `~/.local/share/applications/`** (e.g. hacking `Exec=hyprctl ...` in `chromium.desktop` or `libreoffice.desktop`), because:
  - It breaks internal Omarchy tools like `omarchy-launch-webapp` (which parses `.desktop` files to locate browser binaries).
  - It requires manual maintenance for every new app installed on Arch/Omarchy.
- **Respect Omarchy's Shell & App Launcher Layer**:
  - Omarchy's Quickshell bar and App Menu execute standard `Util.execDetached("uwsm-app -- gtk-launch " + id)`.
  - Let Omarchy launch normally. The summoner interacts at the compositor level.

---

### 3. Respect Hyprland Rules (Compositor Level Only)

- **Respect Hyprland's Event Bus & IPC**:
  - Interact through Hyprland’s native event socket (`.socket2.sock`) and JSON IPC (`hyprctl -j clients`, `hyprctl -j activewindow`).
- **Use Native Hyprland 0.56 Dispatchers**:
  - Dispatch window movements and focus via `hl.dsp.*` Lua syntax.
  - In Hyprland 0.56, use `follow = false` for silent window moves (`hl.dsp.window.move({ workspace = '...', window = 'address:<addr>', follow = false })`).
- **Pure Silent Parking**:
  - Scratchpads / background storage are purely an off-screen storage bin (`name:parked:<app>`), NEVER a visual overlay.
  - **NEVER call `toggle_special` / `togglespecialworkspace`**.

---

### 4. The Universal Window Lifecycle Contract

Every application follows this exact model with zero exceptions:

1. **`Super + W` (Silent Hide)**:
   - Silently parks the active window into the background (`name:parked:<app>`) using `follow = false`.
   - Never toggles or pops up an active overlay scratchpad on screen.
   - Focus stays anchored on the adjacent window to the left.
2. **`Super + Q` (Quit / Kill)**:
   - Calls `hl.dsp.window.close()` / `killactive` to completely terminate the process.
3. **`Ctrl + N` (New)**:
   - The in-app standard shortcut for opening a fresh tab, document, or window.
4. **Summoning (`Super + Shift + Key` / Launcher)**:
   - If app closed ➔ launches it via standard command (`uwsm app -- ...` or `omarchy-launch-webapp ...`).
   - If 1 instance exists ➔ teleports it to the active workspace beside your focused window (`anchor_idx + 1`).
   - If multiple instances exist ➔ pressing the shortcut repeatedly cycles through all instances (1 ➔ 2 ➔ 3 ➔ 1).
   - **Never auto-parks anchors**: preserves multi-window side-by-side tiling (1, 2, 3, 4 columns).

---

### 5. The Universal Viewport & Tape Model

- **"Focus is what user sees, not what tape wants or scratchpad wants."**
- **Continuous Anchor-Relative Viewport Clamping**:
  - Moving focus across already-visible windows produces **zero camera drift**.
  - Moving focus beyond the screen edge slides the viewport smoothly by **exactly 1 column**, keeping your spatial anchor visible.
  - Rigid 2-column page-flipping (`[0-1]` ➔ `[2-3]`) is eradicated.
- **Zero Cross-Screen Slide Animation**:
  - Programmatic slotting (`moveleft` / `moveright` by window address) forces instant calculation (`forceInstant = true`).
  - Summoned windows pop directly into their target column without sliding horizontally across existing windows from coordinate 0.
- **In-Place Gap Collapse**:
  - Hiding (`Super+W`) or closing (`Super+Q`) a window causes the tape to collapse the gap without camera jumping.

---

## Coding and Git Standards
- Follow Conventional Commits format (`feat: ...`, `fix: ...`, `refactor: ...`, `chore: ...`).
- No emojis in commit messages or code comments.
- Keep scratch files inside `scratch/`. line concise (under 50 characters).

## Development and Testing Workflow

### Rapid Local Development Loop
When modifying the plugin source code in `src/main.cpp`:
1. Compile the plugin locally (takes less than 2 seconds):
   ```bash
   make
   ```
2. Manually load the newly built plugin into your running Hyprland instance:
   ```bash
   hyprctl plugin load /home/lemachi/projects/hyprscrolling-plus/hyprscrolling-plus.so
   ```
3. Test your keybindings or dispatchers.
4. To modify the code again, unload the plugin first:
   ```bash
   hyprctl plugin unload /home/lemachi/projects/hyprscrolling-plus/hyprscrolling-plus.so
   ```

### Persistent Registry (hyprpm)
Once your changes are verified and committed:
1. Re-add and build with `hyprpm`:
   ```bash
   hyprpm add /home/lemachi/projects/hyprscrolling-plus
   ```
2. Enable and reload the plugin manager:
   ```bash
   hyprpm enable hyprscrolling-plus
   hyprpm reload -n
   ```

### Debugging and Logs
- **System and Compositor Logs**: Follow the active compositor log in real-time:
  ```bash
  tail -f "$XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/hyprland.log"
  ```
- **Custom Debug Log**: The plugin appends detailed action logs, window layouts, and focus changes to:
  `/tmp/hyprscrolling-plus.log`
