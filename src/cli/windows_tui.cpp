#include "types.hpp"

#if (CLI_WINDOWS && !CLI_ARM)
#include "globals.hpp"
#include "sha256.hpp"
#include "windows_tui.hpp"

#include <cstdlib>
#include <algorithm>
#include <cstdint>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

tui_manager g_tui;

// Tracks the deepest Y coordinate the right-hand boxes reach to prevent overlapping text at the end
static SHORT g_right_bottom_y = 0;

bool tui_manager::set_cursor(const SHORT x, const SHORT y) const noexcept {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hOut, &csbi);

    // if we reach the bottom of the buffer, dynamically expand it to prevent auto-scrolling
    // auto-scrolling permanently breaks absolute Y coordinates, so we must expand instead
    if (y >= csbi.dwSize.Y) {
        const SHORT target_y = static_cast<SHORT>(y + 100);
        const COORD newSize = { csbi.dwSize.X, target_y };
        SetConsoleScreenBufferSize(hOut, newSize);
        GetConsoleScreenBufferInfo(hOut, &csbi);
    }

    SetConsoleCursorPosition(hOut, { x, y });

    // scroll the user's viewport downwards if we draw below the visible area
    if (y > csbi.srWindow.Bottom) {
        SMALL_RECT sr = csbi.srWindow;
        const SHORT diff = static_cast<SHORT>(y - sr.Bottom);
        sr.Top = static_cast<SHORT>(sr.Top + diff);
        sr.Bottom = static_cast<SHORT>(sr.Bottom + diff);
        SetConsoleWindowInfo(hOut, TRUE, &sr);
    }

    return true;
}

void tui_manager::clear_boxes() {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hOut, &csbi);

    if (csbi.dwSize.X <= right_x) return;

    const size_t wipe_len = static_cast<size_t>(csbi.dwSize.X) - static_cast<size_t>(right_x);
    const std::string wipe_str(wipe_len, ' ');
    // clear vertically over the maximum theoretical height of the 3 boxes
    for (SHORT i = 0; i < 70; i++) {
        if (exception_y + i >= csbi.dwSize.Y) break; // Don't expand the buffer just to clear empty space
        SetConsoleCursorPosition(hOut, { right_x, static_cast<SHORT>(exception_y + i) });
        *raw_out << ansi_exit << wipe_str << "\x1B[K";
    }
}

bool tui_manager::update_box_width(size_t incoming_len) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hOut, &csbi);
    console_width = csbi.dwSize.X;

    // leaves room for the big right-side bracket (24 chars)
    const int available_space = static_cast<int>(console_width) - static_cast<int>(right_x) - 24;
    const size_t max_allowed = static_cast<size_t>(std::max<int>(10, available_space));

    if (incoming_len > max_allowed) {
        incoming_len = max_allowed;
    }

    if (incoming_len > global_box_width) {
        global_box_width = incoming_len;
        clear_boxes(); // wipe old ghost boundaries before redrawing
        return true;
    }
    return false;
}

