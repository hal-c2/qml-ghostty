import QtQuick
import QtQuick.Controls
import Ghostty

// A local shell in a window: the smallest complete use of Terminal and Pty.
ApplicationWindow {
    width: 900
    height: 560
    visible: true
    title: terminal.title || "Ghostty"
    color: terminal.backgroundColor

    Pty {
        id: pty
        onOutput: data => terminal.write(data)
        onExited: Qt.quit()
    }

    Terminal {
        id: terminal
        anchors.fill: parent
        anchors.rightMargin: scrollBar.width
        focus: true
        font.family: "monospace"
        font.pixelSize: 14
        onInput: data => pty.write(data)
        onResized: (columns, rows) => pty.resize(columns, rows)
        Component.onCompleted: pty.start(columns, rows)
    }

    ScrollBar {
        id: scrollBar
        anchors { top: parent.top; bottom: parent.bottom; right: parent.right }
        orientation: Qt.Vertical
        size: terminal.scrollTotal > 0 ? terminal.rows / terminal.scrollTotal : 1
        position: terminal.scrollTotal > 0 ? terminal.scrollOffset / terminal.scrollTotal : 0
        onPositionChanged: if (pressed) terminal.scrollTo(position * terminal.scrollTotal)
    }
}
