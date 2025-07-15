#define CHECK_NOTNULL(x)                                                       \
    ((x) ? (x) : throw std::runtime_error("Null pointer exception: " #x))

struct Vec2 {
    float x, y;
};
