# hyprscrolling-plus

A native C++ plugin for Hyprland that extends the scrolling layout with layout partition grids and instant centering toggles.

It enables precise viewport partitioning (e.g., 2w, 3w layouts) and robust active window centering that is preserved across fullscreen/maximize transitions.

## Features

- **Grid Partitions**: Split the infinite scrolling tape into precise fractional columns relative to your viewport (e.g., 1, 2, 3, or 4 columns).
- **Centering Toggles**: Quickly snap the active window/column to the horizontal center of the screen, which dynamically adjusts and remains locked even after toggling fullscreen/maximize state off.

## Dispatchers

The plugin registers two custom dispatchers:

- `scrollingplus:partitions <N>`
  Sets the active workspace column widths to exactly `1/N` of the screen width. Useful for instantly configuring layout columns (e.g., `2` for a 2-window viewport split).
- `scrollingplus:togglecenter`
  Toggles the focus fit method between `0` (Center mode) and `1` (Fit mode), immediately centering the active column on the screen if center mode is activated.

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
