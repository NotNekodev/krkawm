#include <core/log.hpp>
#include <iostream>

Logger::Logger(const std::string &filename) : running(true) {
    outFile.open(filename, std::ios::app);
    if (!outFile)
        throw std::runtime_error("Cannot open log file");

    streams[LogLevel::DEBUG] =
        std::make_unique<LogStream>(*this, LogLevel::DEBUG);
    streams[LogLevel::INFO] =
        std::make_unique<LogStream>(*this, LogLevel::INFO);
    streams[LogLevel::WARN] =
        std::make_unique<LogStream>(*this, LogLevel::WARN);
    streams[LogLevel::ERROR] =
        std::make_unique<LogStream>(*this, LogLevel::ERROR);
    streams[LogLevel::FATAL] =
        std::make_unique<LogStream>(*this, LogLevel::FATAL);

    worker = std::thread(&Logger::workerLoop, this);
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

void Logger::enqueue(LogLevel level, const std::string &message) {
    std::ostringstream oss_file;
    std::ostringstream oss_term;

    std::string color;
    bool fullBold = false;

    switch (level) {
    case LogLevel::FATAL:
        color    = BOLD PINK;
        fullBold = true;
        break;
    case LogLevel::ERROR:
        color = BOLD RED;
        break;
    case LogLevel::WARN:
        color = BOLD YELLOW;
        break;
    case LogLevel::INFO:
        color = BOLD BLUE;
        break;
    case LogLevel::DEBUG:
        color = BOLD PURPLE;
        break;
    }

    std::string ts       = timestamp();
    std::string levelStr = levelToString(level);

    oss_file << "[" << ts << "] [" << levelStr << "] " << message;

    if (fullBold) {
        oss_term << color << "[" << ts << "] [" << levelStr << "] " << message
                 << RESET;
    } else {
        oss_term << color << "[" << ts << "] [" << levelStr << "]" << RESET
                 << " " << message;
    }

    {
        std::lock_guard<std::mutex> lock(mtx);
        buffer.push_back(oss_file.str());
    }
    cv.notify_one();

    // Print colored message to terminal
    if (level >= LogLevel::ERROR)
        std::cerr << oss_term.str();
    else
        std::cout << oss_term.str();

    if (level == LogLevel::FATAL) {
        flush();
        std::abort();
    }
}

void Logger::flush() {
    std::vector<std::string> tmp;
    {
        std::lock_guard<std::mutex> lock(mtx);
        tmp.swap(buffer);
    }
    for (const auto &line : tmp)
        outFile << line << "\n";
    outFile.flush();
}

void Logger::workerLoop() {
    while (running) {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::milliseconds(500));
        if (!buffer.empty()) {
            std::vector<std::string> tmp;
            tmp.swap(buffer);
            lock.unlock();
            for (const auto &line : tmp)
                outFile << line << "\n";
            outFile.flush();
        }
    }
}

std::string Logger::timestamp() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto t   = system_clock::to_time_t(now);
    auto ms  = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm{};
    localtime_r(&t, &tm);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%F %T") << "." << std::setw(3)
        << std::setfill('0') << ms.count();
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

LogStream &Logger::debug() {
    return *streams[LogLevel::DEBUG];
}
LogStream &Logger::info() {
    return *streams[LogLevel::INFO];
}
LogStream &Logger::warn() {
    return *streams[LogLevel::WARN];
}
LogStream &Logger::err() {
    return *streams[LogLevel::ERROR];
}
LogStream &Logger::fatal() {
    return *streams[LogLevel::FATAL];
}

// ----------- LogStreamBuf ------------

LogStreamBuf::LogStreamBuf(Logger &logger, LogLevel level)
    : logger(logger), level(level) {
}

LogStreamBuf::~LogStreamBuf() {
    sync();
}

int LogStreamBuf::overflow(int c) {
    if (c != EOF)
        buffer.put(static_cast<char>(c));
    return c;
}

int LogStreamBuf::sync() {
    std::string msg = buffer.str();
    if (!msg.empty()) {
        logger.enqueue(level, msg);
        buffer.str("");
        buffer.clear();
    }
    return 0;
}

// ------------ LogStream -------------

LogStream::LogStream(Logger &logger, LogLevel level)
    : std::ostream(&buf), buf(logger, level) {
}

LogStream::~LogStream() {
    flush();
}
