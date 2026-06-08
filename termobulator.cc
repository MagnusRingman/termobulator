// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The Termobulator Authors.

#include "termobulator.h"

#include <fcntl.h>
#include <libtsm.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace termobulator {
namespace unstable {

static CellAttr MapAttr(const tsm_screen_attr &attr) {
    CellAttr res;
    res.fccode = attr.fccode;
    res.bccode = attr.bccode;
    res.fr = attr.fr;
    res.fg = attr.fg;
    res.fb = attr.fb;
    res.br = attr.br;
    res.bg = attr.bg;
    res.bb = attr.bb;
    res.bold = attr.bold;
    res.italic = attr.italic;
    res.underline = attr.underline;
    res.inverse = attr.inverse;
    res.protect = attr.protect;
    res.blink = attr.blink;
    return res;
}

static void WriteAll(int fd, const char *data, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, data, len);
        if (n <= 0) break;
        data += n;
        len -= static_cast<size_t>(n);
    }
}

// Returns {master_fd, slave_fd}. Throws on failure.
static std::pair<int, int> OpenPty() {
    int master_fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (master_fd < 0) throw std::runtime_error("posix_openpt failed");
    if (grantpt(master_fd) < 0 || unlockpt(master_fd) < 0) {
        close(master_fd);
        throw std::runtime_error("grantpt/unlockpt failed");
    }
    char *slave_name = ptsname(master_fd);
    if (!slave_name) {
        close(master_fd);
        throw std::runtime_error("ptsname failed");
    }
    int slave_fd = open(slave_name, O_RDWR);
    if (slave_fd < 0) {
        close(master_fd);
        throw std::runtime_error("failed to open slave pty");
    }
    return {master_fd, slave_fd};
}

static void SetWinsize(int fd, unsigned int width, unsigned int height) {
    struct winsize ws{};
    ws.ws_col = static_cast<uint16_t>(width);
    ws.ws_row = static_cast<uint16_t>(height);
    ioctl(fd, TIOCSWINSZ, &ws);
}

class TerminalBase : public Terminal {
  public:
    TerminalBase(unsigned int width, unsigned int height);
    ~TerminalBase() override;

    void SendRawBytes(std::string_view bytes) override;
    std::string DumpScreen(int snapshot_id) override;
    int Snapshot() override;
    ScreenSnapshot GetSnapshot(int snapshot_id) override;
    void SendKey(uint32_t keysym, unsigned int mods) override;

    unsigned int Width() const override;
    unsigned int Height() const override;
    void Resize(unsigned int width, unsigned int height) override;
    WaitResult WaitIdle(unsigned int quiet_ms,
                        unsigned int deadline_ms) override;

  protected:
    void FeedInput(const char *data, size_t len);

    struct tsm_screen *screen_ = nullptr;
    struct tsm_vte *vte_ = nullptr;
    std::atomic<int> master_fd_{-1};
    mutable std::mutex mutex_;
    std::vector<ScreenSnapshot> snapshots_;
    std::mutex pty_update_mutex_;
    std::condition_variable pty_update_cv_;
    std::chrono::steady_clock::time_point last_update_time_;

  private:
    ScreenSnapshot CaptureCurrent();
    static std::string RenderSnapshot(const ScreenSnapshot &snap);
    static void WriteCb(struct tsm_vte *vte, const char *u8, size_t len,
                        void *data);

    struct DrawData {
        unsigned int width;
        unsigned int height;
        std::vector<Cell> &cells;
    };

    static int DrawCb(struct tsm_screen *con, uint64_t id, const uint32_t *ch,
                      size_t len, unsigned int cell_width, unsigned int posx,
                      unsigned int posy, const struct tsm_screen_attr *attr,
                      tsm_age_t age, void *data);
};

TerminalBase::TerminalBase(unsigned int width, unsigned int height) {
    int r = tsm_screen_new(&screen_, nullptr, nullptr);
    if (r < 0) {
        throw std::runtime_error("failed to create tsm_screen");
    }
    tsm_screen_resize(screen_, width, height);

    r = tsm_vte_new(&vte_, screen_, WriteCb, this, nullptr, nullptr);
    if (r < 0) {
        tsm_screen_unref(screen_);
        throw std::runtime_error("failed to create tsm_vte");
    }
}

TerminalBase::~TerminalBase() {
    tsm_vte_unref(vte_);
    tsm_screen_unref(screen_);
}

