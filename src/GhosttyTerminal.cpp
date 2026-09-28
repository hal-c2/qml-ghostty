#include "GhosttyTerminal.h"

#include "GhosttyKeys.h"

#include <QClipboard>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGlyphRun>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QPainter>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QStyleHints>
#include <cmath>
#include <vector>

#include <ghostty/vt.h>

namespace {

constexpr int kRegular = 0;
constexpr int kBold = 1;
constexpr int kItalic = 2;

QColor toQColor(GhosttyColorRgb color) { return QColor(color.r, color.g, color.b); }

GhosttyColorRgb toRgb(const QColor& color) {
    return {static_cast<uint8_t>(color.red()), static_cast<uint8_t>(color.green()),
            static_cast<uint8_t>(color.blue())};
}

QColor blend(const QColor& front, const QColor& back, qreal frontWeight) {
    return QColor::fromRgbF(front.redF() * frontWeight + back.redF() * (1 - frontWeight),
                            front.greenF() * frontWeight + back.greenF() * (1 - frontWeight),
                            front.blueF() * frontWeight + back.blueF() * (1 - frontWeight));
}

QByteArray toBytes(const QVariant& data) {
    return data.metaType().id() == QMetaType::QByteArray ? data.toByteArray() : data.toString().toUtf8();
}

QString fromGhostty(uint8_t* bytes, size_t length) {
    QString text = QString::fromUtf8(reinterpret_cast<const char*>(bytes), static_cast<qsizetype>(length));
    ghostty_free(nullptr, bytes, length);
    return text;
}

// One painted cell, gathered before drawing so backgrounds can go down
// before any glyph (wide glyphs spill into the next cell).
struct Cell {
    QColor fg;
    QColor bg;
    bool hasBg = false;
    int variant = kRegular;
    bool invisible = false;
    bool underline = false;
    bool strikethrough = false;
    bool overline = false;
    char32_t codepoint = 0;  // 0 for an empty cell
    QString cluster;         // set when the grapheme has several codepoints
};

}  // namespace

// libghostty calls these while it parses; they only record what happened.
struct GhosttyCallbacks {
    static GhosttyTerminalItem* self(void* userdata) { return static_cast<GhosttyTerminalItem*>(userdata); }

    static void writePty(::GhosttyTerminal, void* userdata, const uint8_t* data, size_t len) {
        if (data != nullptr && len > 0) self(userdata)->m_replies.append(reinterpret_cast<const char*>(data), len);
    }
    static void titleChanged(::GhosttyTerminal, void* userdata) { self(userdata)->m_titleDirty = true; }
    static void pwdChanged(::GhosttyTerminal, void* userdata) { self(userdata)->m_pwdDirty = true; }
    static void bell(::GhosttyTerminal, void* userdata) { self(userdata)->m_bellPending = true; }
};

GhosttyTerminalItem::GhosttyTerminalItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents);
    setFlag(ItemAcceptsInputMethod);
    setFlag(ItemIsFocusScope);
    setActiveFocusOnTab(true);
    setAcceptedMouseButtons(Qt::AllButtons);
    setAcceptHoverEvents(true);
    setCursor(Qt::IBeamCursor);

    m_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_font.setStyleHint(QFont::Monospace);

    m_blinkTimer.setInterval(QGuiApplication::styleHints()->cursorFlashTime() / 2);
    connect(&m_blinkTimer, &QTimer::timeout, this, [this] {
        m_blinkOn = !m_blinkOn;
        requestFrame();
    });

    ghostty_key_encoder_new(nullptr, &m_keyEncoder);
    ghostty_key_event_new(nullptr, &m_keyEvent);
    ghostty_mouse_encoder_new(nullptr, &m_mouseEncoder);
    ghostty_mouse_event_new(nullptr, &m_mouseEvent);
    ghostty_render_state_new(nullptr, &m_renderState);
    ghostty_render_state_row_iterator_new(nullptr, &m_rowIterator);
    ghostty_render_state_row_cells_new(nullptr, &m_rowCells);

    updateMetrics();
    createTerminal();
}

GhosttyTerminalItem::~GhosttyTerminalItem() {
    destroyTerminal();
    ghostty_render_state_row_cells_free(m_rowCells);
    ghostty_render_state_row_iterator_free(m_rowIterator);
    ghostty_render_state_free(m_renderState);
    ghostty_mouse_event_free(m_mouseEvent);
    ghostty_mouse_encoder_free(m_mouseEncoder);
    ghostty_key_event_free(m_keyEvent);
    ghostty_key_encoder_free(m_keyEncoder);
}

void GhosttyTerminalItem::createTerminal() {
    GhosttyTerminalOptions options{};
    options.cols = static_cast<uint16_t>(m_columns);
    options.rows = static_cast<uint16_t>(m_rows);
    options.max_scrollback = static_cast<size_t>(qMax(0, m_scrollbackLimit));
    if (ghostty_terminal_new(nullptr, &m_terminal, options) != GHOSTTY_SUCCESS) {
        qWarning("Ghostty: could not create a terminal");
        m_terminal = nullptr;
        return;
    }
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_USERDATA, this);
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_WRITE_PTY,
                         reinterpret_cast<const void*>(&GhosttyCallbacks::writePty));
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_TITLE_CHANGED,
                         reinterpret_cast<const void*>(&GhosttyCallbacks::titleChanged));
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_PWD_CHANGED,
                         reinterpret_cast<const void*>(&GhosttyCallbacks::pwdChanged));
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_BELL,
                         reinterpret_cast<const void*>(&GhosttyCallbacks::bell));

    if (m_defaultPalette.isEmpty()) {
        m_defaultPalette.resize(sizeof(GhosttyColorRgb) * 256);
        ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_COLOR_PALETTE_DEFAULT, m_defaultPalette.data());
    }
    applyTheme();
    ghostty_terminal_resize(m_terminal, options.cols, options.rows,
                            static_cast<uint32_t>(std::lround(m_cellWidth * m_dpr)),
                            static_cast<uint32_t>(std::lround(m_cellHeight * m_dpr)));
    requestFrame(true);
}

void GhosttyTerminalItem::destroyTerminal() {
    if (m_terminal != nullptr) ghostty_terminal_free(m_terminal);
    m_terminal = nullptr;
}

// --- Properties -------------------------------------------------------------

