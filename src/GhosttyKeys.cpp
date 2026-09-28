#include "GhosttyKeys.h"

#include <QKeyEvent>
#include <ghostty/vt.h>

namespace {

struct KeyMapping {
    Qt::Key qt;
    GhosttyKey key;
    char32_t unshifted;
};

// Keys whose Qt value is not a letter or digit. Shifted punctuation maps back
// to the US key that types it, so the Kitty protocol sees the base key.
constexpr KeyMapping kKeys[] = {
    {Qt::Key_Escape, GHOSTTY_KEY_ESCAPE, 0},
    {Qt::Key_Tab, GHOSTTY_KEY_TAB, 0},
    {Qt::Key_Backtab, GHOSTTY_KEY_TAB, 0},
    {Qt::Key_Backspace, GHOSTTY_KEY_BACKSPACE, 0},
    {Qt::Key_Return, GHOSTTY_KEY_ENTER, 0},
    {Qt::Key_Enter, GHOSTTY_KEY_NUMPAD_ENTER, 0},
    {Qt::Key_Insert, GHOSTTY_KEY_INSERT, 0},
    {Qt::Key_Delete, GHOSTTY_KEY_DELETE, 0},
    {Qt::Key_Pause, GHOSTTY_KEY_PAUSE, 0},
    {Qt::Key_Print, GHOSTTY_KEY_PRINT_SCREEN, 0},
    {Qt::Key_Home, GHOSTTY_KEY_HOME, 0},
    {Qt::Key_End, GHOSTTY_KEY_END, 0},
    {Qt::Key_Left, GHOSTTY_KEY_ARROW_LEFT, 0},
    {Qt::Key_Up, GHOSTTY_KEY_ARROW_UP, 0},
    {Qt::Key_Right, GHOSTTY_KEY_ARROW_RIGHT, 0},
    {Qt::Key_Down, GHOSTTY_KEY_ARROW_DOWN, 0},
    {Qt::Key_PageUp, GHOSTTY_KEY_PAGE_UP, 0},
    {Qt::Key_PageDown, GHOSTTY_KEY_PAGE_DOWN, 0},
    {Qt::Key_Shift, GHOSTTY_KEY_SHIFT_LEFT, 0},
    {Qt::Key_Control, GHOSTTY_KEY_CONTROL_LEFT, 0},
    {Qt::Key_Meta, GHOSTTY_KEY_META_LEFT, 0},
    {Qt::Key_Alt, GHOSTTY_KEY_ALT_LEFT, 0},
    {Qt::Key_AltGr, GHOSTTY_KEY_ALT_RIGHT, 0},
    {Qt::Key_CapsLock, GHOSTTY_KEY_CAPS_LOCK, 0},
    {Qt::Key_NumLock, GHOSTTY_KEY_NUM_LOCK, 0},
    {Qt::Key_ScrollLock, GHOSTTY_KEY_SCROLL_LOCK, 0},
    {Qt::Key_Menu, GHOSTTY_KEY_CONTEXT_MENU, 0},
    {Qt::Key_Help, GHOSTTY_KEY_HELP, 0},
    {Qt::Key_Space, GHOSTTY_KEY_SPACE, U' '},
    {Qt::Key_Exclam, GHOSTTY_KEY_DIGIT_1, U'1'},
    {Qt::Key_At, GHOSTTY_KEY_DIGIT_2, U'2'},
    {Qt::Key_NumberSign, GHOSTTY_KEY_DIGIT_3, U'3'},
    {Qt::Key_Dollar, GHOSTTY_KEY_DIGIT_4, U'4'},
    {Qt::Key_Percent, GHOSTTY_KEY_DIGIT_5, U'5'},
    {Qt::Key_AsciiCircum, GHOSTTY_KEY_DIGIT_6, U'6'},
    {Qt::Key_Ampersand, GHOSTTY_KEY_DIGIT_7, U'7'},
    {Qt::Key_Asterisk, GHOSTTY_KEY_DIGIT_8, U'8'},
    {Qt::Key_ParenLeft, GHOSTTY_KEY_DIGIT_9, U'9'},
    {Qt::Key_ParenRight, GHOSTTY_KEY_DIGIT_0, U'0'},
    {Qt::Key_Minus, GHOSTTY_KEY_MINUS, U'-'},
    {Qt::Key_Underscore, GHOSTTY_KEY_MINUS, U'-'},
    {Qt::Key_Equal, GHOSTTY_KEY_EQUAL, U'='},
    {Qt::Key_Plus, GHOSTTY_KEY_EQUAL, U'='},
    {Qt::Key_BracketLeft, GHOSTTY_KEY_BRACKET_LEFT, U'['},
    {Qt::Key_BraceLeft, GHOSTTY_KEY_BRACKET_LEFT, U'['},
    {Qt::Key_BracketRight, GHOSTTY_KEY_BRACKET_RIGHT, U']'},
    {Qt::Key_BraceRight, GHOSTTY_KEY_BRACKET_RIGHT, U']'},
    {Qt::Key_Backslash, GHOSTTY_KEY_BACKSLASH, U'\\'},
    {Qt::Key_Bar, GHOSTTY_KEY_BACKSLASH, U'\\'},
    {Qt::Key_Semicolon, GHOSTTY_KEY_SEMICOLON, U';'},
    {Qt::Key_Colon, GHOSTTY_KEY_SEMICOLON, U';'},
    {Qt::Key_Apostrophe, GHOSTTY_KEY_QUOTE, U'\''},
    {Qt::Key_QuoteDbl, GHOSTTY_KEY_QUOTE, U'\''},
    {Qt::Key_QuoteLeft, GHOSTTY_KEY_BACKQUOTE, U'`'},
    {Qt::Key_AsciiTilde, GHOSTTY_KEY_BACKQUOTE, U'`'},
    {Qt::Key_Comma, GHOSTTY_KEY_COMMA, U','},
    {Qt::Key_Less, GHOSTTY_KEY_COMMA, U','},
    {Qt::Key_Period, GHOSTTY_KEY_PERIOD, U'.'},
    {Qt::Key_Greater, GHOSTTY_KEY_PERIOD, U'.'},
    {Qt::Key_Slash, GHOSTTY_KEY_SLASH, U'/'},
    {Qt::Key_Question, GHOSTTY_KEY_SLASH, U'/'},
};

GhosttyKey keypadKey(int qtKey) {
    if (qtKey >= Qt::Key_0 && qtKey <= Qt::Key_9) {
        return static_cast<GhosttyKey>(GHOSTTY_KEY_NUMPAD_0 + (qtKey - Qt::Key_0));
    }
    switch (qtKey) {
    case Qt::Key_Plus: return GHOSTTY_KEY_NUMPAD_ADD;
    case Qt::Key_Minus: return GHOSTTY_KEY_NUMPAD_SUBTRACT;
    case Qt::Key_Asterisk: return GHOSTTY_KEY_NUMPAD_MULTIPLY;
    case Qt::Key_Slash: return GHOSTTY_KEY_NUMPAD_DIVIDE;
    case Qt::Key_Period: return GHOSTTY_KEY_NUMPAD_DECIMAL;
    case Qt::Key_Comma: return GHOSTTY_KEY_NUMPAD_COMMA;
    case Qt::Key_Equal: return GHOSTTY_KEY_NUMPAD_EQUAL;
    case Qt::Key_Enter:
    case Qt::Key_Return: return GHOSTTY_KEY_NUMPAD_ENTER;
    case Qt::Key_Home: return GHOSTTY_KEY_NUMPAD_HOME;
    case Qt::Key_End: return GHOSTTY_KEY_NUMPAD_END;
    case Qt::Key_Up: return GHOSTTY_KEY_NUMPAD_UP;
    case Qt::Key_Down: return GHOSTTY_KEY_NUMPAD_DOWN;
    case Qt::Key_Left: return GHOSTTY_KEY_NUMPAD_LEFT;
    case Qt::Key_Right: return GHOSTTY_KEY_NUMPAD_RIGHT;
    case Qt::Key_PageUp: return GHOSTTY_KEY_NUMPAD_PAGE_UP;
    case Qt::Key_PageDown: return GHOSTTY_KEY_NUMPAD_PAGE_DOWN;
    case Qt::Key_Insert: return GHOSTTY_KEY_NUMPAD_INSERT;
    case Qt::Key_Delete: return GHOSTTY_KEY_NUMPAD_DELETE;
    case Qt::Key_Clear: return GHOSTTY_KEY_NUMPAD_BEGIN;
    default: return GHOSTTY_KEY_UNIDENTIFIED;
    }
}

}  // namespace