void TerminalBase::Resize(unsigned int width, unsigned int height) {
    std::lock_guard<std::mutex> lock(mutex_);
    tsm_screen_resize(screen_, width, height);
}

void TerminalBase::FeedInput(const char *data, size_t len) {
    std::lock_guard<std::mutex> lock(mutex_);
    tsm_vte_input(vte_, data, len);
}

void TerminalBase::SendRawBytes(std::string_view bytes) {
    int fd = master_fd_.load();
    if (fd >= 0) {
        WriteAll(fd, bytes.data(), bytes.size());
    }
}

WaitResult TerminalBase::WaitIdle(unsigned int quiet_ms,
                                  unsigned int deadline_ms) {
    auto start_time = std::chrono::steady_clock::now();
    auto q_dur = std::chrono::milliseconds(quiet_ms);
    auto d_dur = std::chrono::milliseconds(deadline_ms);

    std::unique_lock<std::mutex> lock(pty_update_mutex_);
    if (last_update_time_ < start_time) last_update_time_ = start_time;
    while (true) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed_quiet = now - last_update_time_;
        auto elapsed_wait = now - start_time;

        if (IsExited() || elapsed_quiet >= q_dur) {
            break;
        }
        if (elapsed_wait >= d_dur) {
            break;
        }

        auto remain_q =
            q_dur - std::chrono::duration_cast<std::chrono::milliseconds>(
                        elapsed_quiet);
        auto remain_d =
            d_dur - std::chrono::duration_cast<std::chrono::milliseconds>(
                        elapsed_wait);
        auto wait_dur = std::min(remain_q, remain_d);

        pty_update_cv_.wait_for(lock, wait_dur);
    }
    // Capture last_update_time_ while still holding the lock to avoid a race.
    auto last = last_update_time_;
    lock.unlock();
    // Check idle first: if the screen is stable, report that regardless of
    // exit state so the caller can use the screen content confidently.
    if (std::chrono::steady_clock::now() - last >= q_dur) {
        return WaitResult::kIdle;
    }
    if (IsExited()) {
        return WaitResult::kExited;
    }
    return WaitResult::kDeadline;
}

int TerminalBase::DrawCb(struct tsm_screen *con, uint64_t id,
                         const uint32_t *ch, size_t len,
                         unsigned int cell_width, unsigned int posx,
                         unsigned int posy, const struct tsm_screen_attr *attr,
                         tsm_age_t age, void *data) {
    auto *draw_data = static_cast<DrawData *>(data);
    if (posy >= draw_data->height || posx >= draw_data->width) return 0;

    std::string s;
    if (len == 0) {
        s = " ";
    } else {
        for (size_t i = 0; i < len; ++i) {
            std::array<char, 8> buf;
            size_t bytes = tsm_ucs4_to_utf8(ch[i], buf.data());
            if (bytes > 0 && bytes < buf.size()) s.append(buf.data(), bytes);
        }
    }
    unsigned int base_idx = posy * draw_data->width + posx;
    draw_data->cells[base_idx].ch = std::move(s);
    draw_data->cells[base_idx].attr = MapAttr(*attr);
    for (unsigned int i = 1; i < cell_width && (posx + i) < draw_data->width;
         ++i) {
        draw_data->cells[base_idx + i].ch.clear();
        draw_data->cells[base_idx + i].attr = MapAttr(*attr);
    }
    return 0;
}

ScreenSnapshot TerminalBase::CaptureCurrent() {
    unsigned int w = tsm_screen_get_width(screen_);
    unsigned int h = tsm_screen_get_height(screen_);
    ScreenSnapshot snap{
        w,
        h,
        tsm_screen_get_cursor_x(screen_),
        tsm_screen_get_cursor_y(screen_),
        (tsm_screen_get_flags(screen_) & TSM_SCREEN_HIDE_CURSOR) != 0,
        std::vector<Cell>(w * h, Cell{" ", CellAttr{}})};

    DrawData draw_data{snap.width, snap.height, snap.cells};
    tsm_screen_draw(screen_, DrawCb, &draw_data);
    return snap;
}