void GhosttyTerminalItem::setFont(const QFont& font) {
    if (m_font == font) return;
    m_font = font;
    updateMetrics();
    updateGrid();
    requestFrame(true);
    Q_EMIT fontChanged();
}

void GhosttyTerminalItem::setForegroundColor(const QColor& color) {
    if (m_foreground == color) return;
    m_foreground = color;
    applyTheme();
}

void GhosttyTerminalItem::setBackgroundColor(const QColor& color) {
    if (m_background == color) return;
    m_background = color;
    applyTheme();
}

void GhosttyTerminalItem::setCursorColor(const QColor& color) {
    if (m_cursorColor == color) return;
    m_cursorColor = color;
    applyTheme();
}

void GhosttyTerminalItem::setSelectionColor(const QColor& color) {
    if (m_selectionColor == color) return;
    m_selectionColor = color;
    Q_EMIT themeChanged();
    requestFrame(true);
}

void GhosttyTerminalItem::setAnsiColors(const QVariantList& colors) {
    if (m_ansiColors == colors) return;
    m_ansiColors = colors;
    applyTheme();
}

void GhosttyTerminalItem::applyTheme() {
    if (m_terminal != nullptr) {
        const GhosttyColorRgb foreground = toRgb(m_foreground);
        const GhosttyColorRgb background = toRgb(m_background);
        const GhosttyColorRgb cursor = toRgb(m_cursorColor);
        ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND,
                             m_foreground.isValid() ? &foreground : nullptr);
        ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND,
                             m_background.isValid() ? &background : nullptr);
        ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_COLOR_CURSOR,
                             m_cursorColor.isValid() ? &cursor : nullptr);

        GhosttyColorRgb palette[256];
        memcpy(palette, m_defaultPalette.constData(), sizeof palette);
        for (qsizetype index = 0; index < qMin<qsizetype>(m_ansiColors.size(), 256); ++index) {
            const QColor color = m_ansiColors.at(index).value<QColor>();
            if (color.isValid()) palette[index] = toRgb(color);
        }
        ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_COLOR_PALETTE, palette);
    }
    Q_EMIT themeChanged();
    requestFrame(true);
}

void GhosttyTerminalItem::setPadding(qreal padding) {
    if (qFuzzyCompare(m_padding, padding)) return;
    m_padding = padding;
    updateGrid();
    requestFrame(true);
    Q_EMIT paddingChanged();
}

void GhosttyTerminalItem::setScrollbackLimit(int rows) {
    if (m_scrollbackLimit == rows) return;
    m_scrollbackLimit = rows;
    // Ghostty fixes the limit at creation; before anything was written a new
    // terminal loses nothing.
    if (!isComponentComplete()) {
        destroyTerminal();
        createTerminal();
    } else {
        qWarning("Ghostty: scrollbackLimit only applies before the terminal is complete");
    }
    Q_EMIT scrollbackLimitChanged();
}

// --- Geometry ---------------------------------------------------------------

void GhosttyTerminalItem::updateMetrics() {
    m_dpr = window() != nullptr ? window()->effectiveDevicePixelRatio() : qApp->devicePixelRatio();

    for (int variant = 0; variant < 4; ++variant) {
        QFont font = m_font;
        font.setKerning(false);
        font.setStyleStrategy(QFont::PreferAntialias);
        if (variant & kBold) font.setWeight(QFont::Bold);
        if (variant & kItalic) font.setItalic(true);
        m_fonts[variant] = font;
        m_rawFonts[variant] = QRawFont::fromFont(font);
        m_glyphs[variant].clear();
    }

    // Cells snap to device pixels so every row and column lands on the same
    // physical grid.
    const QFontMetricsF metrics(m_fonts[kRegular]);
    m_cellWidth = std::ceil(metrics.horizontalAdvance(QLatin1Char('M')) * m_dpr) / m_dpr;
    m_cellHeight = std::ceil(metrics.height() * m_dpr) / m_dpr;
    m_ascent = std::round(metrics.ascent() * m_dpr) / m_dpr;
}

void GhosttyTerminalItem::updateGrid() {
    const int columns = qBound(1, int((width() - 2 * m_padding) / m_cellWidth), 65535);
    const int rows = qBound(1, int((height() - 2 * m_padding) / m_cellHeight), 65535);
    if (m_terminal != nullptr) {
        ghostty_terminal_resize(m_terminal, static_cast<uint16_t>(columns), static_cast<uint16_t>(rows),
                                static_cast<uint32_t>(std::lround(m_cellWidth * m_dpr)),
                                static_cast<uint32_t>(std::lround(m_cellHeight * m_dpr)));
        flushReplies();
    }
    const bool resized = columns != m_columns || rows != m_rows;
    m_columns = columns;
    m_rows = rows;
    Q_EMIT gridChanged();
    if (resized) {
        syncScroll();
        requestFrame(true);
        Q_EMIT this->resized(columns, rows);
    }
}

void GhosttyTerminalItem::componentComplete() {
    QQuickItem::componentComplete();
    updateGrid();
}

void GhosttyTerminalItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size() && isComponentComplete()) {
        updateGrid();
        requestFrame(true);
    }
}

void GhosttyTerminalItem::itemChange(ItemChange change, const ItemChangeData& value) {
    QQuickItem::itemChange(change, value);
    if (change == ItemDevicePixelRatioHasChanged || (change == ItemSceneChange && value.window != nullptr)) {
        updateMetrics();
        if (isComponentComplete()) updateGrid();
        requestFrame(true);
    }
}

// --- Data in and out --------------------------------------------------------

void GhosttyTerminalItem::write(const QVariant& data) { feed(toBytes(data), true); }

void GhosttyTerminalItem::restore(const QVariant& data) { feed(toBytes(data), false); }

