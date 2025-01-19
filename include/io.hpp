#pragma once

#include <cstdio>
#include <functional>
#include <tuple>

namespace buxtehude {

struct Field;
class Stream;

using Callback = std::function<void(Stream&, Field&)>;

enum StreamStatus
{
    REACHED_EOF, STREAM_OKAY
};

// Owns the data pointed to by `data`
struct Field
{
    Field* prev = nullptr, *next = nullptr;
    void* data = nullptr;
    size_t length, capacity;
    Callback cb;

    template<typename T>
    T Get() { return *(T*)data; }

    template<typename T>
    std::pair<const T*, size_t> GetPtr()
    {
        return { (const T*)data, length };
    }

    std::string_view GetView()
    {
        return { (const char*) data, length };
    }

    Field& operator[](int offset);

    ~Field();
};

// Owns the Field pointers
struct FieldList
{
    Field* head = nullptr;

    FieldList() = default;
    ~FieldList();

    void Append(Field* f);
    void Remove(Field* f);
};

class Stream
{
public:
    Stream() = default;
    Stream(FILE* file);

    template<typename T=void>
    Stream& Await(size_t len=sizeof(T))
    {
        Field* f;
        if (!deleted.head) {
            f = new Field;
            f->data = malloc(len);
            f->capacity = len;
        } else {
            for (f = deleted.head; f->next; f = f->next)
                if (f->capacity >= len) break;

            if (f->capacity < len) {
                f->data = realloc(f->data, len);
                f->capacity = len;
            }
            deleted.Remove(f);
        }

        f->cb = {};
        f->length = len;
        fields.Append(f);

        return *this;
    }

    Stream& Then(const Callback& cb);
    void Finally(const Callback& cb);
    void Delete(Field& f);

    bool Read();
    bool Done();
    StreamStatus Status();
    void Reset();
    void Rewind(int offset);

    Field& operator[](int offset);

    FILE* file = nullptr;
private:
    Callback finally;

    FieldList fields, deleted;
    Field* current = nullptr;
    size_t data_offset = 0;
    StreamStatus status = STREAM_OKAY;
    bool done = false;
};

}
