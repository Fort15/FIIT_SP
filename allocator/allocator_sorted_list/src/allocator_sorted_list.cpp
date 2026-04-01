#include <not_implemented.h>
#include "../include/allocator_sorted_list.h"

allocator_sorted_list::~allocator_sorted_list()
{
    auto *meta = reinterpret_cast<allocator_meta*>(_trusted_memory);
    auto *parent = meta->parent_allocator;
    size_t total_size = meta->total_size + allocator_metadata_size;

    meta->~allocator_meta();
    parent->deallocate(_trusted_memory, total_size);
}

allocator_sorted_list::allocator_sorted_list(
    allocator_sorted_list &&other) noexcept
{
    _trusted_memory = other._trusted_memory;
    other._trusted_memory = nullptr;
}

allocator_sorted_list &allocator_sorted_list::operator=(
    allocator_sorted_list &&other) noexcept
{
    if (this != &other) {
        if (_trusted_memory != nullptr) this->~allocator_sorted_list();
        _trusted_memory = other._trusted_memory;
        other._trusted_memory = nullptr;
    }
    return *this;
}

allocator_sorted_list::allocator_sorted_list(
        size_t space_size,
        std::pmr::memory_resource *parent_allocator,
        allocator_with_fit_mode::fit_mode allocate_fit_mode)
{
    if (parent_allocator == nullptr) parent_allocator = std::pmr::get_default_resource();

    _trusted_memory = parent_allocator->allocate(space_size + allocator_metadata_size);

    auto *meta = new (_trusted_memory) allocator_meta();
    meta->parent_allocator = parent_allocator;
    meta->mode = allocate_fit_mode;
    meta->total_size = space_size;

    void *first_block_adr = static_cast<std::byte*>(_trusted_memory) + allocator_metadata_size;

    auto *first_block = new (first_block_adr) block_metadata();
    first_block->size = space_size - block_metadata_size;
    first_block->next_free_block = nullptr;

    meta->first_free_block = first_block_adr;
}

[[nodiscard]] void *allocator_sorted_list::do_allocate_sm(
    size_t size)
{
    auto *meta = reinterpret_cast<allocator_meta*>(_trusted_memory);
    std::lock_guard<std::mutex> lock(meta->mtx);

    block_metadata *prev = nullptr;
    block_metadata *cur = reinterpret_cast<block_metadata*>(meta->first_free_block);

    block_metadata *target_prev = nullptr, *target_cur = nullptr;

    while (cur != nullptr) {
        if (cur->size >= size) {
            if (meta->mode == allocator_with_fit_mode::fit_mode::first_fit) {
                target_cur = cur;
                target_prev = prev;
                break;
            }
            else if (meta->mode == allocator_with_fit_mode::fit_mode::the_best_fit) {
                if (!target_cur || cur->size < target_cur->size) {
                    target_cur = cur;
                    target_prev = prev;
                }
            }
            else {
                if (!target_cur || cur->size > target_cur->size) {
                    target_cur = cur;
                    target_prev = prev;
                }
            }
        }
        prev = cur;
        cur = reinterpret_cast<block_metadata*>(cur->next_free_block);
    }

    if (!target_cur) throw std::bad_alloc();

    if (target_cur->size >= size + block_metadata_size + 1) {
        std::byte *new_block_adr = reinterpret_cast<std::byte*>(target_cur) + size + block_metadata_size;

        auto *next_free = new (new_block_adr) block_metadata();
        next_free->size = target_cur->size - size - block_metadata_size;
        next_free->next_free_block = target_cur->next_free_block;

        if (target_prev) target_prev->next_free_block = next_free;
        else meta->first_free_block = next_free;

        target_cur->size = size;
    } else {
        if (target_prev) target_prev->next_free_block = target_cur->next_free_block;
        else meta->first_free_block = target_cur->next_free_block;
    }

    target_cur->next_free_block = nullptr;
    
    return reinterpret_cast<std::byte*>(target_cur) + block_metadata_size;
}

allocator_sorted_list::allocator_sorted_list(const allocator_sorted_list &other)
{}

allocator_sorted_list &allocator_sorted_list::operator=(const allocator_sorted_list &other)
{}

bool allocator_sorted_list::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    if (this == &other) return true;

    auto *other_adr = dynamic_cast<const allocator_sorted_list*>(&other);
    if (!other_adr) return false;

    return this->_trusted_memory == other_adr->_trusted_memory;
}

void allocator_sorted_list::do_deallocate_sm(
    void *at)
{
    if (at == nullptr) return;

    auto *meta = reinterpret_cast<allocator_meta*>(_trusted_memory);

    std::lock_guard<std::mutex> lock(meta->mtx);

    auto *target = reinterpret_cast<block_metadata*>(static_cast<std::byte*>(at) - block_metadata_size);

    block_metadata *prev = nullptr;
    block_metadata *cur = reinterpret_cast<block_metadata*>(meta->first_free_block);

    while (cur != nullptr && cur < target) {
        prev = cur;
        cur = reinterpret_cast<block_metadata*>(cur->next_free_block);
    }

    if (prev) prev->next_free_block = target;
    else meta->first_free_block = target;
    target->next_free_block = cur;

    if (cur != nullptr) {
        std::byte *end_of_target = reinterpret_cast<std::byte*>(target) + block_metadata_size + target->size;

        if (end_of_target == reinterpret_cast<std::byte*>(cur)) {
            target->size += block_metadata_size + cur->size;
            target->next_free_block = cur->next_free_block;
        }
    }
    if (prev != nullptr) {
        std::byte *end_of_prev = reinterpret_cast<std::byte*>(prev) + block_metadata_size + prev->size;

        if (end_of_prev == reinterpret_cast<std::byte*>(target)) {
            prev->size += block_metadata_size + target->size;
            prev->next_free_block = target->next_free_block;
        }
    }
}

