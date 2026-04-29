#include "../include/allocator_buddies_system.h"

#include <cstddef>
#include <memory>
#include <new>
#include <stdexcept>

constexpr size_t block_alignment() noexcept
{
    return alignof(std::max_align_t);
}

bool align_pointer(void *&ptr, size_t &available_space, size_t alignment, size_t size) noexcept
{
    return std::align(alignment, size, ptr, available_space) != nullptr;
}

size_t power_to_size(unsigned char power) noexcept
{
    return static_cast<size_t>(1) << power;
}

allocator_buddies_system::~allocator_buddies_system()
{
    if (_trusted_memory == nullptr) return;

    auto *meta = reinterpret_cast<allocator_meta*>(_trusted_memory);
    auto *parent = meta->parent_allocator;
    size_t allocated_size = meta->allocated_size;

    meta->~allocator_meta();
    parent->deallocate(_trusted_memory, allocated_size);
    _trusted_memory = nullptr;
}

allocator_buddies_system::allocator_buddies_system(
    allocator_buddies_system &&other) noexcept
    : _trusted_memory(nullptr)
{
    if (other._trusted_memory == nullptr) return;

    auto *meta = reinterpret_cast<allocator_meta*>(other._trusted_memory);
    std::lock_guard<std::mutex> lock(meta->mtx);

    _trusted_memory = other._trusted_memory;
    other._trusted_memory = nullptr;
}

allocator_buddies_system &allocator_buddies_system::operator=(
    allocator_buddies_system &&other) noexcept
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
    } else if (_trusted_memory != nullptr) {
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
    } else if (other._trusted_memory != nullptr) {
        auto *other_meta = reinterpret_cast<allocator_meta*>(other._trusted_memory);
        std::lock_guard<std::mutex> lock(other_meta->mtx);
        _trusted_memory = other._trusted_memory;
        other._trusted_memory = nullptr;
    }

    return *this;
}

allocator_buddies_system::allocator_buddies_system(
        size_t space_size,
        std::pmr::memory_resource *parent_allocator,
        allocator_with_fit_mode::fit_mode allocate_fit_mode)
    : _trusted_memory(nullptr)
{
    if (space_size == 0) {
        throw std::logic_error("Allocator size must be positive");
    }

    unsigned char pool_k = static_cast<unsigned char>(__detail::nearest_greater_k_of_2(space_size));
    if (pool_k < min_k) {
        throw std::logic_error("Allocator space is too small for buddies system");
    }

    if (parent_allocator == nullptr) parent_allocator = std::pmr::get_default_resource();

    size_t pool_size = power_to_size(pool_k);
    size_t allocated_size = allocator_metadata_size + pool_size + block_alignment();
    _trusted_memory = parent_allocator->allocate(allocated_size);
    
    try {
        auto *meta = new (_trusted_memory) allocator_meta();
        meta->parent_allocator = parent_allocator;
        meta->mode = allocate_fit_mode;
        meta->pool_size_power = pool_k;
        meta->allocated_size = allocated_size;

        void *pool_start = static_cast<std::byte*>(_trusted_memory) + allocator_metadata_size;
        size_t available = pool_size + block_alignment();
        if (!align_pointer(pool_start, available, block_alignment(), occupied_block_metadata_size) ||
            available < pool_size) {
            meta->~allocator_meta();
            parent_allocator->deallocate(_trusted_memory, allocated_size);
            _trusted_memory = nullptr;
            throw std::bad_alloc();
        }

        auto *first_block = new (pool_start) block_metadata();
        first_block->occupied = false;
        first_block->size = pool_k;
    } catch (...) {
        if (_trusted_memory != nullptr) {
            parent_allocator->deallocate(_trusted_memory, allocated_size);
            _trusted_memory = nullptr;
        }
        throw;
    }
}

