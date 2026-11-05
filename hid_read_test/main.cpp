/*******************************************************
 HIDAPI - hid_read_test

 A small C++11 cmd-line tool that opens a HID device by
 VID/PID, spawns a read thread that prints input reports
 as hex with timestamps, and waits for Enter or Ctrl+C
 to gracefully interrupt the read and exit.

 The contents of this file may be used by anyone for any
 reason without any conditions and may be used as a
 starting point for your own applications which use HIDAPI.
********************************************************/

#include <hidapi.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace {

/* Signal handlers may only use atomic operations that are lock-free. */
static_assert(ATOMIC_INT_LOCK_FREE == 2, "Signal handling requires lock-free atomic<int>");
std::atomic<int> g_terminate(0);
std::mutex g_interrupt_mutex;

std::string timestamp_now()
{
    using namespace std::chrono;
    auto now = system_clock::now();
    auto t = system_clock::to_time_t(now);
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif

    std::ostringstream ss;
    ss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S")
       << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

extern "C" void on_signal(int)
{
    g_terminate.store(1, std::memory_order_relaxed);
}

/* Keep input waits bounded so a signal can stop the reader without leaving
   a helper thread blocked on stdin during process shutdown. */
void wait_for_exit()
{
#ifdef _WIN32
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    bool console = GetConsoleMode(input, &mode) != 0;
    DWORD type = GetFileType(input);
#endif
    while (!g_terminate.load(std::memory_order_relaxed)) {
#ifdef _WIN32
        if (console) {
            DWORD ready = WaitForSingleObject(input, 50);
            if (ready == WAIT_TIMEOUT)
                continue;
            if (ready != WAIT_OBJECT_0)
                return;
            INPUT_RECORD event;
            DWORD count;
            if (!ReadConsoleInputW(input, &event, 1, &count))
                return;
            if (count && event.EventType == KEY_EVENT && event.Event.KeyEvent.bKeyDown &&
                (event.Event.KeyEvent.wVirtualKeyCode == VK_RETURN ||
                 event.Event.KeyEvent.uChar.UnicodeChar == L'\x1a'))
                return;
            continue;
        }
        if (type == FILE_TYPE_PIPE) {
            DWORD available;
            if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr))
                return;  /* EOF or invalid input. */
            if (!available) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
        }
        char ch;
        DWORD count;
        if (!ReadFile(input, &ch, 1, &count, nullptr) || !count || ch == '\n')
            return;
#else
        struct pollfd input = {STDIN_FILENO, POLLIN, 0};
        int ready = poll(&input, 1, 50);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        if (!ready)
            continue;
        char ch;
        ssize_t count = read(STDIN_FILENO, &ch, 1);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0 || ch == '\n')
            return;
#endif
    }
}

void read_thread_fn(hid_device *dev)
{
    unsigned char buf[4096];
    const int max_retries = 3;
    int errors = 0;

    for (;;) {
        int n = hid_read_timeout(dev, buf, sizeof(buf), -1);
        if (n < 0) {
            std::cout << '[' << timestamp_now() << "] read returned -1";
            const wchar_t *err = hid_read_error(dev);
            if (err) std::wcout << L": " << err;
            std::cout << std::endl;

            {
                std::lock_guard<std::mutex> lock(g_interrupt_mutex);
                if (hid_is_read_interrupted(dev))
                    break;
            }

            if (++errors > max_retries) {
                std::cout << '[' << timestamp_now() << "] giving up after "
                          << max_retries << " consecutive errors" << std::endl;
                break;
            }
            continue;
        }

        errors = 0;  /* reset on a successful read */

        if (n == 0) continue;

        std::cout << '[' << timestamp_now() << "] ";
        std::cout << std::hex << std::setfill('0');
        for (int i = 0; i < n; ++i)
            std::cout << std::setw(2) << static_cast<unsigned>(buf[i]) << ' ';
        std::cout << std::dec << std::endl;
    }
    g_terminate.store(1, std::memory_order_relaxed);
}

unsigned short parse_hex_u16(const char *s)
{
    return static_cast<unsigned short>(std::strtoul(s, nullptr, 16));
}

}  // anonymous namespace

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <vid> <pid>\n"
                  << "  vid, pid: your device's vendor and product IDs in hexadecimal\n";
        return 1;
    }

    unsigned short vid = parse_hex_u16(argv[1]);
    unsigned short pid = parse_hex_u16(argv[2]);

    if (hid_init() != 0) {
        std::cerr << "hid_init failed\n";
        return 1;
    }

    hid_device *dev = hid_open(vid, pid, nullptr);
    if (!dev) {
        std::cerr << "hid_open failed for VID=" << std::hex << std::setw(4)
                  << std::setfill('0') << vid << " PID=" << std::setw(4) << pid
                  << std::dec << '\n';
        hid_exit();
        return 1;
    }
#ifdef _WIN32
    std::signal(SIGINT, on_signal);
#ifdef SIGTERM
    std::signal(SIGTERM, on_signal);
#endif
#else
    struct sigaction sa;
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
#endif

    std::cout << "Reading from VID=" << std::hex << std::setw(4) << std::setfill('0')
              << vid << " PID=" << std::setw(4) << pid << std::dec
              << "  (press Enter or Ctrl+C to exit)" << std::endl;

    std::thread reader(read_thread_fn, dev);

    wait_for_exit();
    {
        std::lock_guard<std::mutex> lock(g_interrupt_mutex);
        hid_read_interrupt(dev);
    }
    reader.join();

    hid_close(dev);
    hid_exit();
    return 0;
}