void tui_manager::init() {
    hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleOutputCP(CP_UTF8);
    enabled = true;

    orig_buf = std::cout.rdbuf();
    raw_out = new std::ostream(orig_buf);

    // maximize window ASYNCHRONOUSLY to avoid Windows Terminal DWM freezes
    const HWND hwnd = GetForegroundWindow();
    char className[256] = { 0 };
    if (GetClassNameA(hwnd, &className[0], static_cast<int>(sizeof(className)))) {
        if (strcmp(&className[0], "CASCADIA_HOSTING_WINDOW_CLASS") == 0 ||
            strcmp(&className[0], "ConsoleWindowClass") == 0) {
            PostMessage(hwnd, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
            Sleep(150);
        }
    }
    else {
        const HWND hCon = GetConsoleWindow();
        if (hCon) PostMessage(hCon, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
    }

    // ANSI fallback maximize for Windows Terminal
    *raw_out << "\x1B[9;1t" << std::flush;
    SleepEx(50, FALSE);

    // pre-allocate buffer space downwards for subsequent runs
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hOut, &csbi);

    const SHORT safe_height = static_cast<SHORT>(csbi.dwCursorPosition.Y + 200);
    if (safe_height > csbi.dwSize.Y) {
        SetConsoleScreenBufferSize(hOut, { csbi.dwSize.X, safe_height });
    }

    GetConsoleScreenBufferInfo(hOut, &csbi);
    console_width = csbi.dwSize.X;

    // calculate right-side UI anchors
    right_x = 88;
    if (console_width < 120) {
        right_x = std::max<SHORT>(10, static_cast<SHORT>(console_width - 35));
    }
    global_box_width = 30;

    start_y = csbi.dwCursorPosition.Y;
    left_y = start_y;
    exception_y = start_y;

#ifndef VMAWARE_DEBUG
    debugs.push_back(dim + std::string("Compile in debug mode to view detailed logs.") + ansi_exit);
#endif

    set_cursor(0, start_y);
    print_header();
}

tui_manager::~tui_manager() {
    if (raw_out) {
        delete raw_out;
        raw_out = nullptr;
    }
}

void tui_manager::print_header() {
    std::string arch, vendor, ucode, os;
    int family = 0, model = 0, stepping = 0;

    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);

    if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64) {
        arch = "x64";
    }
    else if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL) {
        arch = "x86";
    }
    else if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64) {
        arch = "ARM64";
    }
    else {
        arch = "Unknown";
    }

    int cpuInfo[4] = { 0 };
    __cpuid(&cpuInfo[0], 0);
    char vendorStr[13] = { 0 };
    memcpy(&vendorStr[0], &cpuInfo[1], 4);
    memcpy(&vendorStr[4], &cpuInfo[3], 4);
    memcpy(&vendorStr[8], &cpuInfo[2], 4);
    vendor = &vendorStr[0];

    g_max_std = cpuInfo[0];
    __cpuid(&cpuInfo[0], 0x40000000);
    g_max_hyp = cpuInfo[0];
    __cpuid(&cpuInfo[0], 0x80000000);
    g_max_ext = cpuInfo[0];

    __cpuid(&cpuInfo[0], 1);
    stepping = cpuInfo[0] & 0xF;
    model = (cpuInfo[0] >> 4) & 0xF;
    family = (cpuInfo[0] >> 8) & 0xF;
    if (family == 6 || family == 15) {
        model += ((cpuInfo[0] >> 16) & 0xF) << 4;
    }

    if (family == 15) {
        family += (cpuInfo[0] >> 20) & 0xFF;
    }

    HKEY hKey = NULL;
    ucode = "N/A";
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD type = 0;
        BYTE data[8] = { 0 };
        DWORD size = static_cast<DWORD>(sizeof(data));
        if (RegQueryValueExA(hKey, "Update Revision", NULL, &type, &data[0], &size) == ERROR_SUCCESS && type == REG_BINARY) {
            std::ostringstream ucode_oss;
            u64 full_val = 0;
            const size_t copy_bytes = (size < sizeof(full_val)) ? static_cast<size_t>(size) : sizeof(full_val);
            memcpy(&full_val, &data[0], copy_bytes);
            DWORD ucode_val = static_cast<DWORD>((full_val >> 32) & 0xFFFFFFFFULL);
            if (ucode_val == 0) {
                ucode_val = static_cast<DWORD>(full_val & 0xFFFFFFFFULL);
            }
            ucode_oss << "0x" << std::hex << ucode_val;
            ucode = ucode_oss.str();
        }
        RegCloseKey(hKey);
    }

    typedef NTSTATUS(WINAPI* RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
    HMODULE hMod = GetModuleHandleA("ntdll.dll");
    if (hMod) {
        RtlGetVersionPtr pRtlGetVersion = reinterpret_cast<RtlGetVersionPtr>(GetProcAddress(hMod, "RtlGetVersion"));
        if (pRtlGetVersion) {
            RTL_OSVERSIONINFOW rovi{};
            rovi.dwOSVersionInfoSize = static_cast<DWORD>(sizeof(rovi));
            if (pRtlGetVersion(&rovi) == 0) {
                std::ostringstream os_oss;
                if (rovi.dwMajorVersion == 10 && rovi.dwBuildNumber >= 22000) {
                    os_oss << "Windows 11 (" << rovi.dwBuildNumber << ")";
                }
                else if (rovi.dwMajorVersion == 10) {
                    os_oss << "Windows 10 (" << rovi.dwBuildNumber << ")";
                }
                else {
                    os_oss << "Windows " << rovi.dwMajorVersion << "." << rovi.dwMinorVersion << " (" << rovi.dwBuildNumber << ")";
                }
                os = os_oss.str();
            }
        }
    }

    const std::string hash_display = compute_self_sha256();

    const std::string raw_header_text = "arch: " + arch + " / vendor: " + vendor + " / family: " + std::to_string(family) +
        " / model: " + std::to_string(model) + " / stepping: " + std::to_string(stepping) +
        " / microcode: " + ucode + " / os: " + os + " / sha256: " + hash_display;

    int padding_val = (static_cast<int>(console_width) - static_cast<int>(raw_header_text.length())) / 2;
    if (padding_val < 0) {
        padding_val = 0;
    }

    const std::string pad_str(static_cast<size_t>(padding_val), ' ');

    *raw_out << pad_str
        << dim << "arch: " << bright << arch << dim
        << " / vendor: " << bright << vendor << dim
        << " / family: " << bright << family << dim
        << " / model: " << bright << model << dim
        << " / stepping: " << bright << stepping << dim
        << " / microcode: " << bright << ucode << dim
        << " / os: " << bright << os << dim
        << " / sha256: " << bright << hash_display << ansi_exit << "\n\n";

    const size_t separator_len = (console_width > 1) ? (static_cast<size_t>(console_width) - 1ULL) : 0ULL;
    *raw_out << dim << repeat_str("─", separator_len) << ansi_exit << "\n";

    // resync Y coordinates natively after the \n's
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hOut, &csbi);

    left_y = csbi.dwCursorPosition.Y;
    exception_y = left_y;

    redraw_all_boxes();
}