std::string TerminalBase::RenderSnapshot(const ScreenSnapshot &snap) {
    std::string result;
    result += "+";
    for (unsigned int x = 0; x < snap.width; ++x) result += "-";
    result += "+\n";

    for (unsigned int y = 0; y < snap.height; ++y) {
        result += "|";
        for (unsigned int x = 0; x < snap.width; ++x)
            result += snap.cells[y * snap.width + x].ch;
        result += "|\n";
    }

    result += "+";
    for (unsigned int x = 0; x < snap.width; ++x) result += "-";
    result += "+\n";

    result += "Cursor: (col=" + std::to_string(snap.cursor_x) +
              ", row=" + std::to_string(snap.cursor_y) + ")";
    if (snap.cursor_hidden) result += " [hidden]";
    result += "\n";

    return result;
}

std::string TerminalBase::DumpScreen(int snapshot_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot_id >= 0) {
        if (static_cast<size_t>(snapshot_id) >= snapshots_.size()) {
            throw std::out_of_range("invalid snapshot id");
        }
        return RenderSnapshot(snapshots_[snapshot_id]);
    }
    return RenderSnapshot(CaptureCurrent());
}

int TerminalBase::Snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshots_.push_back(CaptureCurrent());
    return static_cast<int>(snapshots_.size()) - 1;
}

ScreenSnapshot TerminalBase::GetSnapshot(int snapshot_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot_id >= 0) {
        if (static_cast<size_t>(snapshot_id) >= snapshots_.size()) {
            throw std::out_of_range("invalid snapshot id");
        }
        return snapshots_[snapshot_id];
    }
    return CaptureCurrent();
}

void TerminalBase::WriteCb(struct tsm_vte *vte, const char *u8, size_t len,
                           void *data) {
    TerminalBase *term = static_cast<TerminalBase *>(data);
    int fd = term->master_fd_.load();
    if (fd >= 0) {
        WriteAll(fd, u8, len);
    }
}

void TerminalBase::SendKey(uint32_t keysym, unsigned int mods) {
    std::lock_guard<std::mutex> lock(mutex_);
    tsm_vte_handle_keyboard(vte_, keysym, 0, mods, 0);
}

unsigned int TerminalBase::Width() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tsm_screen_get_width(screen_);
}

unsigned int TerminalBase::Height() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tsm_screen_get_height(screen_);
}

std::string Terminal::ParseEscapes(std::string_view input) {
    std::string res;
    for (size_t i = 0; i < input.length(); ++i) {
        if (input[i] == '\\' && i + 1 < input.length()) {
            char next = input[i + 1];
            if (next == 'n') {
                res += '\n';
                i++;
            } else if (next == 'r') {
                res += '\r';
                i++;
            } else if (next == 't') {
                res += '\t';
                i++;
            } else if (next == '\\') {
                res += '\\';
                i++;
            } else if (next == 'x' && i + 3 <= input.length()) {
                std::string hex_str(input.substr(i + 2, 2));
                try {
                    int val = std::stoi(hex_str, nullptr, 16);
                    res += static_cast<char>(val);
                } catch (...) {
                    res += "\\x" + hex_str;
                }
                i += 3;
            } else if (next >= '0' && next <= '7' && i + 3 <= input.length()) {
                std::string oct_str(input.substr(i + 1, 3));
                try {
                    int val = std::stoi(oct_str, nullptr, 8);
                    res += static_cast<char>(val);
                } catch (...) {
                    res += "\\" + oct_str;
                }
                i += 3;
            } else {
                res += next;
                i++;
            }
        } else {
            res += input[i];
        }
    }
    return res;
}