void GhosttyTerminalItem::feed(const QByteArray& bytes, bool answerQueries) {
    if (m_terminal == nullptr || bytes.isEmpty()) return;
    ghostty_terminal_vt_write(m_terminal, reinterpret_cast<const uint8_t*>(bytes.constData()),
                              static_cast<size_t>(bytes.size()));
    if (answerQueries) {
        flushReplies();
    } else {
        m_replies.clear();
    }

    if (m_titleDirty) {
        m_titleDirty = false;
        GhosttyString title{};
        ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_TITLE, &title);
        const QString value = QString::fromUtf8(reinterpret_cast<const char*>(title.ptr), title.len);
        if (value != m_title) {
            m_title = value;
            Q_EMIT titleChanged();
        }
    }
    if (m_pwdDirty) {
        m_pwdDirty = false;
        GhosttyString pwd{};
        ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_PWD, &pwd);
        const QString value = QString::fromUtf8(reinterpret_cast<const char*>(pwd.ptr), pwd.len);
        if (value != m_workingDirectory) {
            m_workingDirectory = value;
            Q_EMIT workingDirectoryChanged();
        }
    }
    if (m_bellPending) {
        m_bellPending = false;
        if (answerQueries) Q_EMIT bell();
    }
    syncScroll();
    requestFrame();
}

void GhosttyTerminalItem::flushReplies() {
    if (m_replies.isEmpty()) return;
    const QByteArray replies = std::exchange(m_replies, {});
    emitInput(replies);
}

void GhosttyTerminalItem::emitInput(const QByteArray& bytes) {
    if (!bytes.isEmpty()) Q_EMIT input(QString::fromUtf8(bytes));
}

// What the user typed or pasted: it brings the viewport back to the prompt.
void GhosttyTerminalItem::sendUserInput(const QByteArray& bytes) {
    if (bytes.isEmpty()) return;
    if (!atBottom()) scrollToBottom();
    restartBlink();
    emitInput(bytes);
}

void GhosttyTerminalItem::reset() {
    if (m_terminal == nullptr) return;
    ghostty_terminal_reset(m_terminal);
    m_replies.clear();
    if (!m_title.isEmpty()) {
        m_title.clear();
        Q_EMIT titleChanged();
    }
    if (m_hasSelection) {
        m_hasSelection = false;
        Q_EMIT selectionChanged();
    }
    syncScroll();
    requestFrame(true);
}

void GhosttyTerminalItem::syncScroll() {
    if (m_terminal == nullptr) return;
    GhosttyTerminalScrollbar scrollbar{};
    if (ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_SCROLLBAR, &scrollbar) != GHOSTTY_SUCCESS) return;
    const qreal total = static_cast<qreal>(scrollbar.total);
    const qreal offset = static_cast<qreal>(scrollbar.offset);
    if (total == m_scrollTotal && offset == m_scrollOffset) return;
    m_scrollTotal = total;
    m_scrollOffset = offset;
    Q_EMIT scrollChanged();
}

void GhosttyTerminalItem::scrollLines(int delta) {
    if (m_terminal == nullptr || delta == 0) return;
    GhosttyTerminalScrollViewport scroll{};
    scroll.tag = GHOSTTY_SCROLL_VIEWPORT_DELTA;
    scroll.value.delta = delta;
    ghostty_terminal_scroll_viewport(m_terminal, scroll);
    syncScroll();
    requestFrame();
}

void GhosttyTerminalItem::scrollTo(qreal offset) { scrollLines(int(std::lround(offset - m_scrollOffset))); }

void GhosttyTerminalItem::scrollToBottom() {
    if (m_terminal == nullptr) return;
    GhosttyTerminalScrollViewport scroll{};
    scroll.tag = GHOSTTY_SCROLL_VIEWPORT_BOTTOM;
    ghostty_terminal_scroll_viewport(m_terminal, scroll);
    syncScroll();
    requestFrame();
}

// --- Clipboard and selection ------------------------------------------------

void GhosttyTerminalItem::paste(const QString& text) {
    if (m_terminal == nullptr || text.isEmpty()) return;
    bool bracketed = false;
    ghostty_terminal_mode_get(m_terminal, GHOSTTY_MODE_BRACKETED_PASTE, &bracketed);
    QByteArray data = text.toUtf8();
    size_t needed = 0;
    ghostty_paste_encode(data.data(), static_cast<size_t>(data.size()), bracketed, nullptr, 0, &needed);
    QByteArray encoded(static_cast<qsizetype>(needed), Qt::Uninitialized);
    size_t written = 0;
    if (ghostty_paste_encode(data.data(), static_cast<size_t>(data.size()), bracketed, encoded.data(),
                             needed, &written) != GHOSTTY_SUCCESS) {
        return;
    }
    encoded.resize(static_cast<qsizetype>(written));
    sendUserInput(encoded);
}

QString GhosttyTerminalItem::selectedText() const {
    if (m_terminal == nullptr || !m_hasSelection) return {};
    GhosttyTerminalSelectionFormatOptions options{};
    options.size = sizeof(options);
    options.emit = GHOSTTY_FORMATTER_FORMAT_PLAIN;
    options.unwrap = true;
    options.trim = true;
    uint8_t* bytes = nullptr;
    size_t length = 0;
    if (ghostty_terminal_selection_format_alloc(m_terminal, nullptr, options, &bytes, &length) != GHOSTTY_SUCCESS ||
        bytes == nullptr) {
        return {};
    }
    return fromGhostty(bytes, length);
}

QString GhosttyTerminalItem::text() const {
    if (m_terminal == nullptr) return {};
    GhosttyFormatterTerminalOptions options = GHOSTTY_INIT_SIZED(GhosttyFormatterTerminalOptions);
    options.emit = GHOSTTY_FORMATTER_FORMAT_PLAIN;
    options.unwrap = true;
    options.trim = true;
    options.extra.size = sizeof(options.extra);
    options.extra.screen.size = sizeof(options.extra.screen);
    GhosttyFormatter formatter = nullptr;
    if (ghostty_formatter_terminal_new(nullptr, &formatter, m_terminal, options) != GHOSTTY_SUCCESS) return {};
    uint8_t* bytes = nullptr;
    size_t length = 0;
    QString result;
    if (ghostty_formatter_format_alloc(formatter, nullptr, &bytes, &length) == GHOSTTY_SUCCESS && bytes != nullptr) {
        result = fromGhostty(bytes, length);
    }
    ghostty_formatter_free(formatter);
    return result;
}

void GhosttyTerminalItem::copy() {
    const QString selection = selectedText();
    if (!selection.isEmpty()) QGuiApplication::clipboard()->setText(selection);
}

