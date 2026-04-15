#include "../include/allocator_boundary_tags.h"

#include <cstddef>
#include <new>
#include <stdexcept>

constexpr size_t get_block_alignment() noexcept
{
    return alignof(std::max_align_t);
}

bool align_pointer(void *&ptr, size_t &available_space, size_t alignment, size_t size) noexcept
{
    return std::align(alignment, size, ptr, available_space) != nullptr;
}

void allocator_boundary_tags::release_trusted_memory() noexcept
{
    if (_trusted_memory == nullptr) return;

    auto *meta = reinterpret_cast<allocator_meta*>(_trusted_memory);
    auto *parent = meta->parent_allocator;
    size_t allocated_size = meta->allocated_size;

    meta->~allocator_meta();
    parent->deallocate(_trusted_memory, allocated_size);
    _trusted_memory = nullptr;
}

void *allocator_boundary_tags::pool_begin() const noexcept
{
    if (_trusted_memory == nullptr) return nullptr;

    auto *meta = reinterpret_cast<allocator_meta*>(_trusted_memory);
    void *begin = static_cast<std::byte*>(_trusted_memory) + allocator_metadata_size;
    size_t available_space = meta->total_size + get_block_alignment();
    if (!align_pointer(begin, available_space, get_block_alignment(), occupied_block_metadata_size)) {
        return nullptr;
    }

    return begin;
}

void *allocator_boundary_tags::pool_end() const noexcept
{
    if (_trusted_memory == nullptr) return nullptr;

    auto *meta = reinterpret_cast<allocator_meta*>(_trusted_memory);
    return static_cast<std::byte*>(pool_begin()) + meta->total_size;
}

allocator_boundary_tags::block_metadata *allocator_boundary_tags::find_block_by_payload(void *payload) const
{
    auto *meta = reinterpret_cast<allocator_meta*>(_trusted_memory);
    auto *pool_begin_bytes = static_cast<std::byte*>(pool_begin());
    auto *pool_end_bytes = static_cast<std::byte*>(pool_end());

    for (auto *block = reinterpret_cast<block_metadata*>(meta->first_occupied);
        block != nullptr;
        block = reinterpret_cast<block_metadata*>(block->next_block)) {
        auto *block_bytes = reinterpret_cast<std::byte*>(block);

        if (block_bytes < pool_begin_bytes ||
            block_bytes + occupied_block_metadata_size > pool_end_bytes ||
            block_bytes + block->block_size > pool_end_bytes ||
            block->owner != _trusted_memory) {
            throw std::logic_error("Allocator metadata is corrupted");
        }

        if (block_bytes + occupied_block_metadata_size == payload) {
            return block;
        }
    }

    return nullptr;
}

allocator_boundary_tags::~allocator_boundary_tags()
{
    release_trusted_memory();
}

allocator_boundary_tags::allocator_boundary_tags(allocator_boundary_tags &&other) noexcept
    : _trusted_memory(nullptr)
{
    if (other._trusted_memory == nullptr) return;

    auto *meta = reinterpret_cast<allocator_meta*>(other._trusted_memory);
    std::lock_guard<std::mutex> lock(meta->mtx);

    _trusted_memory = other._trusted_memory;
    other._trusted_memory = nullptr;
}

allocator_boundary_tags &allocator_boundary_tags::operator=(allocator_boundary_tags &&other) noexcept
{
    if (this == &other) return *this;

    void *old_memory = nullptr;
    std::pmr::memory_resource *old_parent = nullptr;
    size_t old_allocated_size = 0;

    if (_trusted_memory != nullptr && other._trusted_memory != nullptr) {
        auto *this_meta = reinterpret_cast<allocator_meta*>(_trusted_memory);
        auto *other_meta = reinterpret_cast<allocator_meta*>(other._trusted_memory);

        {
            std::scoped_lock lock(this_meta->mtx, other_meta->mtx);

            old_memory = _trusted_memory;
            old_parent = this_meta->parent_allocator;
            old_allocated_size = this_meta->allocated_size;

            _trusted_memory = other._trusted_memory;
            other._trusted_memory = nullptr;
        }

        reinterpret_cast<allocator_meta*>(old_memory)->~allocator_meta();
        old_parent->deallocate(old_memory, old_allocated_size);
    }
    else if (_trusted_memory != nullptr) {
        auto *this_meta = reinterpret_cast<allocator_meta*>(_trusted_memory);

        {
            std::lock_guard<std::mutex> lock(this_meta->mtx);

            old_memory = _trusted_memory;
            old_parent = this_meta->parent_allocator;
            old_allocated_size = this_meta->allocated_size;

            _trusted_memory = nullptr;
        }

        reinterpret_cast<allocator_meta*>(old_memory)->~allocator_meta();
        old_parent->deallocate(old_memory, old_allocated_size);
    }
    else if (other._trusted_memory != nullptr) {
        auto *other_meta = reinterpret_cast<allocator_meta*>(other._trusted_memory);

        std::lock_guard<std::mutex> lock(other_meta->mtx);
        _trusted_memory = other._trusted_memory;
        other._trusted_memory = nullptr;
    }

    return *this;
}