void tui_manager::print_left(const std::string& str) {
    if (!enabled) {
        std::cout << str << "\n";
        return;
    }
    std::lock_guard<std::mutex> lock(mtx);
    set_cursor(left_margin, left_y);
    *raw_out << str << "\x1B[K" << std::flush;
    left_y++;
}

void tui_manager::redraw_all_boxes() {
    if (!enabled) return;

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hOut, &csbi);
    console_width = csbi.dwSize.X;

    SHORT draw_y = exception_y;
    const size_t content_w = (global_box_width >= 4) ? (global_box_width - 4) : 0;

    // 1. Exceptions Box
    set_cursor(right_x, draw_y++);
    *raw_out << dim << "┌─ " << white << "Exceptions" << dim << " " << repeat_str("─", global_box_width >= 15 ? global_box_width - 15 : 0) << "┐" << ansi_exit << "\x1B[K";

    if (!exceptions.empty()) {
        const auto& lines = exceptions.at(exc_scroll_index);
        for (const auto& line_entry : lines) {
            set_cursor(right_x, draw_y++);
            *raw_out << dim << "│ " << ansi_exit << pad(line_entry, content_w) << dim << " │" << ansi_exit << "\x1B[K";
        }
    }
    else {
        const size_t empty_height = (box_height > 2) ? (static_cast<size_t>(box_height) - 2ULL) : 0ULL;
        for (size_t i = 0; i < empty_height; i++) {
            set_cursor(right_x, draw_y++);
            *raw_out << dim << "│ " << ansi_exit << pad("", content_w) << dim << " │" << ansi_exit << "\x1B[K";
        }
    }

    set_cursor(right_x, draw_y++);
    *raw_out << dim << "└" << repeat_str("─", global_box_width >= 2 ? global_box_width - 2 : 0) << "┘" << ansi_exit << "\x1B[K";

    set_cursor(right_x, draw_y++);
    std::string exc_controls = "Use Left/Right arrows to scroll (" +
        std::to_string(exceptions.empty() ? 0 : exc_scroll_index + 1) + "/" +
        std::to_string(exceptions.size()) + ")";
    *raw_out << dim << " " << exc_controls << " " << ansi_exit << "\x1B[K";

    set_cursor(right_x, draw_y++);
    *raw_out << ansi_exit << "\x1B[K"; // gap

    // 2. Timings Box
    draw_y = draw_box_internal(draw_y, global_box_width, "Timings", cycles, cyc_scroll_index, "Use Up/Down arrows to scroll");

    set_cursor(right_x, draw_y++);
    *raw_out << ansi_exit << "\x1B[K"; // gap

    // 3. Debug Box
    draw_y = draw_box_internal(draw_y, global_box_width, "Debug", debugs, dbg_scroll_index, "Use PgUp/PgDn to scroll");

    const SHORT bottom_y = static_cast<SHORT>(draw_y - 1); // Ends exactly at the Debug control text line
    const SHORT bracket_x = static_cast<SHORT>(right_x + static_cast<SHORT>(global_box_width) + 4);

    if (bracket_x + 15 < console_width) {
        set_cursor(bracket_x, exception_y);
        *raw_out << white << "┐" << ansi_exit;

        for (SHORT y = static_cast<SHORT>(exception_y + 1); y < bottom_y; y++) {
            set_cursor(bracket_x, y);
            *raw_out << white << "│" << ansi_exit;
        }

        set_cursor(bracket_x, bottom_y);
        *raw_out << white << "┘" << ansi_exit;

        const SHORT mid_y = static_cast<SHORT>(exception_y + (bottom_y - exception_y) / 2);
        const SHORT text_x = static_cast<SHORT>(bracket_x + 3);

        set_cursor(text_x, static_cast<SHORT>(mid_y - 1));
        *raw_out << dim << "s: " << white << "0x" << std::hex << std::setfill('0') << std::setw(8) << g_max_std << ansi_exit << "\x1B[K";
        set_cursor(text_x, mid_y);
        *raw_out << dim << "h: " << white << "0x" << std::hex << std::setfill('0') << std::setw(8) << g_max_hyp << ansi_exit << "\x1B[K";
        set_cursor(text_x, static_cast<SHORT>(mid_y + 1));
        *raw_out << dim << "e: " << white << "0x" << std::hex << std::setfill('0') << std::setw(8) << g_max_ext << ansi_exit << "\x1B[K";
    }

    g_right_bottom_y = draw_y;

    // Re-align to primary drawing coords safely
    set_cursor(left_margin, left_y);
    *raw_out << std::flush;
}