void GhosttyTerminalItem::selectAll() {
    if (m_terminal == nullptr) return;
    GhosttySelection selection = GHOSTTY_INIT_SIZED(GhosttySelection);
    if (ghostty_terminal_select_all(m_terminal, &selection) != GHOSTTY_SUCCESS) return;
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_SELECTION, &selection);
    if (!m_hasSelection) {
        m_hasSelection = true;
        Q_EMIT selectionChanged();
    }
    requestFrame(true);
}

void GhosttyTerminalItem::clearSelection() {
    if (m_terminal == nullptr || !m_hasSelection) return;
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_SELECTION, nullptr);
    m_hasSelection = false;
    Q_EMIT selectionChanged();
    requestFrame(true);
}

QPoint GhosttyTerminalItem::cellAt(QPointF position) const {
    const int x = int(std::floor((position.x() - m_padding) / m_cellWidth));
    const int y = int(std::floor((position.y() - m_padding) / m_cellHeight));
    return {qBound(0, x, m_columns - 1), qBound(0, y, m_rows - 1)};
}

namespace {

bool viewportRef(::GhosttyTerminal terminal, QPoint cell, GhosttyGridRef* out) {
    *out = GHOSTTY_INIT_SIZED(GhosttyGridRef);
    GhosttyPoint point{};
    point.tag = GHOSTTY_POINT_TAG_VIEWPORT;
    point.value.coordinate.x = static_cast<uint16_t>(cell.x());
    point.value.coordinate.y = static_cast<uint32_t>(cell.y());
    return ghostty_terminal_grid_ref(terminal, point, out) == GHOSTTY_SUCCESS;
}

}  // namespace

bool GhosttyTerminalItem::setSelection(QPoint anchor, QPoint head) {
    GhosttySelection selection = GHOSTTY_INIT_SIZED(GhosttySelection);
    if (!viewportRef(m_terminal, anchor, &selection.start) || !viewportRef(m_terminal, head, &selection.end)) {
        return false;
    }
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_SELECTION, &selection);
    if (!m_hasSelection) {
        m_hasSelection = true;
        Q_EMIT selectionChanged();
    }
    requestFrame(true);
    return true;
}

// --- Keyboard ---------------------------------------------------------------

bool GhosttyTerminalItem::encodeKey(QKeyEvent* event, bool press) {
    if (m_terminal == nullptr) return false;
    const GhosttyKeyInput key = ghosttyKeyInput(event);
    if (key.key == GHOSTTY_KEY_UNIDENTIFIED && key.text.isEmpty()) return false;

    ghostty_key_encoder_setopt_from_terminal(m_keyEncoder, m_terminal);
    ghostty_key_event_set_action(m_keyEvent, !press                ? GHOSTTY_KEY_ACTION_RELEASE
                                             : event->isAutoRepeat() ? GHOSTTY_KEY_ACTION_REPEAT
                                                                     : GHOSTTY_KEY_ACTION_PRESS);
    ghostty_key_event_set_key(m_keyEvent, static_cast<GhosttyKey>(key.key));
    ghostty_key_event_set_mods(m_keyEvent, key.mods);
    ghostty_key_event_set_consumed_mods(m_keyEvent, key.consumedMods);
    ghostty_key_event_set_composing(m_keyEvent, false);
    ghostty_key_event_set_unshifted_codepoint(m_keyEvent, key.unshiftedCodepoint);
    // The event borrows the text; `key` outlives the encode call.
    ghostty_key_event_set_utf8(m_keyEvent, key.text.isEmpty() ? nullptr : key.text.constData(),
                               static_cast<size_t>(key.text.size()));

    char buffer[128];
    size_t length = 0;
    GhosttyResult result = ghostty_key_encoder_encode(m_keyEncoder, m_keyEvent, buffer, sizeof buffer, &length);
    QByteArray encoded;
    if (result == GHOSTTY_OUT_OF_SPACE) {
        encoded.resize(static_cast<qsizetype>(length));
        result = ghostty_key_encoder_encode(m_keyEncoder, m_keyEvent, encoded.data(), length, &length);
        encoded.resize(static_cast<qsizetype>(length));
    } else {
        encoded = QByteArray(buffer, static_cast<qsizetype>(length));
    }
    ghostty_key_event_set_utf8(m_keyEvent, nullptr, 0);
    if (result != GHOSTTY_SUCCESS) return false;
    if (press) {
        sendUserInput(encoded);
    } else {
        emitInput(encoded);
    }
    return true;
}

void GhosttyTerminalItem::keyPressEvent(QKeyEvent* event) {
    const Qt::KeyboardModifiers modifiers = event->modifiers() & ~Qt::KeypadModifier;
    const int key = event->key();
    // The usual terminal chords: Ctrl+Shift+C/V, Shift+Insert, Shift+PgUp/PgDn.
    if (modifiers == (Qt::ControlModifier | Qt::ShiftModifier) && key == Qt::Key_C) {
        copy();
    } else if ((modifiers == (Qt::ControlModifier | Qt::ShiftModifier) && key == Qt::Key_V) ||
               (modifiers == Qt::ShiftModifier && key == Qt::Key_Insert)) {
        paste(QGuiApplication::clipboard()->text());
    } else if (modifiers == Qt::ShiftModifier && key == Qt::Key_PageUp) {
        scrollLines(-qMax(1, m_rows - 1));
    } else if (modifiers == Qt::ShiftModifier && key == Qt::Key_PageDown) {
        scrollLines(qMax(1, m_rows - 1));
    } else if (!encodeKey(event, true)) {
        event->ignore();
        return;
    }
    event->accept();
}

void GhosttyTerminalItem::keyReleaseEvent(QKeyEvent* event) {
    // Releases only produce bytes under the Kitty protocol's event reporting.
    if (!event->isAutoRepeat()) encodeKey(event, false);
    event->accept();
}

void GhosttyTerminalItem::inputMethodEvent(QInputMethodEvent* event) {
    if (!event->commitString().isEmpty()) sendUserInput(event->commitString().toUtf8());
    event->accept();
}

QVariant GhosttyTerminalItem::inputMethodQuery(Qt::InputMethodQuery query) const {
    switch (query) {
    case Qt::ImEnabled:
        return true;
    case Qt::ImHints:
        return int(Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase | Qt::ImhPreferLatin);
    case Qt::ImCursorRectangle: {
        uint16_t x = 0;
        uint32_t y = 0;
        if (m_terminal != nullptr) {
            ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_CURSOR_X, &x);
            ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_CURSOR_Y, &y);
        }
        return QRectF(m_padding + x * m_cellWidth, m_padding + y * m_cellHeight, m_cellWidth, m_cellHeight);
    }
    case Qt::ImFont:
        return m_font;
    default:
        return QQuickItem::inputMethodQuery(query);
    }
}