[[nodiscard]] void *allocator_buddies_system::do_allocate_sm(
    size_t size)
{
    if (_trusted_memory == nullptr) {
        throw std::logic_error("Cannot allocate");
    }

    auto *meta = reinterpret_cast<allocator_meta*>(_trusted_memory);
    std::lock_guard<std::mutex> lock(meta->mtx);

    size_t required = size + occupied_block_metadata_size;
    auto required_k = static_cast<unsigned char>(required <= 1 ? 0 : __detail::nearest_greater_k_of_2(required));
    if (required_k < min_k) {
        required_k = static_cast<unsigned char>(min_k);
    }

    if (required_k > meta->pool_size_power) {
        throw std::bad_alloc();
    }

    auto *pool_begin = static_cast<std::byte*>(begin()._block);
    auto *pool_end = pool_begin + power_to_size(meta->pool_size_power);

    block_metadata *target = nullptr;
    for (auto *current_ptr = pool_begin; current_ptr < pool_end; ) {
        auto *current = reinterpret_cast<block_metadata*>(current_ptr);
        size_t current_size = power_to_size(current->size);

        if (!current->occupied && current->size >= required_k) {
            bool take_current = target == nullptr;
            if (meta->mode == allocator_with_fit_mode::fit_mode::the_best_fit) {
                take_current = target == nullptr || current->size < target->size;
            } else if (meta->mode == allocator_with_fit_mode::fit_mode::the_worst_fit) {
                take_current = target == nullptr || current->size > target->size;
            }

            if (take_current) {
                target = current;
                if (meta->mode == allocator_with_fit_mode::fit_mode::first_fit) {
                    break;
                }
            }
        }

        current_ptr += current_size;
    }

    if (target == nullptr) {
        throw std::bad_alloc();
    }

    while (target->size > required_k) {
        size_t parent_size = power_to_size(target->size);
        size_t half_size = parent_size / 2;
        auto child_k = static_cast<unsigned char>(target->size - 1);

        target->occupied = false;
        target->size = child_k;

        auto *buddy_ptr = reinterpret_cast<std::byte*>(target) + half_size;
        auto *buddy = new (buddy_ptr) block_metadata();
        buddy->occupied = false;
        buddy->size = child_k;
    }

    auto *occupied_block = reinterpret_cast<occupied_block_metadata*>(target);
    occupied_block->meta.occupied = true;
    occupied_block->meta.size = required_k;
    occupied_block->owner = _trusted_memory;

    return static_cast<std::byte*>(static_cast<void*>(occupied_block)) + occupied_block_metadata_size;
}

void allocator_buddies_system::do_deallocate_sm(void *at)
{
    if (at == nullptr) return;
    if (_trusted_memory == nullptr) {
        throw std::logic_error("Cannot deallocate");
    }

    auto *meta = reinterpret_cast<allocator_meta*>(_trusted_memory);
    std::lock_guard<std::mutex> lock(meta->mtx);

    auto it = begin();
    auto end_it = end();
    block_metadata *target = nullptr;

    for (; it != end_it; ++it) {
        if (it.occupied() && *it == at) {
            target = reinterpret_cast<block_metadata*>(it._block);
            break;
        }
    }

    if (target == nullptr) {
        throw std::logic_error("Pointer was not allocated by this allocator");
    }

    auto *occupied_block = reinterpret_cast<occupied_block_metadata*>(target);
    if (occupied_block->owner != _trusted_memory) {
        throw std::logic_error("Pointer does not belong to this allocator");
    }

    occupied_block->meta.occupied = false;

    void *pool_start_void = static_cast<std::byte*>(_trusted_memory) + allocator_metadata_size;
    size_t available = power_to_size(meta->pool_size_power) + block_alignment();
    if (!align_pointer(pool_start_void, available, block_alignment(), occupied_block_metadata_size)) {
        throw std::logic_error("Allocator metadata is corrupted");
    }

    auto *pool_begin = static_cast<std::byte*>(pool_start_void);

    while (target->size < meta->pool_size_power) {
        size_t block_size = power_to_size(target->size);
        size_t offset = static_cast<size_t>(reinterpret_cast<std::byte*>(target) - pool_begin);
        size_t buddy_offset = offset ^ block_size;
        auto *buddy = reinterpret_cast<block_metadata*>(pool_begin + buddy_offset);

        if (buddy->occupied || buddy->size != target->size) {
            break;
        }

        target = reinterpret_cast<block_metadata*>(pool_begin + (offset & ~block_size));
        target->occupied = false;
        ++target->size;
    }
}

bool allocator_buddies_system::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    if (this == &other) return true;

    auto *other_allocator = dynamic_cast<allocator_buddies_system const*>(&other);
    if (other_allocator == nullptr) return false;

    return _trusted_memory == other_allocator->_trusted_memory;
}