SHORT tui_manager::draw_box_internal(SHORT startY, size_t box_width, const std::string& title, const std::vector<std::string>& items, size_t scroll_idx, const std::string& controls_base) {
    SHORT draw_y = startY;
    const size_t content_w = (box_width >= 4) ? (box_width - 4) : 0;
    const size_t title_len = visible_length(title);
    const size_t dash_count = (box_width >= 5 + title_len) ? (box_width - 5 - title_len) : 0;

    set_cursor(right_x, draw_y++);
    *raw_out << dim << "┌─ " << white << title << " " << dim << repeat_str("─", dash_count) << "┐" << ansi_exit << "\x1B[K" << std::flush;

    const size_t limit = (box_height > 2) ? (static_cast<size_t>(box_height) - 2ULL) : 0ULL;
    for (size_t i = 0; i < limit; i++) {
        set_cursor(right_x, draw_y++);
        if (scroll_idx + i < items.size()) {
            *raw_out << dim << "│ " << ansi_exit << pad(items.at(scroll_idx + i), content_w) << dim << " │" << ansi_exit << "\x1B[K" << std::flush;
        }
        else {
            *raw_out << dim << "│ " << ansi_exit << pad("", content_w) << dim << " │" << ansi_exit << "\x1B[K" << std::flush;
        }
    }

    set_cursor(right_x, draw_y++);
    *raw_out << dim << "└" << repeat_str("─", box_width >= 2 ? box_width - 2 : 0) << "┘" << ansi_exit << "\x1B[K" << std::flush;

    set_cursor(right_x, draw_y++);
    const std::string controls = controls_base + " (" +
        std::to_string(items.empty() ? 0 : scroll_idx + 1) + "/" +
        std::to_string(items.size()) + ")";

    *raw_out << dim << " " << controls << " " << ansi_exit << "\x1B[K" << std::flush;
    return draw_y;
}