void GhosttyTerminalItem::focusInEvent(QFocusEvent* event) {
    QQuickItem::focusInEvent(event);
    bool report = false;
    if (m_terminal != nullptr) ghostty_terminal_mode_get(m_terminal, GHOSTTY_MODE_FOCUS_EVENT, &report);
    if (report) {
        char buffer[8];
        size_t length = 0;
        if (ghostty_focus_encode(GHOSTTY_FOCUS_GAINED, buffer, sizeof buffer, &length) == GHOSTTY_SUCCESS)
            emitInput(QByteArray(buffer, static_cast<qsizetype>(length)));
    }
    restartBlink();
}

void GhosttyTerminalItem::focusOutEvent(QFocusEvent* event) {
    QQuickItem::focusOutEvent(event);
    bool report = false;
    if (m_terminal != nullptr) ghostty_terminal_mode_get(m_terminal, GHOSTTY_MODE_FOCUS_EVENT, &report);
    if (report) {
        char buffer[8];
        size_t length = 0;
        if (ghostty_focus_encode(GHOSTTY_FOCUS_LOST, buffer, sizeof buffer, &length) == GHOSTTY_SUCCESS)
            emitInput(QByteArray(buffer, static_cast<qsizetype>(length)));
    }
    m_blinkTimer.stop();
    m_blinkOn = true;
    requestFrame();
}

// --- Mouse ------------------------------------------------------------------

bool GhosttyTerminalItem::mouseTracking() const {
    bool tracking = false;
    if (m_terminal != nullptr) ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING, &tracking);
    return tracking;
}

namespace {

int ghosttyButton(Qt::MouseButton button) {
    switch (button) {
    case Qt::LeftButton: return GHOSTTY_MOUSE_BUTTON_LEFT;
    case Qt::RightButton: return GHOSTTY_MOUSE_BUTTON_RIGHT;
    case Qt::MiddleButton: return GHOSTTY_MOUSE_BUTTON_MIDDLE;
    case Qt::BackButton: return GHOSTTY_MOUSE_BUTTON_EIGHT;
    case Qt::ForwardButton: return GHOSTTY_MOUSE_BUTTON_NINE;
    default: return GHOSTTY_MOUSE_BUTTON_UNKNOWN;
    }
}

}  // namespace

bool GhosttyTerminalItem::encodeMouse(int action, int button, Qt::KeyboardModifiers modifiers, QPointF position) {
    ghostty_mouse_encoder_setopt_from_terminal(m_mouseEncoder, m_terminal);
    // The encoder works in device pixels, the unit the terminal was sized in.
    GhosttyMouseEncoderSize size = GHOSTTY_INIT_SIZED(GhosttyMouseEncoderSize);
    size.screen_width = static_cast<uint32_t>(std::lround(width() * m_dpr));
    size.screen_height = static_cast<uint32_t>(std::lround(height() * m_dpr));
    size.cell_width = static_cast<uint32_t>(std::lround(m_cellWidth * m_dpr));
    size.cell_height = static_cast<uint32_t>(std::lround(m_cellHeight * m_dpr));
    const auto padding = static_cast<uint32_t>(std::lround(m_padding * m_dpr));
    size.padding_top = size.padding_bottom = size.padding_left = size.padding_right = padding;
    ghostty_mouse_encoder_setopt(m_mouseEncoder, GHOSTTY_MOUSE_ENCODER_OPT_SIZE, &size);
    const bool anyPressed = m_trackedButtons != Qt::NoButton;
    ghostty_mouse_encoder_setopt(m_mouseEncoder, GHOSTTY_MOUSE_ENCODER_OPT_ANY_BUTTON_PRESSED, &anyPressed);

    ghostty_mouse_event_set_action(m_mouseEvent, static_cast<GhosttyMouseAction>(action));
    if (button == GHOSTTY_MOUSE_BUTTON_UNKNOWN) {
        ghostty_mouse_event_clear_button(m_mouseEvent);
    } else {
        ghostty_mouse_event_set_button(m_mouseEvent, static_cast<GhosttyMouseButton>(button));
    }
    GhosttyMods mods = 0;
    if (modifiers & Qt::ShiftModifier) mods |= GHOSTTY_MODS_SHIFT;
    if (modifiers & Qt::ControlModifier) mods |= GHOSTTY_MODS_CTRL;
    if (modifiers & Qt::AltModifier) mods |= GHOSTTY_MODS_ALT;
    if (modifiers & Qt::MetaModifier) mods |= GHOSTTY_MODS_SUPER;
    ghostty_mouse_event_set_mods(m_mouseEvent, mods);
    ghostty_mouse_event_set_position(m_mouseEvent, GhosttyMousePosition{float(position.x() * m_dpr),
                                                                         float(position.y() * m_dpr)});

    char buffer[64];
    size_t length = 0;
    if (ghostty_mouse_encoder_encode(m_mouseEncoder, m_mouseEvent, buffer, sizeof buffer, &length) != GHOSTTY_SUCCESS)
        return false;
    emitInput(QByteArray(buffer, static_cast<qsizetype>(length)));
    return true;
}

void GhosttyTerminalItem::mousePressEvent(QMouseEvent* event) {
    forceActiveFocus(Qt::MouseFocusReason);
    // Shift bypasses mouse reporting so text can still be selected.
    if (mouseTracking() && !(event->modifiers() & Qt::ShiftModifier)) {
        m_trackedButtons |= event->button();
        encodeMouse(GHOSTTY_MOUSE_ACTION_PRESS, ghosttyButton(event->button()), event->modifiers(),
                    event->position());
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        clearSelection();
        m_selecting = true;
        m_selectionAnchor = cellAt(event->position());
    } else if (event->button() == Qt::MiddleButton) {
        paste(QGuiApplication::clipboard()->text(QClipboard::Selection));
    }
    event->accept();
}