GhosttyKeyInput ghosttyKeyInput(const QKeyEvent* event) {
    GhosttyKeyInput input;
    const int qtKey = event->key();
    const Qt::KeyboardModifiers modifiers = event->modifiers();

    if (modifiers & Qt::ShiftModifier) input.mods |= GHOSTTY_MODS_SHIFT;
    if (modifiers & Qt::ControlModifier) input.mods |= GHOSTTY_MODS_CTRL;
    if (modifiers & Qt::AltModifier) input.mods |= GHOSTTY_MODS_ALT;
    if (modifiers & Qt::MetaModifier) input.mods |= GHOSTTY_MODS_SUPER;

    GhosttyKey key = GHOSTTY_KEY_UNIDENTIFIED;
    if (modifiers & Qt::KeypadModifier) key = keypadKey(qtKey);

    if (key == GHOSTTY_KEY_UNIDENTIFIED) {
        if (qtKey >= Qt::Key_A && qtKey <= Qt::Key_Z) {
            key = static_cast<GhosttyKey>(GHOSTTY_KEY_A + (qtKey - Qt::Key_A));
            input.unshiftedCodepoint = U'a' + (qtKey - Qt::Key_A);
        } else if (qtKey >= Qt::Key_0 && qtKey <= Qt::Key_9) {
            key = static_cast<GhosttyKey>(GHOSTTY_KEY_DIGIT_0 + (qtKey - Qt::Key_0));
            input.unshiftedCodepoint = U'0' + (qtKey - Qt::Key_0);
        } else if (qtKey >= Qt::Key_F1 && qtKey <= Qt::Key_F25) {
            key = static_cast<GhosttyKey>(GHOSTTY_KEY_F1 + (qtKey - Qt::Key_F1));
        } else {
            for (const auto& mapping : kKeys) {
                if (mapping.qt == qtKey) {
                    key = mapping.key;
                    input.unshiftedCodepoint = mapping.unshifted;
                    break;
                }
            }
        }
    }
    input.key = key;
    // Backtab is Qt's name for Shift+Tab; make sure the shift survives.
    if (qtKey == Qt::Key_Backtab) input.mods |= GHOSTTY_MODS_SHIFT;

    // The encoder derives control sequences from key and mods itself and
    // must not see the C0 bytes Qt puts in text for Ctrl+letter.
    const QString text = event->text();
    bool printable = !text.isEmpty();
    for (const QChar c : text) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7f) {
            printable = false;
            break;
        }
    }
    if (printable) {
        input.text = text.toUtf8();
        if (input.unshiftedCodepoint == 0 && !(input.mods & GHOSTTY_MODS_SHIFT)) {
            input.unshiftedCodepoint = text.toUcs4().value(0);
        }
        input.consumedMods = input.mods & GHOSTTY_MODS_SHIFT;
    }
    return input;
}