void tui_manager::add_exception(const std::vector<std::string>& lines) {
    if (!enabled) return;
    bool resized = false;
    {
        std::lock_guard<std::mutex> lock(mtx);
        exceptions.push_back(lines);
        exc_scroll_index = exceptions.size() - 1;

        for (const auto& l : lines) {
            if (this->update_box_width(visible_length(l) + 4)) {
                resized = true;
            }
        }
    }
    std::lock_guard<std::mutex> draw_lock(mtx);
    if (resized) clear_boxes();
    redraw_all_boxes();
}

void tui_manager::add_cycle(const std::string& line) {
    if (!enabled) return;
    bool resized = false;
    {
        std::lock_guard<std::mutex> lock(mtx);
        cycles.push_back(line);
        const size_t visible_rows = (box_height > 2) ? (static_cast<size_t>(box_height) - 2ULL) : 0ULL;
        if (cycles.size() > visible_rows) {
            cyc_scroll_index = cycles.size() - visible_rows;
        }

        if (this->update_box_width(visible_length(line) + 4)) {
            resized = true;
        }
    }
    std::lock_guard<std::mutex> draw_lock(mtx);
    if (resized) clear_boxes();
    redraw_all_boxes();
}

void tui_manager::add_debug(const std::string& line) {
    if (!enabled) return;
    bool resized = false;

    std::string colored_line;
    const size_t pos = line.find(": ");

    if (pos != std::string::npos) {
        colored_line = white + line.substr(0, pos) + dim + line.substr(pos) + ansi_exit;
    }
    else {
        colored_line = dim + line + ansi_exit;
    }

    {
        std::lock_guard<std::mutex> lock(mtx);
        debugs.push_back(colored_line);
        const size_t visible_rows = (box_height > 2) ? (static_cast<size_t>(box_height) - 2ULL) : 0ULL;
        if (debugs.size() > visible_rows) {
            dbg_scroll_index = debugs.size() - visible_rows;
        }

        if (this->update_box_width(visible_length(colored_line) + 4)) {
            resized = true;
        }
    }

    std::lock_guard<std::mutex> draw_lock(mtx);
    if (resized) clear_boxes();
    redraw_all_boxes();
}

void tui_manager::scroll_exceptions_up() {
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (exc_scroll_index > 0) {
            exc_scroll_index--;
        }
    }
    std::lock_guard<std::mutex> draw_lock(mtx);
    redraw_all_boxes();
}

void tui_manager::scroll_exceptions_down() {
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (exc_scroll_index + 1 < exceptions.size()) {
            exc_scroll_index++;
        }
    }
    std::lock_guard<std::mutex> draw_lock(mtx);
    redraw_all_boxes();
}

void tui_manager::scroll_cycles_up() {
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (cyc_scroll_index > 0) {
            cyc_scroll_index--;
        }
    }
    std::lock_guard<std::mutex> draw_lock(mtx);
    redraw_all_boxes();
}

void tui_manager::scroll_cycles_down() {
    {
        std::lock_guard<std::mutex> lock(mtx);
        const size_t visible_rows = (box_height > 2) ? (static_cast<size_t>(box_height) - 2ULL) : 0ULL;
        if (cyc_scroll_index + visible_rows < cycles.size()) {
            cyc_scroll_index++;
        }
    }
    std::lock_guard<std::mutex> draw_lock(mtx);
    redraw_all_boxes();
}