inline void allocator_buddies_system::set_fit_mode(
    allocator_with_fit_mode::fit_mode mode)
{
    if (_trusted_memory == nullptr) {
        throw std::logic_error("Cannot change fit mode");
    }

    auto *meta = reinterpret_cast<allocator_meta*>(_trusted_memory);
    std::lock_guard<std::mutex> lock(meta->mtx);
    meta->mode = mode;
}

std::vector<allocator_test_utils::block_info> allocator_buddies_system::get_blocks_info() const noexcept
{
    if (_trusted_memory == nullptr) return {};

    auto *meta = reinterpret_cast<allocator_meta*>(_trusted_memory);
    std::lock_guard<std::mutex> lock(meta->mtx);
    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info> allocator_buddies_system::get_blocks_info_inner() const
{
    std::vector<allocator_test_utils::block_info> result;

    for (auto it = begin(); it != end(); ++it) {
        allocator_test_utils::block_info info;
        info.block_size = it.size();
        info.is_block_occupied = it.occupied();
        result.push_back(info);
    }

    return result;
}

allocator_buddies_system::buddy_iterator allocator_buddies_system::begin() const noexcept
{
    return buddy_iterator(_trusted_memory);
}

allocator_buddies_system::buddy_iterator allocator_buddies_system::end() const noexcept
{
    return buddy_iterator();
}

bool allocator_buddies_system::buddy_iterator::operator==(const allocator_buddies_system::buddy_iterator &other) const noexcept
{
    return _block == other._block && _trusted_memory == other._trusted_memory;
}

bool allocator_buddies_system::buddy_iterator::operator!=(const allocator_buddies_system::buddy_iterator &other) const noexcept
{
    return !(*this == other);
}

allocator_buddies_system::buddy_iterator &allocator_buddies_system::buddy_iterator::operator++() & noexcept
{
    if (_block == nullptr || _trusted_memory == nullptr) return *this;

    auto *meta = reinterpret_cast<allocator_meta*>(_trusted_memory);
    auto *block = reinterpret_cast<block_metadata*>(_block);
    auto *next = static_cast<std::byte*>(_block) + power_to_size(block->size);

    void *pool_start_void = static_cast<std::byte*>(_trusted_memory) + allocator_metadata_size;
    size_t available = power_to_size(meta->pool_size_power) + block_alignment();
    if (!align_pointer(pool_start_void, available, block_alignment(), occupied_block_metadata_size)) {
        _block = nullptr;
        _trusted_memory = nullptr;
        return *this;
    }

    auto *pool_end = static_cast<std::byte*>(pool_start_void) + power_to_size(meta->pool_size_power);
    if (next >= pool_end) {
        _block = nullptr;
        _trusted_memory = nullptr;
    } else {
        _block = next;
    }

    return *this;
}

allocator_buddies_system::buddy_iterator allocator_buddies_system::buddy_iterator::operator++(int)
{
    auto tmp = *this;
    ++(*this);
    return tmp;
}

size_t allocator_buddies_system::buddy_iterator::size() const noexcept
{
    if (_block == nullptr) return 0;
    return power_to_size(reinterpret_cast<block_metadata*>(_block)->size);
}

bool allocator_buddies_system::buddy_iterator::occupied() const noexcept
{
    return _block != nullptr && reinterpret_cast<block_metadata*>(_block)->occupied;
}

void *allocator_buddies_system::buddy_iterator::operator*() const noexcept
{
    if (_block == nullptr) return nullptr;
    if (!occupied()) return _block;

    return static_cast<std::byte*>(_block) + occupied_block_metadata_size;
}

allocator_buddies_system::buddy_iterator::buddy_iterator(void *start)
    : _block(nullptr), _trusted_memory(start)
{
    if (start == nullptr) return;

    auto *meta = reinterpret_cast<allocator_meta*>(start);
    void *pool_start_void = static_cast<std::byte*>(start) + allocator_metadata_size;
    size_t available = power_to_size(meta->pool_size_power) + block_alignment();
    if (!align_pointer(pool_start_void, available, block_alignment(), occupied_block_metadata_size) ||
        available < power_to_size(meta->pool_size_power)) {
        _trusted_memory = nullptr;
        return;
    }

    _block = pool_start_void;
}

allocator_buddies_system::buddy_iterator::buddy_iterator()
    : _block(nullptr), _trusted_memory(nullptr)
{
}
