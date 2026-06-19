# Centralized Agents Project: hyprscrolling-plus

## Project Identity

- Name: hyprscrolling-plus
- Description: Native C++ plugin patch for Hyprland to extend the scrolling layout with layout partition grids and instant centering toggles.
- Purpose: Frame-perfect viewport alignment and column management inside Hyprland's rendering loop.
- Audience: Users of Omarchy Linux and the scrolling layout.
- Goals: Zero-jitter partition layout switching and seamless center-mode toggling.

## Git And Commit Standards

- **No Auto-Committing**: Agents must never commit autonomously, EXCEPT when executing the Conductor/Jules Orchestrator release workflow where autonomous committing is required upon user approval of the commit message.
- **Flat History Only**: Never create merge commits. Always squash or rebase to maintain a linear timeline.
- **Scratch Discipline**: All scratch files, debug scripts, or temporary data created during implementation MUST be placed in `scratch/`. NEVER create scratch files in the project root.
- **Commit Format**: Strictly follow the Conventional Commits specification.
- **No Emojis**: Strictly forbidden in commits, PR titles, or descriptions.
- **Translation Logic (Strict Mapping)**:
   - `add` -> commit as `feat: [description]`
   - `fix` -> commit as `fix: [description]`
   - `remove`, `update`, or `refactor` -> commit as `refactor: [description]`
   - `chore` -> commit as `chore: [description]` (for internal config, tooling, and repo maintenance).
   - **Constraint**: If a request uses a verb outside this list, stop and ask for the correct mapping. Do not infer.

## Submission & PR Guidelines

When creating a PR or submission description, you MUST use Conventional Commit titles and a linear description.

**Title Format:** `type: brief description` (no emojis, no capital start, no trailing period)
- **Message Structure (Required)**:
   - Subject line must be exactly: `<type>: <imperative description>`
   - Add a blank line after subject
   - Add a short bullet body describing concrete changes
   - Use `- ` bullets only (no nested lists)
- **Style Rules**:
   - **Case**: The entire subject line must be lowercase.
   - **No Fluff**: No emojis, no "AI-generated" or "Verified" footers, and no trailing periods.
   - **Length**: Keep the subject line concise (under 50 characters).

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
