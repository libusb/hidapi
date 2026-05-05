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
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#ifndef _WIN32
#include <signal.h>
#endif

namespace {

std::atomic<bool> g_terminate{false};

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
    /* async-signal-safe: atomic store only. Main thread will call
       hid_read_interrupt() once cin.get() returns from EINTR. */
    g_terminate.store(true, std::memory_order_release);
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

            if (hid_is_read_interrupted(dev))
                break;

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
                  << "  vid, pid: hex (e.g. 04d8 003f)\n";
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
    /* Use sigaction without SA_RESTART so cin.get() returns on signal. */
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

    std::cin.get();  /* Enter, EOF, or POSIX-EINTR from a signal */

    hid_read_interrupt(dev);  /* idempotent — also safe if signal handler ran first */
    reader.join();

    hid_close(dev);
    hid_exit();
    return 0;
}
