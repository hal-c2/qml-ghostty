#pragma once

#include <QColor>
#include <QElapsedTimer>
#include <QFont>
#include <QImage>
#include <QQmlEngine>
#include <QQuickItem>
#include <QRawFont>
#include <QTimer>
#include <QVariant>
#include <array>

// Opaque libghostty-vt handles, so this header does not pull in the C API.
typedef struct GhosttyTerminalImpl* GhosttyTerminal;
typedef struct GhosttyRenderStateImpl* GhosttyRenderState;
typedef struct GhosttyRenderStateRowIteratorImpl* GhosttyRenderStateRowIterator;
typedef struct GhosttyRenderStateRowCellsImpl* GhosttyRenderStateRowCells;
typedef struct GhosttyKeyEncoderImpl* GhosttyKeyEncoder;
typedef struct GhosttyKeyEventImpl* GhosttyKeyEvent;
typedef struct GhosttyMouseEncoderImpl* GhosttyMouseEncoder;
typedef struct GhosttyMouseEventImpl* GhosttyMouseEvent;
typedef struct GhosttySelectionGestureImpl* GhosttySelectionGesture;
typedef struct GhosttySelectionGestureEventImpl* GhosttySelectionGestureEvent;

// A terminal surface backed by libghostty-vt. It owns no process: feed it the
// program's output with write() and send what `input` emits back to the
// program, whether that is a local Pty or a remote session.
//
//   Terminal {
//       onInput: data => session.write(data)
//       onResized: (columns, rows) => session.resize(columns, rows)
//   }
//
// libghostty-vt parses, keeps the grid and scrollback, and encodes keys, mouse,
// focus and paste; this item lays the grid out and paints it.
class GhosttyTerminalItem : public QQuickItem {
    Q_OBJECT
    QML_NAMED_ELEMENT(Terminal)

    Q_PROPERTY(QFont font READ font WRITE setFont NOTIFY fontChanged)
    Q_PROPERTY(QColor foregroundColor READ foregroundColor WRITE setForegroundColor NOTIFY themeChanged)
    Q_PROPERTY(QColor backgroundColor READ backgroundColor WRITE setBackgroundColor NOTIFY themeChanged)
    // Invalid means the foreground colour.
    Q_PROPERTY(QColor cursorColor READ cursorColor WRITE setCursorColor NOTIFY themeChanged)
    // Invalid means inverting the selected cells.
    Q_PROPERTY(QColor selectionColor READ selectionColor WRITE setSelectionColor NOTIFY themeChanged)
    // Up to 256 colours; the first sixteen are the ANSI slots. Missing
    // entries keep Ghostty's defaults.
    Q_PROPERTY(QVariantList ansiColors READ ansiColors WRITE setAnsiColors NOTIFY themeChanged)
    Q_PROPERTY(qreal padding READ padding WRITE setPadding NOTIFY paddingChanged)
    // Rows of history kept above the screen. Only read before the first write.
    Q_PROPERTY(int scrollbackLimit READ scrollbackLimit WRITE setScrollbackLimit NOTIFY scrollbackLimitChanged)
    // False keeps the cursor steady even when the program asks it to blink, as
    // for reduced motion.
    Q_PROPERTY(bool cursorBlink READ cursorBlink WRITE setCursorBlink NOTIFY cursorBlinkChanged)
    // Treats Qt's Meta as Ctrl and its Control as Super, which is what they are
    // on macOS. On by default there, unless the application turned Qt's swap off.
    Q_PROPERTY(bool macModifiers READ macModifiers WRITE setMacModifiers NOTIFY macModifiersChanged)

    Q_PROPERTY(int columns READ columns NOTIFY gridChanged)
    Q_PROPERTY(int rows READ rows NOTIFY gridChanged)
    Q_PROPERTY(qreal cellWidth READ cellWidth NOTIFY gridChanged)
    Q_PROPERTY(qreal cellHeight READ cellHeight NOTIFY gridChanged)
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    // As the shell reported it (OSC 7 sends a file:// URI).
    Q_PROPERTY(QString workingDirectory READ workingDirectory NOTIFY workingDirectoryChanged)
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged)
    // History in rows: the viewport shows [scrollOffset, scrollOffset + rows)
    // of scrollTotal. Enough to drive a ScrollBar.
    Q_PROPERTY(qreal scrollTotal READ scrollTotal NOTIFY scrollChanged)
    Q_PROPERTY(qreal scrollOffset READ scrollOffset NOTIFY scrollChanged)
    Q_PROPERTY(bool atBottom READ atBottom NOTIFY scrollChanged)
    // A program has asked for the mouse. Shift, Ctrl and Cmd still select text.
    Q_PROPERTY(bool mouseTracking READ mouseTracking NOTIFY mouseTrackingChanged)
    // Why the terminal could not be created; empty when it is healthy. A property
    // because creation happens before QML can connect to anything.
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)

