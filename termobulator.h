// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Termobulator Authors.

#ifndef TERMOBULATOR_H
#define TERMOBULATOR_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace termobulator {
namespace unstable {

struct CellAttr {
    int fccode = -1;
    int bccode = -1;
    uint8_t fr = 0;
    uint8_t fg = 0;
    uint8_t fb = 0;
    uint8_t br = 0;
    uint8_t bg = 0;
    uint8_t bb = 0;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool inverse = false;
    bool protect = false;
    bool blink = false;

    bool operator==(const CellAttr &o) const {
        if (bold != o.bold || italic != o.italic || underline != o.underline ||
            inverse != o.inverse || protect != o.protect || blink != o.blink)
            return false;
        if (fccode != o.fccode || bccode != o.bccode) return false;
        if (fccode == -1 && (fr != o.fr || fg != o.fg || fb != o.fb))
            return false;
        if (bccode == -1 && (br != o.br || bg != o.bg || bb != o.bb))
            return false;
        return true;
    }
    bool operator!=(const CellAttr &o) const { return !(*this == o); }

    // Strict weak ordering consistent with operator==.
    bool operator<(const CellAttr &o) const {
        if (fccode != o.fccode) return fccode < o.fccode;
        if (bccode != o.bccode) return bccode < o.bccode;
        if (fccode == -1) {
            if (fr != o.fr) return fr < o.fr;
            if (fg != o.fg) return fg < o.fg;
            if (fb != o.fb) return fb < o.fb;
        }
        if (bccode == -1) {
            if (br != o.br) return br < o.br;
            if (bg != o.bg) return bg < o.bg;
            if (bb != o.bb) return bb < o.bb;
        }
        auto flags = [](const CellAttr &a) {
            return (a.bold ? 32 : 0) | (a.italic ? 16 : 0) |
                   (a.underline ? 8 : 0) | (a.inverse ? 4 : 0) |
                   (a.protect ? 2 : 0) | (a.blink ? 1 : 0);
        };
        return flags(*this) < flags(o);
    }
};

struct Cell {
    std::string ch;
    CellAttr attr;
};

struct ScreenSnapshot {
    unsigned int width = 0;
    unsigned int height = 0;
    unsigned int cursor_x = 0;
    unsigned int cursor_y = 0;
    bool cursor_hidden = false;
    std::vector<Cell> cells;
};

enum class WaitResult { kIdle, kDeadline, kExited };

class Terminal {
  public:
    virtual ~Terminal() = default;

    virtual void SendRawBytes(std::string_view bytes) = 0;
    // snapshot_id == -1 means current screen state (not a stored snapshot).
    virtual std::string DumpScreen(int snapshot_id) = 0;
    std::string DumpScreen() { return DumpScreen(-1); }
    virtual int Snapshot() = 0;
    virtual ScreenSnapshot GetSnapshot(int snapshot_id) = 0;
    ScreenSnapshot GetSnapshot() { return GetSnapshot(-1); }
    virtual void SendKey(uint32_t keysym, unsigned int mods) = 0;
    void SendKey(uint32_t keysym) { SendKey(keysym, 0); }

    virtual unsigned int Width() const = 0;
    virtual unsigned int Height() const = 0;

    virtual bool IsExited() const = 0;
    virtual int ExitStatus() const = 0;
    virtual void SendSignal(int sig) = 0;
    virtual void Resize(unsigned int width, unsigned int height) = 0;
    virtual WaitResult WaitIdle(unsigned int quiet_ms,
                                unsigned int deadline_ms) = 0;

    // Static utilities
    static uint32_t ParseKeysym(std::string_view name);
    static std::vector<std::string> GetKeysyms();
    static unsigned int ParseMods(const std::vector<std::string> &mod_args,
                                  size_t start_idx = 0);
    static std::string ParseEscapes(std::string_view input);
};

// Factory functions
std::unique_ptr<Terminal> CreateSubprocessTerminal(
    unsigned int width, unsigned int height, const std::string &cmd,
    const std::vector<std::string> &args,
    const std::string &term_type = "tmux-256color",
    const std::string &locale = "");

std::unique_ptr<Terminal> CreateThreadTerminal(
    unsigned int width, unsigned int height,
    std::function<void()> client_func);

}  // namespace unstable
}  // namespace termobulator

#endif  // TERMOBULATOR_H
