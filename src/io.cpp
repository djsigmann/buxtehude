#include "io.hpp"

#include <cstdio>

namespace buxtehude {

// Field

Field::~Field() { if (data) free(data); }

Field& Field::operator[](int offset)
{
    Field* f;
    if (offset < 0)
        for (f = this; f && offset; f = f->prev) ++offset;
    else
        for (f = this; f && offset; f = f->next) --offset;

    return *f;
}

// FieldList

FieldList::~FieldList()
{
    Field* p, *next;
    for (p = head; p; p = next) {
        next = p->next;
        delete p;
    }
}

void FieldList::Append(Field* f)
{
    Field* poi;
    for (poi = head; poi; poi = poi->next) if (!poi->next) break;

    if (!poi) {
        head = f;
        head->next = nullptr;
        head->prev = nullptr;
    } else {
        poi->next = f;
        f->prev = poi;
        f->next = nullptr;
    }
}

void FieldList::Remove(Field* f)
{
    Field* por;
    for (por = head; por && por != f; por = por->next);

    if (!por) return;
    if (por->prev) por->prev->next = por->next;
    if (por->next) por->next->prev = por->prev;
}

// Stream

Stream::Stream(FILE* f) : file(f) {}

Stream& Stream::Then(const Callback& cb)
{
    Field* last;
    for (last = fields.head; last->next; last = last->next);

    last->cb = cb;

    return *this;
}

void Stream::Finally(const Callback& cb) { finally = cb; }

void Stream::Delete(Field& f)
{
    fields.Remove(&f);
    deleted.Append(&f);
}

bool Stream::Read()
{
    done = false;
    while (true) {
        if (!current) { current = fields.head; }
        size_t expected = current->length - data_offset;

        size_t bytes = fread((uint8_t*)current->data + data_offset, 1, expected, file);

        if (feof(file)) status = REACHED_EOF;
        else status = STREAM_OKAY;

        data_offset += bytes;
        if (data_offset < current->length) return false;

        data_offset = 0;
        Field* old_c = current;
        if (current->cb) current->cb(*this, *current);

        if (!current) continue; // Reset

        Field* next = current == old_c ? current->next : old_c;
        if (!next) { // Reached the end
            if (finally) finally(*this, *current);
            done = true;
            return true;
        } else current = next;
    }

    return false;
}

bool Stream::Done() { return done; }

StreamStatus Stream::Status() { return status; }

void Stream::Reset() { current = nullptr; }

void Stream::Rewind(int offset) { current = &(*current)[-offset]; }

Field& Stream::operator[](int offset) { return (*fields.head)[offset]; }

}
