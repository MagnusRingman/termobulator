// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Termobulator Authors.

#include "crash_test_dummy.h"

#include <curses.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace crash_test_dummy {

static std::atomic<bool> g_instance_active{false};

struct CursesGuard {
    SCREEN* scr = nullptr;
    FILE* out_file = nullptr;
    FILE* in_file = nullptr;

    CursesGuard() {
        FILE* log =
            fopen("/home/bmr/src/termobulator/tmp/crash-test-dummy.log", "a");
        if (log) {
            fprintf(log, "CursesGuard: started\n");
            fflush(log);
        }

        if (g_instance_active.exchange(true)) {
            if (log) {
                fprintf(log,
                        "CursesGuard: concurrent active instances error\n");
                fclose(log);
            }
            throw std::runtime_error(
                "crash_test_dummy: concurrent active instances are not "
                "supported");
        }

        int out_fd = dup(1);
        int in_fd = dup(0);
        out_file = fdopen(out_fd, "w");
        in_file = fdopen(in_fd, "r");

        if (!out_file || !in_file) {
            g_instance_active = false;
            if (out_fd >= 0) close(out_fd);
            if (in_fd >= 0) close(in_fd);
            if (log) {
                fprintf(log, "CursesGuard: fdopen failed\n");
                fclose(log);
            }
            throw std::runtime_error("crash_test_dummy: fdopen failed");
        }

        setvbuf(out_file, NULL, _IONBF, 0);
        setvbuf(in_file, NULL, _IONBF, 0);
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);

        const char* term_env = getenv("TERM");
        const char* tdirs_env = getenv("TERMINFO_DIRS");
        if (log) {
            fprintf(log, "CursesGuard: TERM=%s, TERMINFO_DIRS=%s\n",
                    term_env ? term_env : "null",
                    tdirs_env ? tdirs_env : "null");
            fflush(log);
        }

        scr = newterm(nullptr, out_file, in_file);
        if (!scr) {
            g_instance_active = false;
            fclose(out_file);
            fclose(in_file);
            out_file = nullptr;
            in_file = nullptr;
            if (log) {
                fprintf(log, "CursesGuard: newterm failed\n");
                fclose(log);
            }
            throw std::runtime_error("crash_test_dummy: newterm failed");
        }

        if (log) {
            fprintf(log, "CursesGuard: newterm succeeded\n");
            fclose(log);
        }

        set_term(scr);
        noecho();
        cbreak();
        keypad(stdscr, TRUE);
        nodelay(stdscr, TRUE);
        curs_set(0);  // Hide cursor by default
        if (has_colors()) {
            start_color();
            use_default_colors();
        }
    }

    ~CursesGuard() {
        if (scr) {
            endwin();
            delscreen(scr);
        }
        if (out_file) fclose(out_file);
        if (in_file) fclose(in_file);
        g_instance_active = false;
    }
};

class Exhibit {
  public:
    virtual ~Exhibit() = default;
    virtual std::string Name() const = 0;
    virtual void Draw() = 0;
    virtual void HandleInput(int key) {}
};

class CapabilityReportExhibit : public Exhibit {
  public:
    std::string Name() const override { return "capability"; }

    void Draw() override {
        werase(stdscr);
        box(stdscr, 0, 0);

        mvwprintw(stdscr, 1, 2, "EXHIBIT: Capability Report");
        mvwprintw(stdscr, 2, 2, "--------------------------");

        const char* term_env = getenv("TERM");
        mvwprintw(stdscr, 4, 2, "TERM environment variable: %s",
                  term_env ? term_env : "(null)");

        mvwprintw(stdscr, 6, 2, "Colors supported (has_colors): %s",
                  has_colors() ? "YES" : "NO");
        mvwprintw(stdscr, 7, 2, "Can change colors (can_change_color): %s",
                  can_change_color() ? "YES" : "NO");
        mvwprintw(stdscr, 8, 2, "Maximum colors (COLORS): %d", COLORS);
        mvwprintw(stdscr, 9, 2, "Maximum color pairs (COLOR_PAIRS): %d",
                  COLOR_PAIRS);

        mvwprintw(stdscr, 11, 2, "Press Tab to cycle, q to quit");
    }
};

class ColorsExhibit : public Exhibit {
  private:
    bool initialized_ = false;

    void InitColorPairs() {
        if (initialized_) return;
        if (!has_colors()) return;

        int max_pairs = std::min(COLOR_PAIRS - 1, 255);
        for (int i = 1; i <= max_pairs; ++i) {
            init_pair(i, COLOR_BLACK, i);
        }
        initialized_ = true;
    }

  public:
    std::string Name() const override { return "colors"; }