static const std::unordered_map<std::string, uint32_t> &GetKeysymMap() {
    static const std::unordered_map<std::string, uint32_t> kKeys = {
        {"up", 0xff52},           {"down", 0xff54},
        {"left", 0xff51},         {"right", 0xff53},
        {"f1", 0xffbe},           {"f2", 0xffbf},
        {"f3", 0xffc0},           {"f4", 0xffc1},
        {"f5", 0xffc2},           {"f6", 0xffc3},
        {"f7", 0xffc4},           {"f8", 0xffc5},
        {"f9", 0xffc6},           {"f10", 0xffc7},
        {"f11", 0xffc8},          {"f12", 0xffc9},
        {"f13", 0xffca},          {"f14", 0xffcb},
        {"f15", 0xffcc},          {"f16", 0xffcd},
        {"f17", 0xffce},          {"f18", 0xffcf},
        {"f19", 0xffd0},          {"f20", 0xffd1},
        {"backspace", 0xff08},    {"tab", 0xff09},
        {"lefttab", 0xfe20},      {"iso_left_tab", 0xfe20},
        {"backtab", 0xfe20},      {"linefeed", 0xff0a},
        {"lf", 0xff0a},           {"clear", 0xff0b},
        {"sysreq", 0xff15},       {"sys_req", 0xff15},
        {"enter", 0xff0d},        {"return", 0xff0d},
        {"escape", 0xff1b},       {"esc", 0xff1b},
        {"find", 0xff68},         {"insert", 0xff63},
        {"delete", 0xffff},       {"del", 0xffff},
        {"select", 0xff60},       {"home", 0xff50},
        {"end", 0xff57},          {"pageup", 0xff55},
        {"pgup", 0xff55},         {"pagedown", 0xff56},
        {"pgdn", 0xff56},         {"kp_space", 0xff80},
        {"space", 0x0020},        {"kp_tab", 0xff89},
        {"kp_enter", 0xff8d},     {"kp_f1", 0xff91},
        {"kp_f2", 0xff92},        {"kp_f3", 0xff93},
        {"kp_f4", 0xff94},        {"kp_home", 0xff95},
        {"kp_left", 0xff96},      {"kp_up", 0xff97},
        {"kp_right", 0xff98},     {"kp_down", 0xff99},
        {"kp_pageup", 0xff9a},    {"kp_pgup", 0xff9a},
        {"kp_pagedown", 0xff9b},  {"kp_pgdn", 0xff9b},
        {"kp_end", 0xff9c},       {"kp_begin", 0xff9d},
        {"kp_insert", 0xff9e},    {"kp_delete", 0xff9f},
        {"kp_del", 0xff9f},       {"kp_multiply", 0xffaa},
        {"kp_mul", 0xffaa},       {"kp_add", 0xffab},
        {"kp_separator", 0xffac}, {"kp_sep", 0xffac},
        {"kp_subtract", 0xffad},  {"kp_sub", 0xffad},
        {"kp_decimal", 0xffae},   {"kp_dec", 0xffae},
        {"kp_divide", 0xffaf},    {"kp_div", 0xffaf},
        {"kp_0", 0xffb0},         {"kp_1", 0xffb1},
        {"kp_2", 0xffb2},         {"kp_3", 0xffb3},
        {"kp_4", 0xffb4},         {"kp_5", 0xffb5},
        {"kp_6", 0xffb6},         {"kp_7", 0xffb7},
        {"kp_8", 0xffb8},         {"kp_9", 0xffb9},
        {"kp_equal", 0xffbd}};
    return kKeys;
}

uint32_t Terminal::ParseKeysym(std::string_view name) {
    if (name.length() == 1) {
        return static_cast<uint32_t>(name[0]);
    }

    std::string lower_name(name);
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
                   ::tolower);

    const auto &kKeys = GetKeysymMap();
    if (auto it = kKeys.find(lower_name); it != kKeys.end()) {
        return it->second;
    }

    return 0;
}

std::vector<std::string> Terminal::GetKeysyms() {
    const auto &kKeys = GetKeysymMap();
    std::vector<std::string> keysyms;
    keysyms.reserve(kKeys.size());
    for (const auto &pair : kKeys) {
        keysyms.push_back(pair.first);
    }
    std::sort(keysyms.begin(), keysyms.end());
    return keysyms;
}

unsigned int Terminal::ParseMods(const std::vector<std::string> &mod_args,
                                 size_t start_idx) {
    unsigned int mods = 0;
    for (size_t i = start_idx; i < mod_args.size(); ++i) {
        std::string lower_mod = mod_args[i];
        std::transform(lower_mod.begin(), lower_mod.end(), lower_mod.begin(),
                       ::tolower);
        if (lower_mod == "shift") {
            mods |= TSM_SHIFT_MASK;
        } else if (lower_mod == "ctrl" || lower_mod == "control") {
            mods |= TSM_CONTROL_MASK;
        } else if (lower_mod == "alt" || lower_mod == "meta") {
            mods |= TSM_ALT_MASK;
        }
    }
    return mods;
}

class SubprocessTerminalImpl : public TerminalBase {
  public:
    SubprocessTerminalImpl(unsigned int width, unsigned int height,
                           const std::string &cmd,
                           const std::vector<std::string> &args,
                           const std::string &term_type,
                           const std::string &locale);
    ~SubprocessTerminalImpl() override;