void GhosttyTerminalItem::mouseMoveEvent(QMouseEvent* event) {
    if (m_trackedButtons != Qt::NoButton) {
        encodeMouse(GHOSTTY_MOUSE_ACTION_MOTION, GHOSTTY_MOUSE_BUTTON_UNKNOWN, event->modifiers(), event->position());
    } else if (m_selecting) {
        // Dragging past the top or bottom edge scrolls the history along.
        if (event->position().y() < m_padding) scrollLines(-1);
        if (event->position().y() > height() - m_padding) scrollLines(1);
        setSelection(m_selectionAnchor, cellAt(event->position()));
    }
    event->accept();
}

void GhosttyTerminalItem::mouseReleaseEvent(QMouseEvent* event) {
    if (m_trackedButtons & event->button()) {
        m_trackedButtons &= ~event->button();
        encodeMouse(GHOSTTY_MOUSE_ACTION_RELEASE, ghosttyButton(event->button()), event->modifiers(),
                    event->position());
    } else if (m_selecting) {
        m_selecting = false;
        if (m_hasSelection && QGuiApplication::clipboard()->supportsSelection())
            QGuiApplication::clipboard()->setText(selectedText(), QClipboard::Selection);
    }
    event->accept();
}

void GhosttyTerminalItem::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || (mouseTracking() && !(event->modifiers() & Qt::ShiftModifier))) {
        mousePressEvent(event);
        return;
    }
    GhosttyTerminalSelectWordOptions options = GHOSTTY_INIT_SIZED(GhosttyTerminalSelectWordOptions);
    if (!viewportRef(m_terminal, cellAt(event->position()), &options.ref)) return;
    GhosttySelection selection = GHOSTTY_INIT_SIZED(GhosttySelection);
    if (ghostty_terminal_select_word(m_terminal, &options, &selection) != GHOSTTY_SUCCESS) return;
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_SELECTION, &selection);
    if (!m_hasSelection) {
        m_hasSelection = true;
        Q_EMIT selectionChanged();
    }
    if (QGuiApplication::clipboard()->supportsSelection())
        QGuiApplication::clipboard()->setText(selectedText(), QClipboard::Selection);
    requestFrame(true);
    event->accept();
}

void GhosttyTerminalItem::hoverMoveEvent(QHoverEvent* event) {
    // Any-event tracking (mode 1003) wants motion without buttons too.
    if (m_trackedButtons == Qt::NoButton && mouseTracking())
        encodeMouse(GHOSTTY_MOUSE_ACTION_MOTION, GHOSTTY_MOUSE_BUTTON_UNKNOWN, event->modifiers(), event->position());
    QQuickItem::hoverMoveEvent(event);
}

void GhosttyTerminalItem::wheelEvent(QWheelEvent* event) {
    // Touchpads report pixels, wheels report eighths of a degree; both become rows.
    qreal rows = 0;
    if (!event->pixelDelta().isNull()) {
        rows = -event->pixelDelta().y() / m_cellHeight;
    } else {
        rows = -event->angleDelta().y() / 120.0 * 3;
    }
    m_wheelRemainder += rows;
    const int steps = int(m_wheelRemainder);
    m_wheelRemainder -= steps;
    event->accept();
    if (steps == 0 || m_terminal == nullptr) return;

    if (mouseTracking() && !(event->modifiers() & Qt::ShiftModifier)) {
        // Wheel steps are presses of buttons four (up) and five (down).
        const int button = steps < 0 ? GHOSTTY_MOUSE_BUTTON_FOUR : GHOSTTY_MOUSE_BUTTON_FIVE;
        for (int step = 0; step < qMin(qAbs(steps), 10); ++step)
            encodeMouse(GHOSTTY_MOUSE_ACTION_PRESS, button, event->modifiers(), event->position());
        return;
    }

    bool alternateScreen = false;
    bool alternateScroll = false;
    ghostty_terminal_mode_get(m_terminal, GHOSTTY_MODE_ALT_SCREEN_SAVE, &alternateScreen);
    if (!alternateScreen) ghostty_terminal_mode_get(m_terminal, GHOSTTY_MODE_ALT_SCREEN, &alternateScreen);
    ghostty_terminal_mode_get(m_terminal, GHOSTTY_MODE_ALT_SCROLL, &alternateScroll);
    if (alternateScreen && alternateScroll) {
        // Full-screen programs without mouse support (less, man) see arrows.
        bool application = false;
        ghostty_terminal_mode_get(m_terminal, GHOSTTY_MODE_DECCKM, &application);
        const QByteArray arrow = application ? (steps < 0 ? "\x1bOA" : "\x1bOB") : (steps < 0 ? "\x1b[A" : "\x1b[B");
        emitInput(arrow.repeated(qAbs(steps)));
        return;
    }
    scrollLines(steps);
}

// --- Painting ---------------------------------------------------------------

void GhosttyTerminalItem::requestFrame(bool full) {
    m_fullRepaint = m_fullRepaint || full;
    polish();
}

void GhosttyTerminalItem::restartBlink() {
    m_blinkOn = true;
    if (hasActiveFocus()) m_blinkTimer.start();
    requestFrame();
}

