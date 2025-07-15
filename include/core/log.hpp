#ifndef LOGGING_HPP
#define LOGGING_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

enum class LogLevel {
    DEBUG,
    INFO,
    WARN,
    ERROR,
    FATAL
};

class Logger {
public:
    Logger(const std::string &logFile, size_t maxBufferSize = 8192);
    ~Logger();

    void log(LogLevel level, const std::string &message);
    void flush(); // Force flush

    // Optional crash dump
    void dumpBufferToFile(const std::string &path);

private:
    void run();
    std::string levelToString(LogLevel level);
    std::string timestamp();

    std::ofstream outFile;
    std::vector<std::string> buffer;
    size_t maxBufferSize;

    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<bool> running;
    std::atomic<bool> dirty;

    std::thread worker;
};

#endif // LOGGING_HPP
