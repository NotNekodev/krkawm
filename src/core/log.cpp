#include <core/log.hpp>
#include <iostream>

Logger::Logger(const std::string &logFile, size_t maxBufferSize)
    : maxBufferSize(maxBufferSize), running(true), dirty(false) {
    outFile.open(logFile, std::ios::out | std::ios::app);
    if (!outFile.is_open()) {
        std::cerr << "Logger: Failed to open log file: " << logFile
                  << std::endl;
        std::exit(1);
    }
    worker = std::thread(&Logger::run, this);
}

Logger::~Logger() {
    {
        std::lock_guard<std::mutex> lock(mtx);
        running = false;
        cv.notify_all();
    }
    if (worker.joinable())
        worker.join();
    flush();
    outFile.close();
}

void Logger::log(LogLevel level, const std::string &message) {
    std::ostringstream oss;
    oss << "[" << timestamp() << "] [" << levelToString(level) << "] "
        << message;

    {
        std::lock_guard<std::mutex> lock(mtx);
        if (buffer.size() >= maxBufferSize)
            buffer.erase(buffer.begin()); // Drop oldest
        buffer.push_back(oss.str());
        dirty = true;
    }
    cv.notify_one();

    if (level == LogLevel::FATAL) {
        flush();
        std::abort(); // crash intentionally after flushing
    }
}

void Logger::flush() {
    std::vector<std::string> temp;
    {
        std::lock_guard<std::mutex> lock(mtx);
        temp.swap(buffer);
        dirty = false;
    }
    for (const auto &line : temp)
        outFile << line << "\n";
    outFile.flush();
}

void Logger::run() {
    while (running) {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::milliseconds(500), [&] {
            return !running || dirty;
        });

        if (!running && buffer.empty())
            break;

        std::vector<std::string> temp;
        temp.swap(buffer);
        dirty = false;
        lock.unlock();

        for (const auto &line : temp)
            outFile << line << "\n";
        outFile.flush();
    }
}

std::string Logger::timestamp() {
    using namespace std::chrono;
    auto now  = system_clock::now();
    auto time = system_clock::to_time_t(now);
    auto ms   = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm{};
    localtime_r(&time, &tm);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%F %T") << "." << std::setfill('0')
        << std::setw(3) << ms.count();
    return oss.str();
}

std::string Logger::levelToString(LogLevel level) {
    switch (level) {
    case LogLevel::DEBUG:
        return "DEBUG";
    case LogLevel::INFO:
        return "INFO";
    case LogLevel::WARN:
        return "WARN";
    case LogLevel::ERROR:
        return "ERROR";
    case LogLevel::FATAL:
        return "FATAL";
    default:
        return "UNKNOWN";
    }
}

void Logger::dumpBufferToFile(const std::string &path) {
    std::lock_guard<std::mutex> lock(mtx);
    std::ofstream crashFile(path);
    for (const auto &line : buffer)
        crashFile << line << "\n";
}
