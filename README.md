# qml-ghostty

A Qt Quick terminal item built on [libghostty-vt](https://github.com/ghostty-org/ghostty).
libghostty-vt does the parsing, keeps the grid and scrollback, and encodes
keys, mouse, focus and paste. The item lays out the grid and paints it.

The `Terminal` item owns no process. You feed it output with `write()` and
send whatever it emits on `input` to the program. The program can be a
local `Pty` or a remote session such as hal-c2's `terminal.*` RPCs.

```qml
import Ghostty

Terminal {
    id: terminal
    focus: true
    font.family: "monospace"
    onInput: data => pty.write(data)
    onResized: (columns, rows) => pty.resize(columns, rows)
    Component.onCompleted: pty.start(columns, rows)

    Pty {
        id: pty
        onOutput: data => terminal.write(data)
    }
}
```

## Building

Requires Qt 6.8 or newer, CMake 3.21 or newer, and a C++20 compiler.

```sh
scripts/build-libghostty-vt.sh      # clones Ghostty at libghostty-vt.version, builds with Zig 0.15.2
cmake -B build -G Ninja
cmake --build build
QT_QPA_PLATFORM=offscreen ctest --test-dir build
./build/examples/shell/ghostty-shell
```

The script caches the Ghostty checkout and Zig under
`${XDG_CACHE_HOME:-~/.cache}/qml-ghostty` and installs into
`third_party/libghostty-vt`. Set `GHOSTTY_ZIG` to use a Zig you already
have. The pinned revision is the one hal-c2 vendors in
`native/libghostty-vt`, so the headers are interchangeable.

## API

### Terminal

| Property | |
| --- | --- |
| `font` | Monospace font. Bold, italic and bold italic are derived from it. |
| `foregroundColor`, `backgroundColor` | Default colours. |
| `cursorColor` | Invalid means the foreground colour. |
| `selectionColor` | Invalid means the selected cells are inverted. |
| `ansiColors` | Up to 256 colours. Missing entries keep Ghostty's palette. |
| `padding` | Space around the grid, in logical pixels. |
| `scrollbackLimit` | Rows of history. Only read at creation. |
| `columns`, `rows`, `cellWidth`, `cellHeight` | Read only. The grid follows the item's size. |
| `title`, `workingDirectory` | Set by OSC 0/2 and OSC 7. |
| `hasSelection` | Read only. |
| `scrollTotal`, `scrollOffset`, `atBottom` | Scrollback position in rows, enough to drive a `ScrollBar`. |

| Method | |
| --- | --- |
| `write(data)` | Program output, as a string or an `ArrayBuffer`. |
| `restore(data)` | Like `write`, but queries in it (DSR, DA) are not answered. Use it to replay retained history. |
| `reset()` | Blank screen with no history. |
| `paste(text)` | Bracketed if the program enabled it. Unsafe control bytes are defused. |
| `copy()`, `selectAll()`, `clearSelection()`, `selectedText()` | Selection and clipboard. |
| `text()` | Scrollback plus screen as plain text. |
| `scrollLines(delta)`, `scrollTo(offset)`, `scrollToBottom()` | Viewport scrolling. |

| Signal | |
| --- | --- |
| `input(data)` | Bytes for the program: keys, pastes, mouse reports, query replies. |
| `resized(columns, rows)` | The grid changed size. |
| `bell()` | BEL. |

Built-in bindings:

- Ctrl+Shift+C and Ctrl+Shift+V copy and paste.
- Shift+Insert pastes.
- Shift+PgUp and Shift+PgDn scroll.
- Dragging selects and double-click selects a word. Selections go to the X11/Wayland primary selection, and middle-click pastes it.
- While the program tracks the mouse, holding Shift bypasses tracking so you can still select.
- The wheel scrolls history. In full-screen programs it sends arrow keys instead (alternate scroll mode).

### Pty

`Pty` is Unix only. It runs one program on a pseudo terminal.

| Member | |
| --- | --- |
| `program` | Defaults to `$SHELL`, then `/bin/sh`. |
| `arguments` | Arguments for `program`. |
| `workingDirectory` | Directory the program starts in. |
| `environment` | Extra `NAME=value` entries. `TERM=xterm-256color` and `COLORTERM=truecolor` are always set. |
| `running`, `exitCode` | Read only. |
| `start(cols, rows)` | Starts the program at the given size. |
| `write(data)` | Sends bytes to the program. |
| `resize(cols, rows)` | Updates the pty's window size. |
| `terminate()` | Sends SIGHUP. |
| `output(data)`, `exited(code)`, `errorOccurred(message)` | Signals. |

## Using it from hal-c2

`apps/desktop-qt` can include this as a subdirectory and reuse its own
vendored headers. It still needs a Linux build of `libghostty-vt.a`, because
hal-c2 only ships headers:

```cmake
set(GHOSTTY_VT_INCLUDE_DIR "${HAL_C2_ROOT}/native/libghostty-vt/include")
set(GHOSTTY_VT_LIBRARY "/path/to/libghostty-vt.a")
set(QML_GHOSTTY_BUILD_EXAMPLES OFF)
set(QML_GHOSTTY_BUILD_TESTS OFF)
add_subdirectory(path/to/qml-ghostty qml-ghostty)
target_link_libraries(hal-c2-qt PRIVATE qmlghosttyplugin)
```

`TerminalDrawer.qml` can then replace its `WebSurface` with a `Terminal`.
In the sketch below, `session` stands for however the drawer reaches the
node's terminal RPCs and events:

```qml
import Ghostty

Terminal {
    onInput: data => session.request("terminal.write", { data })
    onResized: (cols, rows) => session.request("terminal.resize", { cols, rows })
    // On attach: terminal.restore(history), then write() each live `output` event.
    // On `restarted`: terminal.reset().
}
```

Replaying history with `restore()` rather than `write()` follows the rule in
`docs/internals/terminal-runtime.md`: replayed bytes must not produce replies
to the shell.

## Rendering

Each frame, `updatePolish()` repaints only the rows libghostty-vt reports as
dirty into a device-pixel `QImage`, and `updatePaintNode()` uploads it as a
texture. Glyphs the font covers go out as positioned `QGlyphRun`s, one run
per colour and weight. Everything else falls back to Qt's font matching:
emoji, CJK from other fonts, and multi-codepoint clusters. Cells snap to
whole device pixels.