void tui_manager::scroll_debug_up() {
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (dbg_scroll_index > 0) {
            dbg_scroll_index--;
        }
    }
    std::lock_guard<std::mutex> draw_lock(mtx);
    redraw_all_boxes();
}

void tui_manager::scroll_debug_down() {
    {
        std::lock_guard<std::mutex> lock(mtx);
        const size_t visible_rows = (box_height > 2) ? (static_cast<size_t>(box_height) - 2ULL) : 0ULL;
        if (dbg_scroll_index + visible_rows < debugs.size()) {
            dbg_scroll_index++;
        }
    }
    std::lock_guard<std::mutex> draw_lock(mtx);
    redraw_all_boxes();
}

void tui_manager::draw_summary_box(const std::vector<std::string>& lines) {
    if (!enabled) return;
    SHORT draw_y = static_cast<SHORT>(left_y + 1);

    size_t max_len = 0;
    for (auto& l : lines) {
        max_len = std::max(max_len, visible_length(l));
    }

    size_t box_width = std::max(static_cast<size_t>(80), max_len + 4);

    if (console_width > 2 && box_width >= static_cast<size_t>(console_width - 2)) {
        const size_t max_allowed_width = static_cast<size_t>(console_width - 2);
        box_width = std::max(static_cast<size_t>(40), max_allowed_width);
    }

    set_cursor(left_margin, draw_y++);
    *raw_out << dim << "┌" << repeat_str("─", box_width) << "┐" << ansi_exit << "\n";

    const size_t content_width = (box_width >= 2) ? (box_width - 2) : 0;

    for (const auto& line : lines) {
        set_cursor(left_margin, draw_y++);
        *raw_out << dim << "│ " << ansi_exit << pad(line, content_width) << dim << " │" << ansi_exit << "\n";
    }

    set_cursor(left_margin, draw_y++);
    *raw_out << dim << "└" << repeat_str("─", box_width) << "┘" << ansi_exit << "\n";

    left_y = draw_y;
}

void tui_manager::finalize() {
    if (!enabled) return;

    // position terminal exit prompt below EVERYTHING drawn 
    const SHORT final_y = std::max<SHORT>(left_y, static_cast<SHORT>(g_right_bottom_y + 1));
    set_cursor(0, final_y);
    *raw_out << ansi_exit << "\n" << std::flush;
}

debug_interceptor::~debug_interceptor() {
    if (!buffer.empty()) {
        try {
            std::ostream os(original);
            os << buffer;
        }
        catch (...) {
        }
    }
}

debug_interceptor::int_type debug_interceptor::overflow(int_type c) {
    if (c == EOF) {
        return c;
    }

    const char ch = static_cast<char>(c);

    if (ch != '\n') {
        buffer += ch;
        return c;
    }

    while (!buffer.empty() && (buffer.back() == '\r' || buffer.back() == ' ')) {
        if (buffer.back() == '\r') {
            buffer.pop_back();
        }
        else if (buffer.back() == ' ' && buffer.find("[DEBUG]") != std::string::npos) {
            break;
        }
        else {
            break;
        }
    }

    if (buffer.find('\t') != std::string::npos) {
        size_t pos;
        while ((pos = buffer.find('\t')) != std::string::npos) {
            buffer.replace(pos, 1, "    ");
        }
    }

    std::string msg = buffer;
    const size_t debug_pos = msg.find("[DEBUG]");
    if (debug_pos != std::string::npos) {
        msg = msg.substr(debug_pos + 7);
        while (!msg.empty() && msg.front() == ' ') {
            msg = msg.substr(1);
        }
        while (!msg.empty() && (msg.back() == '\r' || msg.back() == ' ')) {
            msg.pop_back();
        }
        g_tui.add_debug(msg);
    }
    else {
        if (g_tui.raw_out) {
            g_tui.set_cursor(g_tui.left_margin, g_tui.left_y);
            *(g_tui.raw_out) << buffer << "\x1B[K" << std::flush;
            g_tui.left_y++;
        }
        else {
            try {
                std::ostream os(original);
                os << buffer << "\n";
            }
            catch (...) {
            }
        }
    }

    buffer.clear();
    return c;
}