inline void allocator_sorted_list::set_fit_mode(
    allocator_with_fit_mode::fit_mode mode)
{
    auto *meta = reinterpret_cast<allocator_meta*>(_trusted_memory);
    std::lock_guard<std::mutex> lock(meta->mtx);
    meta->mode = mode;
}

std::vector<allocator_test_utils::block_info> allocator_sorted_list::get_blocks_info() const noexcept
{
    auto *meta = reinterpret_cast<allocator_meta*>(_trusted_memory);
    std::lock_guard<std::mutex> lock(meta->mtx);
    return get_blocks_info_inner();
}


std::vector<allocator_test_utils::block_info> allocator_sorted_list::get_blocks_info_inner() const
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

allocator_sorted_list::sorted_free_iterator allocator_sorted_list::free_begin() const noexcept
{
    return sorted_free_iterator(_trusted_memory);
}

allocator_sorted_list::sorted_free_iterator allocator_sorted_list::free_end() const noexcept
{
    return sorted_free_iterator();
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::begin() const noexcept
{
    return sorted_iterator(_trusted_memory);
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::end() const noexcept
{
    return sorted_iterator();
}


bool allocator_sorted_list::sorted_free_iterator::operator==(
        const allocator_sorted_list::sorted_free_iterator & other) const noexcept
{
    return _free_ptr == other._free_ptr;
}

bool allocator_sorted_list::sorted_free_iterator::operator!=(
        const allocator_sorted_list::sorted_free_iterator &other) const noexcept
{
    return _free_ptr != other._free_ptr;
}

allocator_sorted_list::sorted_free_iterator &allocator_sorted_list::sorted_free_iterator::operator++() & noexcept
{
    if (_free_ptr) {
        _free_ptr = reinterpret_cast<block_metadata*>(_free_ptr)->next_free_block;
    }
    return *this;
}

allocator_sorted_list::sorted_free_iterator allocator_sorted_list::sorted_free_iterator::operator++(int n)
{
    auto tmp = *this;
    ++(*this);
    return tmp;
}

size_t allocator_sorted_list::sorted_free_iterator::size() const noexcept
{
    return reinterpret_cast<block_metadata*>(_free_ptr)->size;
}

void *allocator_sorted_list::sorted_free_iterator::operator*() const noexcept
{
    return static_cast<std::byte*>(_free_ptr) + block_metadata_size;
}

allocator_sorted_list::sorted_free_iterator::sorted_free_iterator() : _free_ptr(nullptr) {}

allocator_sorted_list::sorted_free_iterator::sorted_free_iterator(void *trusted)
{
    if (!trusted) _free_ptr = nullptr;
    else {
        auto *meta = reinterpret_cast<allocator_meta*>(trusted);
        _free_ptr = meta->first_free_block;
    }
}

bool allocator_sorted_list::sorted_iterator::operator==(const allocator_sorted_list::sorted_iterator & other) const noexcept
{
    return _current_ptr == other._current_ptr;
}

bool allocator_sorted_list::sorted_iterator::operator!=(const allocator_sorted_list::sorted_iterator &other) const noexcept
{
    return _current_ptr != other._current_ptr;
}

allocator_sorted_list::sorted_iterator &allocator_sorted_list::sorted_iterator::operator++() & noexcept
{
    if (_current_ptr) {
        auto *meta = reinterpret_cast<allocator_meta*>(_trusted_memory);
        auto *block = reinterpret_cast<block_metadata*>(_current_ptr);

        if (_current_ptr == _free_ptr) {
            _free_ptr = block->next_free_block;
        }

        std::byte *next_ptr = static_cast<std::byte*>(_current_ptr) + block_metadata_size + block->size;
        std::byte *end_ptr = static_cast<std::byte*>(_trusted_memory) + allocator_metadata_size + meta->total_size;

        if (next_ptr >= end_ptr) _current_ptr = nullptr;
        else _current_ptr = next_ptr;
    }
    return *this;
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::sorted_iterator::operator++(int n)
{
    auto tmp = *this;
    ++(*this);
    return tmp;
}

size_t allocator_sorted_list::sorted_iterator::size() const noexcept
{
    return reinterpret_cast<block_metadata*>(_current_ptr)->size;
}

void *allocator_sorted_list::sorted_iterator::operator*() const noexcept
{
    return static_cast<std::byte*>(_current_ptr) + block_metadata_size;
}

allocator_sorted_list::sorted_iterator::sorted_iterator()
    : _current_ptr(nullptr), _free_ptr(nullptr), _trusted_memory(nullptr) {}

allocator_sorted_list::sorted_iterator::sorted_iterator(void *trusted)
{
    if (trusted == nullptr) {
        _current_ptr = nullptr;
        _free_ptr = nullptr;
    }else {
        auto *meta = reinterpret_cast<allocator_meta*>(trusted);
        _current_ptr = static_cast<std::byte*>(trusted) + allocator_metadata_size;
        _free_ptr = meta->first_free_block;
        _trusted_memory = trusted;
    }
}

bool allocator_sorted_list::sorted_iterator::occupied() const noexcept
{   
    return _current_ptr != _free_ptr;
}
