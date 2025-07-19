#ifndef CHECKS_HPP
#define CHECKS_HPP

#define CHECK_NOTNULL(x)                                                       \
    ((x) ? (x) : throw std::runtime_error("Null pointer exception: " #x))

struct Vec2 {
    float x, y;
};

#endif // CHECKS_HPP