std::streamsize debug_interceptor::xsputn(const char* s, std::streamsize n) {
    if (!s) {
        return 0;
    }
    for (std::streamsize i = 0; i < n; ++i) {
        overflow(static_cast<unsigned char>(s[i]));
    }
    return n;
}

LONG WINAPI exception_handler_logger(PEXCEPTION_POINTERS ep) {
    if (!ep || !g_tui.enabled) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const std::string c_white = white;
    const std::string c_grey = dim;
    const std::string c_rst = ansi_exit;

    auto to_hex = [](u64 val) -> std::string {
        std::ostringstream oss;
        oss << "0x" << std::hex << std::uppercase << val;
        return oss.str();
        };

    auto hex_pad = [&](u64 val, int width) -> std::string {
        return pad(to_hex(val), static_cast<size_t>(width));
        };

    std::vector<std::string> lines;
    lines.push_back(c_grey + "Exception Hit: " + c_white + to_hex(static_cast<u64>(ep->ExceptionRecord->ExceptionCode)) + c_rst);
    lines.push_back(c_grey + "Address: " + c_white + to_hex(reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress)) + c_rst);
    lines.push_back(c_grey + "Flags: " + c_white + hex_pad(static_cast<u64>(ep->ExceptionRecord->ExceptionFlags), 6) + c_grey + " Params: " + c_white + std::to_string(ep->ExceptionRecord->NumberParameters) + c_rst);
    lines.push_back("");

