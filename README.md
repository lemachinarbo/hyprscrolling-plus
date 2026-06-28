# hyprscrolling-plus

A native C++ plugin for Hyprland that extends the scrolling layout with layout partition grids, instant centering, and Zen focus modes.

It enables precise viewport partitioning (e.g., 2w, 3w layouts) and robust active window centering that is preserved across fullscreen/maximize transitions.

## Features



https://github.com/user-attachments/assets/8e94c795-f587-4339-aa77-8e89c87dbb40



- **Grid Partitions**: Split the infinite scrolling tape into precise fractional columns relative to your viewport (e.g., 1, 2, 3, or 4 columns).
- **Centering Toggles**: Quickly snap the active window/column to the horizontal center of the screen, which dynamically adjusts and remains locked even after toggling fullscreen/maximize state off.
- **Native Zen Mode**: Dim inactive windows and focus entirely on the active centered column with a single shortcut.

## Dispatchers

The plugin registers three custom dispatchers:

- `scrollingplus:partitions <N>`
  Sets the active workspace column widths to exactly `1/N` of the screen width. Useful for instantly configuring layout columns (e.g., `2` for a 2-window viewport split).
- `scrollingplus:togglecenter`
  Toggles the focus fit method between `0` (Center mode) and `1` (Fit mode), immediately centering the active column on the screen if center mode is activated.
- `scrollingplus:togglezen [opacity]`
  Toggles Zen focus mode. When turned ON, it dims inactive windows to the specified `opacity` (defaults to `0.0` if not provided) and centers the active column. Toggling it OFF restores your previous opacity, dim, and centering settings.

## Recommended Keybindings

To use the custom dispatchers, map them in your `hyprland.conf` file:

```hyprland
# scrollingplus layout partitions (Super + Alt + 1-4)
bind = SUPER ALT, 1, scrollingplus:partitions, 1
bind = SUPER ALT, 2, scrollingplus:partitions, 2
bind = SUPER ALT, 3, scrollingplus:partitions, 3
bind = SUPER ALT, 4, scrollingplus:partitions, 4

# Centering active window toggle (Super + Alt + C)
bind = SUPER ALT, C, scrollingplus:togglecenter,

# Zen mode toggle (Super + Alt + Z)
bind = SUPER ALT, Z, scrollingplus:togglezen, 0.0
```

## Configuration

By default, Zen mode and Centering mode are per-workspace. If you prefer these modes to apply globally across all workspaces, enable them in your `hyprland.conf`:

```hyprland
plugin {
    scrollingplus {
        # Set to 1 to enable global mode across workspaces (default is 0)
        zen_mode_global = 1
        center_mode_global = 1
    }
}
```

## Installation

### Persistent Installation (hyprpm)

To persistently register the plugin so it is managed by the Hyprland plugin manager:

```bash
hyprpm add https://github.com/lemachinarbo/hyprscrolling-plus.git
hyprpm enable hyprscrolling-plus
hyprpm reload -n
```

If there are new updates in the repository, you can fetch and rebuild the installed plugin via:

```bash
hyprpm update
```

### Local Compilation and Loading

If you want to clone, modify, or load the plugin dynamically:

```bash
# Clone the repository
git clone https://github.com/lemachinarbo/hyprscrolling-plus.git
cd hyprscrolling-plus

# Compile the plugin locally
make

# Load the compiled plugin
hyprctl plugin load $(pwd)/hyprscrolling-plus.so

# Unload the plugin
hyprctl plugin unload $(pwd)/hyprscrolling-plus.so
```

---

Deployed by [mutants.txt](mutants.txt)
