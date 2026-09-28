#pragma once

#include <QString>
#include <cstdint>

class QKeyEvent;

// What libghostty's key encoder needs from a Qt key event. Qt reports logical
// keys, so the physical key is the US-layout key that produces them; the
// layout's own text still travels in `text`.
struct GhosttyKeyInput {
    int key = 0;                 // GhosttyKey
    uint16_t mods = 0;           // GhosttyMods
    uint16_t consumedMods = 0;   // GhosttyMods the layout used to make `text`
    uint32_t unshiftedCodepoint = 0;
    QByteArray text;             // UTF-8, never C0 controls or DEL
};

GhosttyKeyInput ghosttyKeyInput(const QKeyEvent* event);
