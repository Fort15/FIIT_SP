#include <not_implemented.h>
#include "../include/allocator_global_heap.h"

allocator_global_heap::allocator_global_heap()
{
}

[[nodiscard]] void *allocator_global_heap::do_allocate_sm(
    size_t size)
{
    std::lock_guard<std::mutex> lock(_mutex);
    return ::operator new(size);
}

void allocator_global_heap::do_deallocate_sm(
    void *at)
{
    std::lock_guard<std::mutex> lock(_mutex);
    ::operator delete(at);
}

allocator_global_heap::~allocator_global_heap()
{
}

allocator_global_heap::allocator_global_heap(const allocator_global_heap &other)
{
}

allocator_global_heap &allocator_global_heap::operator=(const allocator_global_heap &other)
{
    if (this != &other) {
        (void)other;
    }
    return *this;
}

bool allocator_global_heap::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    return dynamic_cast<const allocator_global_heap *>(&other);
}

allocator_global_heap::allocator_global_heap(allocator_global_heap &&other) noexcept
{
    // нет состояния для перемещения
    (void)other;
}

allocator_global_heap &allocator_global_heap::operator=(allocator_global_heap &&other) noexcept
{
    if (this != &other) {
        (void)other;
    }
    return *this;
}