allocator_boundary_tags::allocator_boundary_tags(
        size_t space_size,
        std::pmr::memory_resource *parent_allocator,
        allocator_with_fit_mode::fit_mode allocate_fit_mode)
{
    if (parent_allocator == nullptr) parent_allocator = std::pmr::get_default_resource();

    constexpr size_t alignment = get_block_alignment();
    size_t allocated_size = allocator_metadata_size + space_size + alignment;
    _trusted_memory = parent_allocator->allocate(allocated_size);

    try {
        auto *meta = new (_trusted_memory) allocator_meta();
        meta->parent_allocator = parent_allocator;
        meta->mode = allocate_fit_mode;
        meta->allocated_size = allocated_size;
        meta->first_occupied = nullptr;

        void *pool_start = static_cast<std::byte*>(_trusted_memory) + allocator_metadata_size;
        size_t available_space = space_size + alignment;
        if (!align_pointer(pool_start, available_space, alignment, occupied_block_metadata_size) ||
            available_space < occupied_block_metadata_size) {
            meta->~allocator_meta();
            parent_allocator->deallocate(_trusted_memory, allocated_size);
            _trusted_memory = nullptr;
            throw std::bad_alloc();
        }

        meta->total_size = space_size;
    } catch (...) {
        if (_trusted_memory != nullptr) {
            parent_allocator->deallocate(_trusted_memory, allocated_size);
            _trusted_memory = nullptr;
        }
        throw;
    }
}

[[nodiscard]] void *allocator_boundary_tags::do_allocate_sm(size_t size)
{
    if (_trusted_memory == nullptr) {
        throw std::logic_error("Cannot allocate");
    }

    auto *meta = reinterpret_cast<allocator_meta*>(_trusted_memory);
    std::lock_guard<std::mutex> lock(meta->mtx);

    size_t required_block_size = size + occupied_block_metadata_size;
    if (required_block_size > meta->total_size) {
        throw std::bad_alloc();
    }

    void *target_begin = nullptr;
    size_t target_size = 0;
    block_metadata *target_previous = nullptr;
    block_metadata *target_next = nullptr;

    auto *memory_begin = static_cast<std::byte*>(pool_begin());
    auto *memory_end = static_cast<std::byte*>(pool_end());
    auto *first = reinterpret_cast<block_metadata*>(meta->first_occupied);

    if (first == nullptr) {
        target_begin = memory_begin;
        target_size = meta->total_size;
    } else {
        size_t free_before_first = reinterpret_cast<std::byte*>(first) - memory_begin;
        if (free_before_first >= required_block_size) {
            target_begin = memory_begin;
            target_size = free_before_first;
            target_next = first;
        }

        for (auto *current = first; current != nullptr; current = reinterpret_cast<block_metadata*>(current->next_block)) {
            auto *gap_begin = reinterpret_cast<std::byte*>(current) + current->block_size;
            auto *next = reinterpret_cast<block_metadata*>(current->next_block);
            auto *gap_end = next == nullptr ? memory_end : reinterpret_cast<std::byte*>(next);
            size_t gap_size = gap_end - gap_begin;

            if (gap_size < required_block_size) continue;

            bool take_gap = target_begin == nullptr;
            if (meta->mode == allocator_with_fit_mode::fit_mode::first_fit) {
                take_gap = target_begin == nullptr;
            } else if (meta->mode == allocator_with_fit_mode::fit_mode::the_best_fit) {
                take_gap = target_begin == nullptr || gap_size < target_size;
            } else {
                take_gap = target_begin == nullptr || gap_size > target_size;
            }

            if (take_gap) {
                target_begin = gap_begin;
                target_size = gap_size;
                target_previous = current;
                target_next = next;
            }

            if (meta->mode == allocator_with_fit_mode::fit_mode::first_fit && target_begin != nullptr) {
                break;
            }
        }
    }

    if (target_begin == nullptr) {
        throw std::bad_alloc();
    }

    size_t block_size = required_block_size;
    if (target_size - required_block_size < occupied_block_metadata_size) {
        block_size = target_size;
    }

    auto *block = new (target_begin) block_metadata();
    block->block_size = block_size;
    block->previous_block = target_previous;
    block->next_block = target_next;
    block->owner = _trusted_memory;

    if (target_previous != nullptr) {
        target_previous->next_block = block;
    } else {
        meta->first_occupied = block;
    }

    if (target_next != nullptr) {
        target_next->previous_block = block;
    }

    return static_cast<std::byte*>(target_begin) + occupied_block_metadata_size;
}

