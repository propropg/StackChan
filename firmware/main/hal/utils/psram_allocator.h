/*
 * STL allocator that places buffers in PSRAM (audio clips are too big for internal RAM)
 */
#pragma once
#include <esp_heap_caps.h>
#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>

template <typename T>
struct PsramAllocator {
    using value_type = T;
    PsramAllocator() = default;
    template <typename U>
    PsramAllocator(const PsramAllocator<U>&)
    {
    }
    T* allocate(size_t n)
    {
        auto* p = static_cast<T*>(heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!p) {
            throw std::bad_alloc();
        }
        return p;
    }
    void deallocate(T* p, size_t)
    {
        heap_caps_free(p);
    }
    template <typename U>
    bool operator==(const PsramAllocator<U>&) const
    {
        return true;
    }
    template <typename U>
    bool operator!=(const PsramAllocator<U>&) const
    {
        return false;
    }
};

using PsramBytes = std::vector<uint8_t, PsramAllocator<uint8_t>>;