    bool IsExited() const override;
    int ExitStatus() const override;
    void SendSignal(int sig) override;
    void Resize(unsigned int width, unsigned int height) override;

  private:
    pid_t child_pid_ = -1;
    std::atomic<bool> child_exited_{false};
    std::atomic<int> child_exit_status_{0};
    std::thread reader_thread_;
};

SubprocessTerminalImpl::SubprocessTerminalImpl(
    unsigned int width, unsigned int height, const std::string &cmd,
    const std::vector<std::string> &args, const std::string &term_type,
    const std::string &locale)
        : TerminalBase(width, height) {
    auto [master_fd, slave_fd] = OpenPty();

    pid_t pid = fork();
    if (pid < 0) {
        close(slave_fd);
        close(master_fd);
        throw std::runtime_error("fork failed");
    }

    if (pid == 0) {
        close(master_fd);
        setsid();
#ifdef TIOCSCTTY
        ioctl(slave_fd, TIOCSCTTY, 0);
#endif
        dup2(slave_fd, STDIN_FILENO);
        dup2(slave_fd, STDOUT_FILENO);
        dup2(slave_fd, STDERR_FILENO);
        if (slave_fd > STDERR_FILENO) {
            close(slave_fd);
        }

        setenv("TERM", term_type.c_str(), 1);
        unsetenv("TERMCAP");
        if (!locale.empty()) {
            setenv("LC_ALL", locale.c_str(), 1);
            setenv("LANG", locale.c_str(), 1);
        }

        std::vector<char *> argv;
        argv.push_back(const_cast<char *>(cmd.c_str()));
        for (const auto &arg : args) {
            argv.push_back(const_cast<char *>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execvp(cmd.c_str(), argv.data());
        perror("execvp");
        _exit(1);
    }

    close(slave_fd);
    child_pid_ = pid;
    master_fd_ = master_fd;
    SetWinsize(master_fd, width, height);
    last_update_time_ = std::chrono::steady_clock::now();

    reader_thread_ = std::thread([this]() {
        std::array<char, 1024> buf;
        while (true) {
            int fd = master_fd_.load();
            if (fd < 0) break;
            ssize_t n = read(fd, buf.data(), buf.size());
            if (n <= 0) {
                break;
            }
            FeedInput(buf.data(), n);
            {
                std::lock_guard<std::mutex> lock(pty_update_mutex_);
                last_update_time_ = std::chrono::steady_clock::now();
            }
            pty_update_cv_.notify_all();
        }
        int status;
        if (waitpid(child_pid_, &status, 0) == child_pid_) {
            int exit_code = 0;
            if (WIFEXITED(status)) {
                exit_code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                exit_code = -WTERMSIG(status);
            }
            child_exit_status_ = exit_code;
            child_exited_ = true;
        }
        {
            std::lock_guard<std::mutex> lock(pty_update_mutex_);
            last_update_time_ = std::chrono::steady_clock::now();
        }
        pty_update_cv_.notify_all();
    });
}

SubprocessTerminalImpl::~SubprocessTerminalImpl() {
    int fd = master_fd_.exchange(-1);
    if (!child_exited_ && child_pid_ > 0) {
        kill(child_pid_, SIGKILL);
    }
    if (fd >= 0) {
        close(fd);
    }
    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }
}

bool SubprocessTerminalImpl::IsExited() const { return child_exited_; }

int SubprocessTerminalImpl::ExitStatus() const { return child_exit_status_; }

void SubprocessTerminalImpl::SendSignal(int sig) {
    if (!child_exited_) {
        kill(child_pid_, sig);
    }
}

void SubprocessTerminalImpl::Resize(unsigned int width, unsigned int height) {
    TerminalBase::Resize(width, height);
    int fd = master_fd_.load();
    if (fd >= 0) {
        SetWinsize(fd, width, height);
    }
}

class ThreadTerminalImpl : public TerminalBase {
  public:
    ThreadTerminalImpl(unsigned int width, unsigned int height,
                       std::function<void()> client_func);
    ~ThreadTerminalImpl() override;

    bool IsExited() const override;
    int ExitStatus() const override;
    void SendSignal(int sig) override;
    void Resize(unsigned int width, unsigned int height) override;

  private:
    std::thread client_thread_;
    std::thread reader_thread_;
    std::atomic<bool> client_exited_{false};
    std::atomic<bool> stop_requested_{false};
    int slave_fd_ = -1;
};

ThreadTerminalImpl::ThreadTerminalImpl(unsigned int width, unsigned int height,
                                       std::function<void()> client_func)
        : TerminalBase(width, height) {
    auto [master_fd, slave_fd] = OpenPty();
    slave_fd_ = slave_fd;
    SetWinsize(master_fd, width, height);

    master_fd_ = master_fd;
    last_update_time_ = std::chrono::steady_clock::now();

    sigset_t set, old_set;
    sigemptyset(&set);
    sigaddset(&set, SIGWINCH);
    pthread_sigmask(SIG_BLOCK, &set, &old_set);

    reader_thread_ = std::thread([this]() {
        std::array<char, 1024> buf;
        while (!stop_requested_) {
            int fd = master_fd_.load();
            if (fd < 0) break;
            ssize_t n = read(fd, buf.data(), buf.size());
            if (n <= 0) {
                break;
            }
            FeedInput(buf.data(), n);
            {
                std::lock_guard<std::mutex> lock(pty_update_mutex_);
                last_update_time_ = std::chrono::steady_clock::now();
            }
            pty_update_cv_.notify_all();
        }
    });

    client_thread_ = std::thread([this, client_func]() {
        sigset_t cset;
        sigemptyset(&cset);
        sigaddset(&cset, SIGWINCH);
        pthread_sigmask(SIG_UNBLOCK, &cset, nullptr);

        int saved_stdin = dup(STDIN_FILENO);
        int saved_stdout = dup(STDOUT_FILENO);
        int saved_stderr = dup(STDERR_FILENO);

        dup2(slave_fd_, STDIN_FILENO);
        dup2(slave_fd_, STDOUT_FILENO);
        dup2(slave_fd_, STDERR_FILENO);
        close(slave_fd_);
        slave_fd_ = -1;

        // RAII guard to restore original FDs whether client_func returns
        // normally or throws.
        struct FdRestore {
            int in, out, err;
            ~FdRestore() {
                dup2(in, STDIN_FILENO);
                close(in);
                dup2(out, STDOUT_FILENO);
                close(out);
                dup2(err, STDERR_FILENO);
                close(err);
            }
        } guard{saved_stdin, saved_stdout, saved_stderr};

        try {
            client_func();
        } catch (...) {
        }

        // guard restores FDs here.
        client_exited_ = true;
        pty_update_cv_.notify_all();
    });

    pthread_sigmask(SIG_SETMASK, &old_set, nullptr);
}

ThreadTerminalImpl::~ThreadTerminalImpl() {
    stop_requested_ = true;
    int fd = master_fd_.exchange(-1);
    if (fd >= 0) {
        close(fd);
    }
    if (slave_fd_ >= 0) {
        close(slave_fd_);
        slave_fd_ = -1;
    }
    if (client_thread_.joinable()) {
        client_thread_.join();
    }
    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }
}