public:
    explicit GhosttyTerminalItem(QQuickItem* parent = nullptr);
    ~GhosttyTerminalItem() override;

    QFont font() const { return m_font; }
    void setFont(const QFont& font);
    QColor foregroundColor() const { return m_foreground; }
    void setForegroundColor(const QColor& color);
    QColor backgroundColor() const { return m_background; }
    void setBackgroundColor(const QColor& color);
    QColor cursorColor() const { return m_cursorColor; }
    void setCursorColor(const QColor& color);
    QColor selectionColor() const { return m_selectionColor; }
    void setSelectionColor(const QColor& color);
    QVariantList ansiColors() const { return m_ansiColors; }
    void setAnsiColors(const QVariantList& colors);
    qreal padding() const { return m_padding; }
    void setPadding(qreal padding);
    int scrollbackLimit() const { return m_scrollbackLimit; }
    void setScrollbackLimit(int rows);
    bool cursorBlink() const { return m_cursorBlink; }
    void setCursorBlink(bool blink);
    bool macModifiers() const { return m_macModifiers; }
    void setMacModifiers(bool swapped);

    int columns() const { return m_columns; }
    int rows() const { return m_rows; }
    qreal cellWidth() const { return m_cellWidth; }
    qreal cellHeight() const { return m_cellHeight; }
    QString title() const { return m_title; }
    QString workingDirectory() const { return m_workingDirectory; }
    bool hasSelection() const { return m_hasSelection; }
    qreal scrollTotal() const { return m_scrollTotal; }
    qreal scrollOffset() const { return m_scrollOffset; }
    bool atBottom() const { return m_scrollOffset + m_rows >= m_scrollTotal; }
    bool mouseTracking() const { return m_mouseTracking; }
    QString error() const { return m_error; }

    // Program output: a string (encoded as UTF-8) or an ArrayBuffer.
    Q_INVOKABLE void write(const QVariant& data);
    // Replays retained history. Queries in it (device status, attributes)
    // are not answered: the program that sent them is gone, and the reply
    // would land at the new shell's prompt.
    Q_INVOKABLE void restore(const QVariant& data);
    // Back to a blank screen with no history, as for a restarted shell.
    Q_INVOKABLE void reset();

    // Sends text as a paste: bracketed when the program asked for it, with
    // unsafe control bytes defused.
    Q_INVOKABLE void paste(const QString& text);
    // Whether the clipboard took the selection: false with nothing selected, or
    // when the clipboard does not read back what was put there.
    Q_INVOKABLE bool copy();
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE QString selectedText() const;
    // Scrollback and screen as plain text, soft wraps joined.
    Q_INVOKABLE QString text() const;

    Q_INVOKABLE void scrollLines(int delta);
    Q_INVOKABLE void scrollTo(qreal offset);
    Q_INVOKABLE void scrollToBottom();

signals:
    void fontChanged();
    void themeChanged();
    void paddingChanged();
    void scrollbackLimitChanged();
    void gridChanged();
    void titleChanged();
    void workingDirectoryChanged();
    void selectionChanged();
    void scrollChanged();
    void cursorBlinkChanged();
    void macModifiersChanged();
    void mouseTrackingChanged();
    void errorChanged();

    // Bytes for the program: keys, pastes, mouse reports and replies to its
    // queries. UTF-8 except for legacy X10 mouse reports.
    void input(const QString& data);
    // The grid changed size; tell the program (TIOCSWINSZ, terminal.resize).
    void resized(int columns, int rows);
    void bell();

