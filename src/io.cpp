#include "io.hpp"

#include <cstdio>

namespace buxtehude
{

// Field

Field& Field::operator[](int offset)
{
    FieldIterator iter = self_iterator;
    if (offset < 0)
        for (; offset < 0; ++offset) --iter;
    else
        for (; offset > 0; --offset) ++iter;

    return *iter;
}

// Stream

Stream::Stream(FILE* f) : file(f) {}

Stream& Stream::Then(Callback&& cb)
{
    fields.rbegin()->cb = std::move(cb);

    return *this;
}

void Stream::Finally(Callback&& cb) { finally = std::move(cb); }

void Stream::Delete(Field& f)
{
    FieldIterator iter_to_erase = f.self_iterator;
    deleted.emplace_back(std::move(f));
    fields.erase(iter_to_erase);
}

bool Stream::Read()
{
    done = false;
    while (true) {
        if (fields.empty()) {
            status = StreamStatus::OKAY;
            return true;
        }

        if (current == fields.end()) current = fields.begin();
        size_t expected = current->length - data_offset;

        // Evil writing to vector through the data pointer, but we
        // don't care about vector::size() anyways. cppreference says data()
        // "may or may not" return nullptr. I think reserve() should guarantee
        // a valid data pointer given no allocation failure.
        size_t bytes_read
            = fread(reinterpret_cast<uint8_t*>(current->data.data()) + data_offset, 1,
                    expected, file);

        if (feof(file)) status = StreamStatus::REACHED_EOF;
        else status = StreamStatus::OKAY;

        data_offset += bytes_read;
        if (data_offset < current->length) return false;

        data_offset = 0;
        auto old_current = current;
        if (current->cb) current->cb(*this, *current);

        if (current == fields.end()) continue; // The stream has been reset

        if (current == old_current) ++current;

        if (current == fields.end()) { // Reached the end
            if (finally) finally(*this, *fields.rbegin());
            done = true;
            return true;
        }
    }

    return false;
}

bool Stream::Done() { return done; }

StreamStatus Stream::Status() { return status; }

void Stream::Reset() { current = fields.end(); }

void Stream::Rewind(int offset)
{
    for (; offset > 0; --offset) --current;
}

Field& Stream::operator[](int offset)
{
    FieldIterator iter = fields.begin();
    for (; offset > 0; --offset) ++iter;
    return *iter;
}

}