bool ThreadTerminalImpl::IsExited() const { return client_exited_; }

int ThreadTerminalImpl::ExitStatus() const { return 0; }

void ThreadTerminalImpl::SendSignal(int sig) {
    if (client_thread_.joinable()) {
        pthread_kill(client_thread_.native_handle(), sig);
    }
}

void ThreadTerminalImpl::Resize(unsigned int width, unsigned int height) {
    TerminalBase::Resize(width, height);
    int fd = master_fd_.load();
    if (fd >= 0) {
        SetWinsize(fd, width, height);
    }
    if (client_thread_.joinable()) {
        pthread_kill(client_thread_.native_handle(), SIGWINCH);
    }
}

std::unique_ptr<Terminal> CreateSubprocessTerminal(
    unsigned int width, unsigned int height, const std::string &cmd,
    const std::vector<std::string> &args, const std::string &term_type,
    const std::string &locale) {
    return std::make_unique<SubprocessTerminalImpl>(width, height, cmd, args,
                                                    term_type, locale);
}

std::unique_ptr<Terminal> CreateThreadTerminal(
    unsigned int width, unsigned int height,
    std::function<void()> client_func) {
    return std::make_unique<ThreadTerminalImpl>(width, height, client_func);
}

}  // namespace unstable
}  // namespace termobulator