void GhosttyTerminalItem::updatePolish() {
    if (m_terminal == nullptr || width() <= 0 || height() <= 0) return;

    const QSize imageSize(qCeil(width() * m_dpr), qCeil(height() * m_dpr));
    if (m_image.size() != imageSize) {
        m_image = QImage(imageSize, QImage::Format_RGB32);
        m_image.setDevicePixelRatio(m_dpr);
        m_fullRepaint = true;
    }

    if (ghostty_render_state_update(m_renderState, m_terminal) != GHOSTTY_SUCCESS) return;
    GhosttyRenderStateDirty dirty = GHOSTTY_RENDER_STATE_DIRTY_FALSE;
    ghostty_render_state_get(m_renderState, GHOSTTY_RENDER_STATE_DATA_DIRTY, &dirty);

    // Where the cursor goes this frame; it is painted into its row, so a
    // move repaints both rows.
    bool cursorVisible = false;
    bool cursorInViewport = false;
    bool cursorBlinks = false;
    uint16_t cursorY = 0;
    ghostty_render_state_get(m_renderState, GHOSTTY_RENDER_STATE_DATA_CURSOR_VISIBLE, &cursorVisible);
    ghostty_render_state_get(m_renderState, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_HAS_VALUE, &cursorInViewport);
    ghostty_render_state_get(m_renderState, GHOSTTY_RENDER_STATE_DATA_CURSOR_BLINKING, &cursorBlinks);
    if (cursorInViewport) ghostty_render_state_get(m_renderState, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_Y, &cursorY);
    if (hasActiveFocus() && cursorBlinks != m_blinkTimer.isActive()) {
        if (cursorBlinks) {
            m_blinkTimer.start();
        } else {
            m_blinkTimer.stop();
            m_blinkOn = true;
        }
    }
    const int cursorRow = cursorVisible && cursorInViewport ? cursorY : -1;

    QPainter painter(&m_image);
    const bool full = m_fullRepaint || dirty == GHOSTTY_RENDER_STATE_DIRTY_FULL;
    if (full) {
        GhosttyRenderStateColors colors = GHOSTTY_INIT_SIZED(GhosttyRenderStateColors);
        ghostty_render_state_colors_get(m_renderState, &colors);
        painter.fillRect(QRectF(0, 0, width(), height()), toQColor(colors.background));
    }

    bool painted = full;
    if (ghostty_render_state_get(m_renderState, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &m_rowIterator) ==
        GHOSTTY_SUCCESS) {
        int y = 0;
        while (ghostty_render_state_row_iterator_next(m_rowIterator)) {
            bool rowDirty = false;
            ghostty_render_state_row_get(m_rowIterator, GHOSTTY_RENDER_STATE_ROW_DATA_DIRTY, &rowDirty);
            // The blinking cursor's row repaints every frame it is asked for.
            if (full || rowDirty || y == cursorRow || y == m_paintedCursorRow) {
                paintRow(painter, y);
                const bool clean = false;
                ghostty_render_state_row_set(m_rowIterator, GHOSTTY_RENDER_STATE_ROW_OPTION_DIRTY, &clean);
                painted = true;
            }
            ++y;
        }
    }
    painter.end();

    const GhosttyRenderStateDirty clean = GHOSTTY_RENDER_STATE_DIRTY_FALSE;
    ghostty_render_state_set(m_renderState, GHOSTTY_RENDER_STATE_OPTION_DIRTY, &clean);
    m_paintedCursorRow = cursorRow;
    m_fullRepaint = false;
    if (painted) {
        m_imageChanged = true;
        update();
    }
}