void allocator_boundary_tags::do_deallocate_sm(void *at)
{
    if (at == nullptr) return;
    if (_trusted_memory == nullptr) {
        throw std::logic_error("Cannot deallocate");
    }

    auto *meta = reinterpret_cast<allocator_meta*>(_trusted_memory);
    std::lock_guard<std::mutex> lock(meta->mtx);

    auto *payload = static_cast<std::byte*>(at);
    if (payload < static_cast<std::byte*>(pool_begin()) + occupied_block_metadata_size ||
        payload >= static_cast<std::byte*>(pool_end())) {
        throw std::logic_error("Pointer does not belong to this allocator");
    }

    auto *block = find_block_by_payload(at);
    if (block == nullptr) {
        throw std::logic_error("Pointer was not allocated by this allocator");
    }
    

    auto *previous = reinterpret_cast<block_metadata*>(block->previous_block);
    auto *next = reinterpret_cast<block_metadata*>(block->next_block);

    if (previous != nullptr) {
        previous->next_block = next;
    } else {
        meta->first_occupied = next;
    }

    if (next != nullptr) {
        next->previous_block = previous;
    }
}

inline void allocator_boundary_tags::set_fit_mode(allocator_with_fit_mode::fit_mode mode)
{
    if (_trusted_memory == nullptr) {
        throw std::logic_error("Cannot change fit mode");
    }

    auto *meta = reinterpret_cast<allocator_meta*>(_trusted_memory);
    std::lock_guard<std::mutex> lock(meta->mtx);
    meta->mode = mode;
}