    void Draw() override {
        werase(stdscr);
        box(stdscr, 0, 0);

        mvwprintw(stdscr, 1, 2, "EXHIBIT: Color Rendering");
        mvwprintw(stdscr, 2, 2, "------------------------");

        if (!has_colors()) {
            mvwprintw(stdscr, 4, 4, "Colors: NOT SUPPORTED");
            mvwprintw(stdscr, 6, 4, "Press Tab to cycle, q to quit");
            return;
        }

        InitColorPairs();

        mvwprintw(stdscr, 4, 2, "COLORS available: %d", COLORS);

        if (COLORS <= 8) {
            mvwprintw(stdscr, 6, 2, "Standard 8 colors:");
            for (int i = 0; i < 8; ++i) {
                int pair = i;
                if (pair > 0 && pair < COLOR_PAIRS) {
                    attron(COLOR_PAIR(pair));
                    mvwprintw(stdscr, 8, 4 + i * 4, "  ");
                    attroff(COLOR_PAIR(pair));
                } else {
                    mvwprintw(stdscr, 8, 4 + i * 4, "XX");
                }
                mvwprintw(stdscr, 9, 4 + i * 4, "%02d", i);
            }
        } else if (COLORS <= 16) {
            mvwprintw(stdscr, 6, 2, "Standard + Bright (16 colors):");
            for (int i = 0; i < 16; ++i) {
                int pair = i;
                if (pair > 0 && pair < COLOR_PAIRS) {
                    attron(COLOR_PAIR(pair));
                    mvwprintw(stdscr, 8, 4 + i * 4, "  ");
                    attroff(COLOR_PAIR(pair));
                } else {
                    mvwprintw(stdscr, 8, 4 + i * 4, "XX");
                }
                mvwprintw(stdscr, 9, 4 + i * 4, "%02d", i);
            }
        } else {
            mvwprintw(stdscr, 5, 2, "256-color grid:");
            for (int i = 0; i < 256; ++i) {
                int r = i / 16;
                int c = i % 16;
                int pair = i;

                if (pair > 0 && pair < COLOR_PAIRS) {
                    attron(COLOR_PAIR(pair));
                    mvwprintw(stdscr, 6 + r, 4 + c * 3, "  ");
                    attroff(COLOR_PAIR(pair));
                } else {
                    mvwprintw(stdscr, 6 + r, 4 + c * 3, "XX");
                }
            }
        }

        mvwprintw(stdscr, 22, 2, "Press Tab to cycle, q to quit");
    }
};

class TextStylesExhibit : public Exhibit {
  public:
    std::string Name() const override { return "styles"; }

    void Draw() override {
        werase(stdscr);
        box(stdscr, 0, 0);

        mvwprintw(stdscr, 1, 2, "EXHIBIT: Text Styles");
        mvwprintw(stdscr, 2, 2, "--------------------");

        auto draw_style_line = [](int row, const char* label, attr_t attr) {
            mvwprintw(stdscr, row, 4, "%-12s: ", label);
            attron(attr);
            wprintw(stdscr, "Sample Text in %s style", label);
            attroff(attr);
        };

        draw_style_line(4, "NORMAL", A_NORMAL);
        draw_style_line(6, "BOLD", A_BOLD);
        draw_style_line(8, "UNDERLINE", A_UNDERLINE);
        draw_style_line(10, "REVERSE", A_REVERSE);
        draw_style_line(12, "DIM", A_DIM);
        draw_style_line(14, "BLINK", A_BLINK);
        draw_style_line(16, "STANDOUT", A_STANDOUT);

#ifdef A_ITALIC
        draw_style_line(18, "ITALIC", A_ITALIC);
#else
        mvwprintw(stdscr, 18, 4, "ITALIC      : (A_ITALIC not defined)");
#endif

        mvwprintw(stdscr, 20, 2, "Press Tab to cycle, q to quit");
    }
};

class AcsExhibit : public Exhibit {
  public:
    std::string Name() const override { return "acs"; }

    void Draw() override {
        werase(stdscr);
        box(stdscr, 0, 0);

        mvwprintw(stdscr, 1, 2, "EXHIBIT: Line Drawing / ACS");
        mvwprintw(stdscr, 2, 2, "---------------------------");

        int start_r = 4;
        int start_c = 4;
        int height = 8;
        int width = 30;

        mvwaddch(stdscr, start_r, start_c, ACS_ULCORNER);
        for (int i = 1; i < width - 1; ++i) {
            waddch(stdscr, ACS_HLINE);
        }
        waddch(stdscr, ACS_URCORNER);

        for (int r = 1; r < height - 1; ++r) {
            mvwaddch(stdscr, start_r + r, start_c, ACS_VLINE);
            mvwaddch(stdscr, start_r + r, start_c + width - 1, ACS_VLINE);
        }

        mvwaddch(stdscr, start_r + height - 1, start_c, ACS_LLCORNER);
        for (int i = 1; i < width - 1; ++i) {
            waddch(stdscr, ACS_HLINE);
        }
        waddch(stdscr, ACS_LRCORNER);

        mvwprintw(stdscr, start_r + 2, start_c + 3, "ACS Manual Border");
        mvwprintw(stdscr, start_r + 4, start_c + 3, "Width: %d, Height: %d",
                  width, height);

        mvwprintw(stdscr, 14, 4, "ACS Tree structure:");

        mvwaddch(stdscr, 16, 4, ACS_VLINE);
        mvwprintw(stdscr, 16, 5, " Folder A");

        mvwaddch(stdscr, 17, 4, ACS_LTEE);
        mvwaddch(stdscr, 17, 5, ACS_HLINE);
        mvwprintw(stdscr, 17, 6, " File 1.txt");

        mvwaddch(stdscr, 18, 4, ACS_LTEE);
        mvwaddch(stdscr, 18, 5, ACS_HLINE);
        mvwprintw(stdscr, 18, 6, " File 2.txt");

        mvwaddch(stdscr, 19, 4, ACS_LLCORNER);
        mvwaddch(stdscr, 19, 5, ACS_HLINE);
        mvwprintw(stdscr, 19, 6, " Subfolder B");

        mvwprintw(stdscr, 21, 2, "Press Tab to cycle, q to quit");
    }
};