protected:
    void componentComplete() override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;
    void itemChange(ItemChange change, const ItemChangeData& value) override;
    void updatePolish() override;
    QSGNode* updatePaintNode(QSGNode* node, UpdatePaintNodeData*) override;

    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseUngrabEvent() override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    friend struct GhosttyCallbacks;

    void createTerminal();
    void destroyTerminal();
    void feed(const QByteArray& bytes, bool answerQueries);
    void flushReplies();
    void emitInput(const QByteArray& bytes);
    void sendUserInput(const QByteArray& bytes);
    void applyTheme();
    void updateMetrics();
    void updateGrid();
    void syncScroll();
    void requestFrame(bool full = false);
    void restartBlink();
    bool blinkAllowed() const;
    void syncMouseTracking();
    void setError(const QString& error);
    void setHasSelection(bool hasSelection);

    bool encodeKey(QKeyEvent* event, bool press);
    bool encodeMouse(int action, int button, Qt::KeyboardModifiers modifiers, QPointF position);
    QPoint cellAt(QPointF position) const;
    bool runGesture(GhosttySelectionGestureEvent event);
    void setGestureGeometry(GhosttySelectionGestureEvent event) const;
    void setGesturePosition(GhosttySelectionGestureEvent event, QPointF position) const;
    void pressSelection(QPointF position);
    void dragSelection(QPointF position);
    void releaseSelection(QPointF position);
    void autoscrollSelection();
    void updateAutoscroll();
    void cancelSelecting();

    void paintRow(QPainter& painter, int y);

    GhosttyTerminal m_terminal = nullptr;
    GhosttyRenderState m_renderState = nullptr;
    GhosttyRenderStateRowIterator m_rowIterator = nullptr;
    GhosttyRenderStateRowCells m_rowCells = nullptr;
    GhosttyKeyEncoder m_keyEncoder = nullptr;
    GhosttyKeyEvent m_keyEvent = nullptr;
    GhosttyMouseEncoder m_mouseEncoder = nullptr;
    GhosttyMouseEvent m_mouseEvent = nullptr;
    GhosttySelectionGesture m_gesture = nullptr;
    GhosttySelectionGestureEvent m_gesturePress = nullptr;
    GhosttySelectionGestureEvent m_gestureRelease = nullptr;
    GhosttySelectionGestureEvent m_gestureDrag = nullptr;
    GhosttySelectionGestureEvent m_gestureTick = nullptr;

    QFont m_font;
    std::array<QFont, 4> m_fonts;       // regular, bold, italic, bold italic
    std::array<QRawFont, 4> m_rawFonts;
    std::array<QHash<char32_t, quint32>, 4> m_glyphs;
    QColor m_foreground{0xd8, 0xd8, 0xd8};
    QColor m_background{0x1d, 0x1f, 0x21};
    QColor m_cursorColor;
    QColor m_selectionColor;
    QVariantList m_ansiColors;
    QByteArray m_defaultPalette;        // Ghostty's own 256 colours
    qreal m_padding = 4;
    int m_scrollbackLimit = 10000;
    bool m_cursorBlink = true;
    bool m_macModifiers = false;
    QString m_error;

    int m_columns = 80;
    int m_rows = 24;
    qreal m_cellWidth = 8;
    qreal m_cellHeight = 16;
    qreal m_ascent = 12;
    qreal m_dpr = 1;
    QString m_title;
    QString m_workingDirectory;
    bool m_hasSelection = false;
    qreal m_scrollTotal = 0;
    qreal m_scrollOffset = 0;
    bool m_mouseTracking = false;

    // Filled by libghostty callbacks while it parses, acted on afterwards.
    QByteArray m_replies;
    bool m_titleDirty = false;
    bool m_pwdDirty = false;
    bool m_bellPending = false;

    QImage m_image;
    bool m_imageChanged = false;
    bool m_fullRepaint = true;
    // Where the cursor was last painted, so its old row is repainted too.
    int m_paintedCursorRow = -1;
    QTimer m_blinkTimer;
    bool m_blinkOn = true;

    // Text the input method is composing, drawn at the cursor until it commits.
    QString m_preedit;

    bool m_selecting = false;
    // Runs only while the pointer is held past the top or bottom edge.
    QTimer m_autoscrollTimer;
    QPointF m_dragPosition;
    QElapsedTimer m_clock;              // times repeat clicks
    Qt::MouseButtons m_trackedButtons;
    qreal m_wheelRemainder = 0;
};