std::vector<allocator_test_utils::block_info> allocator_boundary_tags::get_blocks_info() const
{
    if (_trusted_memory == nullptr) return {};

    auto *meta = reinterpret_cast<allocator_meta*>(_trusted_memory);
    std::lock_guard<std::mutex> lock(meta->mtx);

    return get_blocks_info_inner();
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::begin() const noexcept
{
    return boundary_iterator(_trusted_memory);
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::end() const noexcept
{
    return boundary_iterator();
}

std::vector<allocator_test_utils::block_info> allocator_boundary_tags::get_blocks_info_inner() const
{
    std::vector<allocator_test_utils::block_info> result;

    for (auto it = begin(); it != end(); ++it) {
        if (it.size() == 0) continue;
        allocator_test_utils::block_info info;
        info.block_size = it.size();
        info.is_block_occupied = it.occupied();
        result.push_back(info);
    }

    return result;
}

bool allocator_boundary_tags::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    if (this == &other) return true;

    auto *other_allocator = dynamic_cast<allocator_boundary_tags const*>(&other);
    if (other_allocator == nullptr) return false;

    return _trusted_memory == other_allocator->_trusted_memory;
}

bool allocator_boundary_tags::boundary_iterator::operator==(
        const allocator_boundary_tags::boundary_iterator &other) const noexcept
{
    return _occupied_ptr == other._occupied_ptr &&
           _occupied == other._occupied &&
           _trusted_memory == other._trusted_memory;
}

bool allocator_boundary_tags::boundary_iterator::operator!=(
        const allocator_boundary_tags::boundary_iterator &other) const noexcept
{
    return !(*this == other);
}

allocator_boundary_tags::boundary_iterator &allocator_boundary_tags::boundary_iterator::operator++() & noexcept
{
    if (_trusted_memory == nullptr) return *this;

    if (_occupied_ptr == nullptr) {
        _trusted_memory = nullptr;
        return *this;
    }

    auto *block = reinterpret_cast<block_metadata*>(_occupied_ptr);
    if (!_occupied) {
        _occupied = true;
        return *this;
    }

    if (block->next_block == nullptr) {
        _occupied_ptr = nullptr;
        _occupied = false;
        return *this;
    }

    auto *next_block = reinterpret_cast<block_metadata*>(block->next_block);
    auto *expected_next = reinterpret_cast<std::byte*>(block) + block->block_size;
    _occupied_ptr = next_block;
    _occupied = reinterpret_cast<std::byte*>(next_block) == expected_next;

    return *this;
}

allocator_boundary_tags::boundary_iterator &allocator_boundary_tags::boundary_iterator::operator--() & noexcept
{
    if (_trusted_memory == nullptr) return *this;

    auto *meta = reinterpret_cast<allocator_meta*>(_trusted_memory);
    auto *memory_begin = static_cast<std::byte*>(_trusted_memory) + allocator_metadata_size;

    if (_occupied) {
        auto *block = reinterpret_cast<block_metadata*>(_occupied_ptr);
        auto *previous = reinterpret_cast<block_metadata*>(block->previous_block);
        auto *block_begin = reinterpret_cast<std::byte*>(block);
        auto *previous_end = previous == nullptr
            ? memory_begin
            : reinterpret_cast<std::byte*>(previous) + previous->block_size;

        if (block_begin == previous_end) {
            _occupied_ptr = previous;
            _occupied = previous != nullptr;
        } else {
            _occupied = false;
        }
        return *this;
    }

    if (_occupied_ptr == nullptr) {
        auto *block = reinterpret_cast<block_metadata*>(meta->first_occupied);
        if (block == nullptr) return *this;

        while (block->next_block != nullptr) {
            block = reinterpret_cast<block_metadata*>(block->next_block);
        }

        _occupied_ptr = block;
        _occupied = true;
        return *this;
    }

    auto *block = reinterpret_cast<block_metadata*>(_occupied_ptr);
    _occupied_ptr = block->previous_block;
    _occupied = _occupied_ptr != nullptr;

    return *this;
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::boundary_iterator::operator++(int)
{
    auto tmp = *this;
    ++(*this);
    return tmp;
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::boundary_iterator::operator--(int)
{
    auto tmp = *this;
    --(*this);
    return tmp;
}

size_t allocator_boundary_tags::boundary_iterator::size() const noexcept
{
    if (_trusted_memory == nullptr) return 0;

    auto *meta = reinterpret_cast<allocator_meta*>(_trusted_memory);
    auto *begin = static_cast<std::byte*>(_trusted_memory) + allocator_metadata_size;
    auto *end = begin + meta->total_size;

    if (_occupied) {
        return reinterpret_cast<block_metadata*>(_occupied_ptr)->block_size;
    }

    if (_occupied_ptr == nullptr) {
        if (meta->first_occupied == nullptr) {
            return meta->total_size;
        }

        auto *last = reinterpret_cast<block_metadata*>(meta->first_occupied);
        while (last->next_block != nullptr) {
            last = reinterpret_cast<block_metadata*>(last->next_block);
        }

        return end - (reinterpret_cast<std::byte*>(last) + last->block_size);
    }

    auto *next_block = reinterpret_cast<block_metadata*>(_occupied_ptr);
    if (next_block->previous_block == nullptr) {
        return reinterpret_cast<std::byte*>(next_block) - begin;
    }

    auto *previous_block = reinterpret_cast<block_metadata*>(next_block->previous_block);
    return reinterpret_cast<std::byte*>(next_block) -
           (reinterpret_cast<std::byte*>(previous_block) + previous_block->block_size);
}

bool allocator_boundary_tags::boundary_iterator::occupied() const noexcept
{
    return _occupied;
}

void *allocator_boundary_tags::boundary_iterator::operator*() const noexcept
{
    if (_occupied) {
        return static_cast<std::byte*>(_occupied_ptr) + occupied_block_metadata_size;
    }

    return get_ptr();
}

allocator_boundary_tags::boundary_iterator::boundary_iterator()
    : _occupied_ptr(nullptr), _occupied(false), _trusted_memory(nullptr)
{
}

allocator_boundary_tags::boundary_iterator::boundary_iterator(void *trusted)
    : _occupied_ptr(nullptr), _occupied(false), _trusted_memory(trusted)
{
    if (trusted == nullptr) return;

    auto *meta = reinterpret_cast<allocator_meta*>(trusted);
    if (meta->first_occupied == nullptr) return;

    auto *begin = static_cast<std::byte*>(trusted) + allocator_metadata_size;
    _occupied_ptr = meta->first_occupied;
    _occupied = _occupied_ptr == begin;
}

void *allocator_boundary_tags::boundary_iterator::get_ptr() const noexcept
{
    if (_trusted_memory == nullptr) return nullptr;

    auto *begin = static_cast<std::byte*>(_trusted_memory) + allocator_metadata_size;

    if (_occupied) {
        return _occupied_ptr;
    }

    if (_occupied_ptr == nullptr) {
        return begin;
    }

    auto *next_block = reinterpret_cast<block_metadata*>(_occupied_ptr);
    if (next_block->previous_block == nullptr) {
        return begin;
    }

    auto *previous_block = reinterpret_cast<block_metadata*>(next_block->previous_block);
    return reinterpret_cast<std::byte*>(previous_block) + previous_block->block_size;
}