class InputEchoExhibit : public Exhibit {
  private:
    std::vector<std::string> key_history_;

    std::string GetKeyName(int key) {
        if (key >= 32 && key < 127) {
            return std::string("'") + static_cast<char>(key) + "'";
        }
        switch (key) {
        case '\n':
            return "ENTER (\\n)";
        case '\r':
            return "RETURN (\\r)";
        case '\t':
            return "TAB (\\t)";
        case 27:
            return "ESCAPE (27)";
        case KEY_UP:
            return "KEY_UP";
        case KEY_DOWN:
            return "KEY_DOWN";
        case KEY_LEFT:
            return "KEY_LEFT";
        case KEY_RIGHT:
            return "KEY_RIGHT";
        case KEY_BACKSPACE:
            return "KEY_BACKSPACE";
        default:
            if (key >= KEY_F(1) && key <= KEY_F(20)) {
                return "KEY_F(" + std::to_string(key - KEY_F(0)) + ")";
            }
            return "CODE " + std::to_string(key);
        }
    }

  public:
    std::string Name() const override { return "input"; }

    void HandleInput(int key) override {
        if (key == '\t') return;

        std::string name = GetKeyName(key);
        key_history_.push_back(name);
        if (key_history_.size() > 10) {
            key_history_.erase(key_history_.begin());
        }
    }

    void Draw() override {
        werase(stdscr);
        box(stdscr, 0, 0);

        mvwprintw(stdscr, 1, 2, "EXHIBIT: Input Keystroke Echo");
        mvwprintw(stdscr, 2, 2, "-----------------------------");

        mvwprintw(stdscr, 4, 4,
                  "Press keys to see their curses codes. (Except Tab/q).");
        mvwprintw(stdscr, 6, 4, "Last 10 keystrokes (oldest first):");

        for (size_t i = 0; i < key_history_.size(); ++i) {
            mvwprintw(stdscr, 8 + i, 6, "%2d: %s", static_cast<int>(i + 1),
                      key_history_[i].c_str());
        }

        if (key_history_.empty()) {
            mvwprintw(stdscr, 8, 6, "(no keys received yet)");
        }

        mvwprintw(stdscr, 21, 2, "Press Tab to cycle, q to quit");
    }
};

void Run(int argc, char* argv[]) {
    std::string exhibit_flag;
    bool one_shot = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--one-shot") == 0) {
            one_shot = true;
        } else if (std::strcmp(argv[i], "--exhibit") == 0 && i + 1 < argc) {
            exhibit_flag = argv[i + 1];
            i++;
        }
    }

    CursesGuard guard;

    std::vector<std::unique_ptr<Exhibit>> exhibits;
    exhibits.push_back(std::make_unique<CapabilityReportExhibit>());
    exhibits.push_back(std::make_unique<ColorsExhibit>());
    exhibits.push_back(std::make_unique<TextStylesExhibit>());
    exhibits.push_back(std::make_unique<AcsExhibit>());
    exhibits.push_back(std::make_unique<InputEchoExhibit>());

    int active_idx = 0;
    if (!exhibit_flag.empty()) {
        bool found = false;
        for (size_t i = 0; i < exhibits.size(); ++i) {
            if (exhibits[i]->Name() == exhibit_flag) {
                active_idx = static_cast<int>(i);
                found = true;
                break;
            }
        }
        if (!found) {
            throw std::runtime_error(
                "crash_test_dummy: unknown exhibit name '" + exhibit_flag +
                "'");
        }
    }

    if (one_shot) {
        exhibits[active_idx]->Draw();
        wrefresh(stdscr);
        // Sleep to allow the terminal reader thread to ingest the rendered
        // frame before endwin() is called.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return;
    }

    bool quit = false;
    while (!quit) {
        exhibits[active_idx]->Draw();
        wrefresh(stdscr);

        int ch = getch();
        if (ch != ERR) {
            FILE* log = fopen(
                "/home/bmr/src/termobulator/tmp/crash-test-dummy.log", "a");
            if (log) {
                fprintf(log, "Run loop: ch = %d (char '%c')\n", ch,
                        (ch >= 32 && ch < 127) ? ch : ' ');
                fclose(log);
            }
            if (ch == 'q') {
                quit = true;
            } else if (ch == '\t') {
                active_idx = (active_idx + 1) % exhibits.size();
            } else {
                exhibits[active_idx]->HandleInput(ch);
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

}  // namespace crash_test_dummy