void GhosttyTerminalItem::paintRow(QPainter& painter, int y) {
    GhosttyRenderStateColors colors = GHOSTTY_INIT_SIZED(GhosttyRenderStateColors);
    ghostty_render_state_colors_get(m_renderState, &colors);
    const QColor defaultFg = toQColor(colors.foreground);
    const QColor defaultBg = toQColor(colors.background);

    const qreal top = m_padding + y * m_cellHeight;
    // The row's band spans the padding too, so a coloured last column or a
    // full-width status bar reaches the edge.
    painter.fillRect(QRectF(0, top, width(), m_cellHeight), defaultBg);

    if (ghostty_render_state_row_get(m_rowIterator, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &m_rowCells) !=
        GHOSTTY_SUCCESS)
        return;

    GhosttyRenderStateRowSelection selection = GHOSTTY_INIT_SIZED(GhosttyRenderStateRowSelection);
    const bool rowSelected = ghostty_render_state_row_get(m_rowIterator, GHOSTTY_RENDER_STATE_ROW_DATA_SELECTION,
                                                          &selection) == GHOSTTY_SUCCESS;

    static thread_local std::vector<Cell> cells;
    cells.assign(static_cast<size_t>(m_columns), Cell{});
    int x = 0;
    while (x < m_columns && ghostty_render_state_row_cells_next(m_rowCells)) {
        Cell& cell = cells[static_cast<size_t>(x)];
        GhosttyColorRgb rgb{};
        cell.fg = ghostty_render_state_row_cells_get(m_rowCells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR, &rgb) ==
                          GHOSTTY_SUCCESS
                      ? toQColor(rgb)
                      : defaultFg;
        cell.hasBg = ghostty_render_state_row_cells_get(m_rowCells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR,
                                                        &rgb) == GHOSTTY_SUCCESS;
        cell.bg = cell.hasBg ? toQColor(rgb) : defaultBg;

        bool styled = false;
        ghostty_render_state_row_cells_get(m_rowCells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_HAS_STYLING, &styled);
        if (styled) {
            GhosttyStyle style = GHOSTTY_INIT_SIZED(GhosttyStyle);
            ghostty_render_state_row_cells_get(m_rowCells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &style);
            if (style.bold) cell.variant |= kBold;
            if (style.italic) cell.variant |= kItalic;
            if (style.inverse) {
                std::swap(cell.fg, cell.bg);
                cell.hasBg = true;
            }
            if (style.faint) cell.fg = blend(cell.fg, cell.bg, 0.6);
            cell.invisible = style.invisible;
            cell.underline = style.underline != 0;
            cell.strikethrough = style.strikethrough;
            cell.overline = style.overline;
        }
        if (rowSelected && x >= selection.start_x && x <= selection.end_x) {
            if (m_selectionColor.isValid()) {
                cell.bg = m_selectionColor;
            } else {
                std::swap(cell.fg, cell.bg);
            }
            cell.hasBg = true;
        }

        uint32_t count = 0;
        ghostty_render_state_row_cells_get(m_rowCells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN, &count);
        if (count == 1) {
            uint32_t codepoint = 0;
            ghostty_render_state_row_cells_get(m_rowCells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF,
                                               &codepoint);
            cell.codepoint = codepoint;
        } else if (count > 1) {
            std::vector<char32_t> buffer(count);
            ghostty_render_state_row_cells_get(m_rowCells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF,
                                               buffer.data());
            cell.codepoint = buffer[0];
            cell.cluster = QString::fromUcs4(buffer.data(), static_cast<qsizetype>(count));
        }
        ++x;
    }

    // Cursor state for this row.
    bool cursorVisible = false;
    bool cursorInViewport = false;
    uint16_t cursorX = 0;
    uint16_t cursorY = 0;
    GhosttyRenderStateCursorVisualStyle cursorStyle = GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK;
    ghostty_render_state_get(m_renderState, GHOSTTY_RENDER_STATE_DATA_CURSOR_VISIBLE, &cursorVisible);
    ghostty_render_state_get(m_renderState, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_HAS_VALUE, &cursorInViewport);
    if (cursorInViewport) {
        ghostty_render_state_get(m_renderState, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_X, &cursorX);
        ghostty_render_state_get(m_renderState, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_Y, &cursorY);
        ghostty_render_state_get(m_renderState, GHOSTTY_RENDER_STATE_DATA_CURSOR_VISUAL_STYLE, &cursorStyle);
    }
    const bool focused = hasActiveFocus();
    const bool drawCursor = cursorVisible && cursorInViewport && cursorY == y && cursorX < m_columns &&
                            (m_blinkOn || !focused);
    const QColor cursorColor = colors.cursor_has_value ? toQColor(colors.cursor) : defaultFg;
    if (drawCursor && focused && cursorStyle == GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK) {
        Cell& cell = cells[cursorX];
        cell.bg = cursorColor;
        cell.hasBg = true;
        cell.fg = defaultBg;
    }

    // Backgrounds, in runs of one colour.
    for (int start = 0; start < m_columns;) {
        const Cell& first = cells[static_cast<size_t>(start)];
        int end = start + 1;
        while (end < m_columns && cells[static_cast<size_t>(end)].hasBg == first.hasBg &&
               cells[static_cast<size_t>(end)].bg == first.bg)
            ++end;
        if (first.hasBg && first.bg != defaultBg) {
            const qreal left = start == 0 ? 0 : m_padding + start * m_cellWidth;
            const qreal right = end == m_columns ? width() : m_padding + end * m_cellWidth;
            painter.fillRect(QRectF(left, top, right - left, m_cellHeight), first.bg);
        }
        start = end;
    }

    // Glyphs: everything the primary font covers goes out as one positioned
    // glyph run per colour and weight; the rest falls back to Qt's font
    // matching one cell at a time.
    struct Run {
        int variant;
        QRgb color;
        QList<quint32> glyphs;
        QList<QPointF> positions;
    };
    QList<Run> runs;
    const qreal baseline = top + m_ascent;
    for (int column = 0; column < m_columns; ++column) {
        const Cell& cell = cells[static_cast<size_t>(column)];
        if (cell.codepoint == 0 || cell.codepoint == U' ' || cell.invisible) continue;
        const QPointF origin(m_padding + column * m_cellWidth, baseline);
        const QRawFont& raw = m_rawFonts[cell.variant];
        if (cell.cluster.isEmpty() && raw.isValid()) {
            auto& cache = m_glyphs[cell.variant];
            auto found = cache.constFind(cell.codepoint);
            if (found == cache.constEnd()) {
                quint32 glyph = 0;
                if (raw.supportsCharacter(cell.codepoint)) {
                    const QList<quint32> indexes = raw.glyphIndexesForString(QString::fromUcs4(&cell.codepoint, 1));
                    glyph = indexes.value(0);
                }
                found = cache.insert(cell.codepoint, glyph);
            }
            if (*found != 0) {
                const QRgb color = cell.fg.rgb();
                auto run = std::find_if(runs.begin(), runs.end(), [&](const Run& candidate) {
                    return candidate.variant == cell.variant && candidate.color == color;
                });
                if (run == runs.end()) run = runs.insert(runs.end(), Run{cell.variant, color, {}, {}});
                run->glyphs.append(*found);
                run->positions.append(origin);
                continue;
            }
        }
        painter.setFont(m_fonts[cell.variant]);
        painter.setPen(cell.fg);
        painter.drawText(origin, cell.cluster.isEmpty() ? QString::fromUcs4(&cell.codepoint, 1) : cell.cluster);
    }
    for (const Run& run : runs) {
        QGlyphRun glyphs;
        glyphs.setRawFont(m_rawFonts[run.variant]);
        glyphs.setGlyphIndexes(run.glyphs);
        glyphs.setPositions(run.positions);
        painter.setPen(QColor::fromRgb(run.color));
        painter.drawGlyphRun(QPointF(0, 0), glyphs);
    }

    // Decorations.
    const qreal line = qMax(1.0, std::round(m_cellHeight / 16 * m_dpr) / m_dpr);
    for (int column = 0; column < m_columns; ++column) {
        const Cell& cell = cells[static_cast<size_t>(column)];
        if (!(cell.underline || cell.strikethrough || cell.overline)) continue;
        const qreal left = m_padding + column * m_cellWidth;
        if (cell.underline) painter.fillRect(QRectF(left, baseline + line, m_cellWidth, line), cell.fg);
        if (cell.strikethrough)
            painter.fillRect(QRectF(left, top + m_cellHeight / 2, m_cellWidth, line), cell.fg);
        if (cell.overline) painter.fillRect(QRectF(left, top, m_cellWidth, line), cell.fg);
    }

    // Non-block cursors sit over the glyph; an unfocused one is hollow.
    if (drawCursor && !(focused && cursorStyle == GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK)) {
        const QRectF cell(m_padding + cursorX * m_cellWidth, top, m_cellWidth, m_cellHeight);
        const qreal stroke = qMax(1.0, std::round(m_dpr) / m_dpr);
        if (!focused || cursorStyle == GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK_HOLLOW) {
            painter.setPen(QPen(cursorColor, stroke));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(cell.adjusted(stroke / 2, stroke / 2, -stroke / 2, -stroke / 2));
        } else if (cursorStyle == GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BAR) {
            painter.fillRect(QRectF(cell.left(), cell.top(), 2 * stroke, cell.height()), cursorColor);
        } else {
            painter.fillRect(QRectF(cell.left(), cell.bottom() - 2 * stroke, cell.width(), 2 * stroke), cursorColor);
        }
    }
}

QSGNode* GhosttyTerminalItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    auto* node = static_cast<QSGSimpleTextureNode*>(oldNode);
    if (m_image.isNull()) {
        delete node;
        return nullptr;
    }
    if (node == nullptr) {
        node = new QSGSimpleTextureNode();
        node->setOwnsTexture(true);
        node->setFiltering(QSGTexture::Nearest);
        m_imageChanged = true;
    }
    if (m_imageChanged) {
        m_imageChanged = false;
        node->setTexture(window()->createTextureFromImage(m_image));
    }
    node->setRect(QRectF(QPointF(0, 0), QSizeF(m_image.size()) / m_dpr));
    return node;
}
