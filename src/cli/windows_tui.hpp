#pragma once

#if (CLI_WINDOWS)

    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX

    #include "globals.hpp"
    #include "sha256.hpp"

    #include <windows.h>
    #include <iostream>
    #include <sstream>
    #include <winternl.h>
    #include <intrin.h>
    #include <mutex>
    #include <deque>
    #include <conio.h>
    #include <iomanip>
    #include <vector>
    #include <string>

    #pragma comment(lib, "ntdll.lib")

    #if (!CLI_ARM)
        class win_ansi_enabler_t
        {
        public:
            win_ansi_enabler_t() noexcept : m_set(FALSE), m_old(0), m_out(GetStdHandle(STD_OUTPUT_HANDLE))
            {
                if (m_out != nullptr && m_out != INVALID_HANDLE_VALUE) {
                    if (GetConsoleMode(m_out, &m_old) != FALSE) {
                        m_set = SetConsoleMode(m_out, m_old | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
                    }
                }
            }
            ~win_ansi_enabler_t() {
                if (m_set != FALSE) {
                    SetConsoleMode(m_out, m_old);
                }
            }

            win_ansi_enabler_t(const win_ansi_enabler_t&) = delete;
            win_ansi_enabler_t& operator=(const win_ansi_enabler_t&) = delete;
            win_ansi_enabler_t(win_ansi_enabler_t&&) = delete;
            win_ansi_enabler_t& operator=(win_ansi_enabler_t&&) = delete;
        private:
            bool m_set;
            DWORD m_old;
            HANDLE m_out;
        };

        // safely trims and pads a string ensuring it fits perfectly within bounds
        // without leaking unclosed ANSI tags or overflowing text visually
        inline std::string pad(const std::string& str, const size_t target_len) {
            size_t vlen = 0;
            bool in_ansi = false;
            std::string result;

            if (target_len < 4096) {
                result.reserve(str.size() + target_len);
            }

            for (const char c : str) {
                if (c == '\x1B') {
                    in_ansi = true;
                }

                if (!in_ansi) {
                    if (vlen < target_len) {
                        result += c;
                        vlen++;
                    }
                }
                else {
                    result += c;
                    if (c == 'm') {
                        in_ansi = false;
                    }
                }
            }

            if (vlen < target_len) {
                const size_t pad_amount = target_len - vlen;
                result.append(pad_amount, ' ');
            }

            if (vlen >= target_len) {
                result += "\x1B[0m"; 
            }

            return result;
        }

        inline size_t visible_length(const std::string& str) noexcept {
            size_t len = 0;
            bool in_ansi = false;
            for (const char c : str) {
                if (c == '\x1B') {
                    in_ansi = true;
                } else if (in_ansi && c == 'm') {
                    in_ansi = false;
                } else if (!in_ansi) {
                    len++;
                }
            }
            return len;
        }

        inline std::string repeat_str(const std::string& str, size_t count) {
            std::string res;
            res.reserve(str.length() * count);
            for (size_t i = 0; i < count; ++i) {
                res += str;
            }
            return res;
        }

        // dynamic scaling boxes with dark/white sync theme
        class tui_manager {
        public:
            SHORT start_y = 0;
            SHORT left_y = 0;
            SHORT right_x = 0;
            SHORT left_margin = 0;
            SHORT console_width = 120;

            SHORT exception_y = 0;
            SHORT box_height = 10;
            size_t global_box_width = 70; // dynamic base width

            HANDLE hOut = nullptr;
            std::mutex mtx;
            bool enabled = false;

            std::streambuf* orig_buf = nullptr;
            std::ostream* raw_out = nullptr;

            std::vector<std::vector<std::string>> exceptions;
            size_t exc_scroll_index = 0;

            std::vector<std::string> cycles;
            size_t cyc_scroll_index = 0;

            std::vector<std::string> debugs;
            size_t dbg_scroll_index = 0;

            u32 g_max_std = 0;
            u32 g_max_hyp = 0;
            u32 g_max_ext = 0;

            bool set_cursor(const SHORT x, const SHORT y) const noexcept;
            bool update_box_width(size_t incoming_len);
            void init();
            ~tui_manager();
            void print_header();
            void print_left(const std::string& str);
            void clear_boxes();
            void redraw_all_boxes();
            SHORT draw_box_internal(SHORT startY, size_t box_width, const std::string& title, const std::vector<std::string>& items, size_t scroll_idx, const std::string& controls);
            void add_exception(const std::vector<std::string>& lines);
            void add_cycle(const std::string& line);
            void add_debug(const std::string& line);
            void scroll_exceptions_up();
            void scroll_exceptions_down();
            void scroll_cycles_up();
            void scroll_cycles_down();
            void scroll_debug_up();
            void scroll_debug_down();
            void draw_summary_box(const std::vector<std::string>& lines);
            void finalize();
        };

        extern tui_manager g_tui;

        // ALL output sent through std::cout that doesn't explicitly bypass
        // into g_tui.raw_out gets captured and sent to the Debug Log UI. No layout escapes possible
        class debug_interceptor : public std::streambuf {
            std::string buffer;
        public:
            std::streambuf* original;

            debug_interceptor(std::streambuf* orig) : original(orig) {}
            ~debug_interceptor();
        protected:
            virtual int_type overflow(int_type c) override;
            virtual std::streamsize xsputn(const char* s, std::streamsize n) override;
        };

        #define PRINT_LINE(msg) \
            do { \
                std::ostringstream _oss; \
                _oss << msg; \
                g_tui.print_left(_oss.str()); \
            } while(0)

        template<typename... Args>
        void VMAWARE_CLI_DEBUG(Args&&... args) {
            std::ostringstream oss;
            int dummy[] = { 0, ((void)(oss << std::forward<Args>(args)), 0)... };
            (void)dummy;

            if (g_tui.enabled) {
                g_tui.add_debug(oss.str());
            }
            else {
                std::cout << "[DEBUG] " << oss.str() << "\n";
            }
        }

        LONG WINAPI exception_handler_logger(PEXCEPTION_POINTERS ep);

    #else

        #define PRINT_LINE(msg) std::cout << (msg) << "\n"

        template<typename... Args>
        void VMAWARE_CLI_DEBUG(Args&&... args) {
            std::ostringstream oss;
            int dummy[] = { 0, ((void)(oss << std::forward<Args>(args)), 0)... };
            (void)dummy;
            std::cout << "[DEBUG] " << oss.str() << "\n";
        }
    #endif

#else

    #define PRINT_LINE(msg) std::cout << (msg) << "\n"

    template<typename... Args>
    void VMAWARE_CLI_DEBUG(Args&&... args) {
        std::ostringstream oss;
        int dummy[] = { 0, ((void)(oss << std::forward<Args>(args)), 0)... };
        (void)dummy;
        std::cout << "[DEBUG] " << oss.str() << "\n";
    }

#endif