#if (defined(VMAWARE_X86_64) || defined(_M_X64))
    lines.push_back(c_grey + "RIP: " + c_white + hex_pad(static_cast<u64>(ep->ContextRecord->Rip), 18) + c_grey + " RSP: " + c_white + to_hex(static_cast<u64>(ep->ContextRecord->Rsp)) + c_rst);
    lines.push_back(c_grey + "RAX: " + c_white + hex_pad(static_cast<u64>(ep->ContextRecord->Rax), 18) + c_grey + " RCX: " + c_white + to_hex(static_cast<u64>(ep->ContextRecord->Rcx)) + c_rst);
    lines.push_back(c_grey + "RDX: " + c_white + hex_pad(static_cast<u64>(ep->ContextRecord->Rdx), 18) + c_grey + " RBX: " + c_white + to_hex(static_cast<u64>(ep->ContextRecord->Rbx)) + c_rst);
    lines.push_back(c_grey + "RBP: " + c_white + hex_pad(static_cast<u64>(ep->ContextRecord->Rbp), 18) + c_grey + " RSI: " + c_white + to_hex(static_cast<u64>(ep->ContextRecord->Rsi)) + c_rst);
    lines.push_back(c_grey + "RDI: " + c_white + hex_pad(static_cast<u64>(ep->ContextRecord->Rdi), 18) + c_grey + " EFL: " + c_white + to_hex(static_cast<u64>(ep->ContextRecord->EFlags)) + c_rst);
    lines.push_back(c_grey + "R8 : " + c_white + hex_pad(static_cast<u64>(ep->ContextRecord->R8), 18) + c_grey + " R9 : " + c_white + to_hex(static_cast<u64>(ep->ContextRecord->R9)) + c_rst);
    lines.push_back(c_grey + "R10: " + c_white + hex_pad(static_cast<u64>(ep->ContextRecord->R10), 18) + c_grey + " R11: " + c_white + to_hex(static_cast<u64>(ep->ContextRecord->R11)) + c_rst);
    lines.push_back(c_grey + "R12: " + c_white + hex_pad(static_cast<u64>(ep->ContextRecord->R12), 18) + c_grey + " R13: " + c_white + to_hex(static_cast<u64>(ep->ContextRecord->R13)) + c_rst);
    lines.push_back(c_grey + "R14: " + c_white + hex_pad(static_cast<u64>(ep->ContextRecord->R14), 18) + c_grey + " R15: " + c_white + to_hex(static_cast<u64>(ep->ContextRecord->R15)) + c_rst);
    lines.push_back("");
    lines.push_back(c_grey + "CS: " + c_white + hex_pad(static_cast<u64>(ep->ContextRecord->SegCs), 8) + c_grey + " DS: " + c_white + hex_pad(static_cast<u64>(ep->ContextRecord->SegDs), 8) + c_grey + " SS: " + c_white + to_hex(static_cast<u64>(ep->ContextRecord->SegSs)) + c_rst);
    lines.push_back(c_grey + "ES: " + c_white + hex_pad(static_cast<u64>(ep->ContextRecord->SegEs), 8) + c_grey + " FS: " + c_white + hex_pad(static_cast<u64>(ep->ContextRecord->SegFs), 8) + c_grey + " GS: " + c_white + to_hex(static_cast<u64>(ep->ContextRecord->SegGs)) + c_rst);
    lines.push_back("");
    lines.push_back(c_grey + "Dr0: " + c_white + hex_pad(static_cast<u64>(ep->ContextRecord->Dr0), 18) + c_grey + " Dr1: " + c_white + to_hex(static_cast<u64>(ep->ContextRecord->Dr1)) + c_rst);
    lines.push_back(c_grey + "Dr2: " + c_white + hex_pad(static_cast<u64>(ep->ContextRecord->Dr2), 18) + c_grey + " Dr3: " + c_white + to_hex(static_cast<u64>(ep->ContextRecord->Dr3)) + c_rst);
    lines.push_back(c_grey + "Dr6: " + c_white + hex_pad(static_cast<u64>(ep->ContextRecord->Dr6), 18) + c_grey + " Dr7: " + c_white + to_hex(static_cast<u64>(ep->ContextRecord->Dr7)) + c_rst);
    lines.push_back("");
    lines.push_back(c_grey + "ContextFlags: " + c_white + hex_pad(static_cast<u64>(ep->ContextRecord->ContextFlags), 10) + c_grey + " MxCsr: " + c_white + to_hex(static_cast<u64>(ep->ContextRecord->MxCsr)) + c_rst);
    lines.push_back(c_grey + "DebugControl: " + c_white + to_hex(static_cast<u64>(ep->ContextRecord->DebugControl)) + c_rst);
#else
    lines.push_back(c_grey + "EIP: " + c_white + hex_pad(static_cast<u64>(ep->ContextRecord->Eip), 10) + c_grey + " ESP: " + c_white + to_hex(static_cast<u64>(ep->ContextRecord->Esp)) + c_rst);
    lines.push_back(c_grey + "EAX: " + c_white + hex_pad(static_cast<u64>(ep->ContextRecord->Eax), 10) + c_grey + " ECX: " + c_white + to_hex(static_cast<u64>(ep->ContextRecord->Ecx)) + c_rst);
    lines.push_back(c_grey + "EDX: " + c_white + hex_pad(static_cast<u64>(ep->ContextRecord->Edx), 10) + c_grey + " EBX: " + c_white + to_hex(static_cast<u64>(ep->ContextRecord->Ebx)) + c_rst);
    lines.push_back(c_grey + "EBP: " + c_white + hex_pad(static_cast<u64>(ep->ContextRecord->Ebp), 10) + c_grey + " ESI: " + c_white + to_hex(static_cast<u64>(ep->ContextRecord->Esi)) + c_rst);
    lines.push_back(c_grey + "EDI: " + c_white + hex_pad(static_cast<u64>(ep->ContextRecord->Edi), 10) + c_grey + " EFL: " + c_white + to_hex(static_cast<u64>(ep->ContextRecord->EFlags)) + c_rst);
#endif

    g_tui.add_exception(lines);

    return EXCEPTION_CONTINUE_SEARCH;
}

#endif