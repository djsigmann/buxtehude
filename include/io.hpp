#pragma once

#include <cstdio>
#include <functional>
#include <tuple>
#include <list>

namespace buxtehude
{

struct Field;
class Stream;

using Callback = std::function<void(Stream&, Field&)>;
using FieldIterator = std::list<Field>::iterator;

enum StreamStatus
{
    REACHED_EOF, STREAM_OKAY
};

struct Field
{
    std::vector<uint8_t> data;
    size_t length;
    FieldIterator self_iterator;
    Callback cb;

    Field(size_t length) : length(length) { data.reserve(length); }

    template<typename T>
    T Get() { return *(T*) data.data(); }

    template<typename T>
    std::pair<const T*, size_t> GetPtr()
    {
        return { (const T*) data.data(), length };
    }

    std::string_view GetView()
    {
        return { (const char*) data.data(), length };
    }

    Field& operator[](int offset);
};

class Stream
{
public:
    Stream() = default;
    Stream(FILE* file);

    template<typename T=void>
    Stream& Await(size_t len=sizeof(T))
    {
        FieldIterator iter = fields.emplace(fields.end(), len);
        Field& new_field = *iter;
        new_field.self_iterator = iter;

        if (deleted.empty()) {
            new_field.data.reserve(len);
        } else {
            auto reusable = std::find_if(deleted.begin(), deleted.end(),
                [len] (const Field& f) {
                    return f.data.capacity() >= len;
                }
            );

            if (reusable == deleted.end()) {
                new_field.data.reserve(len);
                deleted.erase(deleted.begin());
            } else {
                new_field.data = std::move(reusable->data);
                deleted.erase(reusable);
            }
        }

        return *this;
    }

    Stream& Then(Callback&& cb);
    void Finally(Callback&& cb);
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

    std::list<Field> fields, deleted;
    FieldIterator current = fields.end();
    size_t data_offset = 0;
    StreamStatus status = STREAM_OKAY;
    bool done = false;
};

}
