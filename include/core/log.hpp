#ifndef LOGGING_HPP
#define LOGGING_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#define RESET "\033[0m"
#define BOLD  "\033[1m"

#define PINK   "\033[35m"
#define RED    "\033[31m"
#define YELLOW "\033[33m"
#define BLUE   "\033[34m"
#define PURPLE "\033[95m"

enum class LogLevel {
    DEBUG,
    INFO,
    WARN,
    ERROR,
    FATAL
};

class Logger;

class LogStreamBuf : public std::streambuf {
public:
    LogStreamBuf(Logger &logger, LogLevel level);
    ~LogStreamBuf();

protected:
    int overflow(int c) override;
    int sync() override;

private:
    Logger &logger;
    LogLevel level;
    std::ostringstream buffer;
};

class LogStream : public std::ostream {
public:
    LogStream(Logger &logger, LogLevel level);
    ~LogStream();

private:
    LogStreamBuf buf;
};

class Logger {
public:
    Logger(const std::string &filename);
    ~Logger();

    LogStream &debug();
    LogStream &info();
    LogStream &warn();
    LogStream &err();
    LogStream &fatal();

    void enqueue(LogLevel level, const std::string &message);
    void flush();

private:
    friend class LogStreamBuf;

    void workerLoop();
    std::string timestamp();
    std::string levelToString(LogLevel level);

    std::ofstream outFile;
    std::vector<std::string> buffer;
    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<bool> running;
    std::thread worker;

    std::map<LogLevel, std::unique_ptr<LogStream>> streams;
};

#endif // LOGGING_HPP
