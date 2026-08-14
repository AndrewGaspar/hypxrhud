#include <signal.h>
#include <string_view>
#include <unistd.h>

int main() {
    constexpr std::string_view first =
        R"({"event_name":"KEYBOARD_KEY","event_type":300,"time_stamp":1,"key_name":"KEY_A","key_code":30,"state_name":"PRESSED","state_code":1})";
    constexpr std::string_view second =
        R"({"event_name":"KEYBOARD_KEY","event_type":300,"time_stamp":2,"key_name":"KEY_A","key_code":30,"state_name":"RELEASED","state_code":0})";

    write(STDOUT_FILENO, first.data(), first.size() / 2);
    usleep(10000);
    write(STDOUT_FILENO, first.data() + first.size() / 2, first.size() - first.size() / 2);
    write(STDOUT_FILENO, "\n", 1);
    write(STDOUT_FILENO, second.data(), second.size());
    write(STDOUT_FILENO, "\n", 1);
    for (;;)
        pause();
}
