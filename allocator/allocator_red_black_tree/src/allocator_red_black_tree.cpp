#include "../include/allocator_red_black_tree.h"

#include <algorithm>
#include <cstddef>
#include <new>
#include <stdexcept>

namespace
{
    constexpr size_t block_alignment() noexcept
    {
        return alignof(std::max_align_t);
    }

    size_t align_up(size_t value, size_t alignment = block_alignment()) noexcept
    {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    bool align_pointer(void *&ptr, size_t &available_space, size_t alignment, size_t size) noexcept
    {
        return std::align(alignment, size, ptr, available_space) != nullptr;
    }
}

void *allocator_red_black_tree::begin_of_pool() const noexcept
{
    if (_trusted_memory == nullptr) return nullptr;

    auto *meta = reinterpret_cast<allocator_meta*>(_trusted_memory);
    void *begin = static_cast<std::byte*>(_trusted_memory) + allocator_metadata_size;
    size_t available = meta->allocated_size - allocator_metadata_size;

    if (!align_pointer(begin, available, alignof(block_metadata), occupied_block_metadata_size)) {
        return nullptr;
    }

    return begin;
}

void *allocator_red_black_tree::end_of_pool() const noexcept
{
    if (_trusted_memory == nullptr) return nullptr;

    auto *meta = reinterpret_cast<allocator_meta*>(_trusted_memory);
    return static_cast<std::byte*>(begin_of_pool()) + meta->total_size;
}

allocator_red_black_tree::block_color allocator_red_black_tree::color_of(block_metadata *node) noexcept
{
    return node == nullptr ? block_color::BLACK : node->color;
}

void allocator_red_black_tree::set_color(block_metadata *node, block_color color) noexcept
{
    if (node != nullptr) node->color = color;
}

bool allocator_red_black_tree::less_by_address(block_metadata *lhs, block_metadata *rhs) noexcept
{
    return reinterpret_cast<std::byte*>(lhs) < reinterpret_cast<std::byte*>(rhs);
}

void allocator_red_black_tree::rotate_left(block_metadata *node) noexcept
{
    auto *meta = reinterpret_cast<allocator_meta*>(_trusted_memory);
    auto *right = node->right;

    node->right = right->left;
    if (right->left != nullptr) right->left->parent = node;

    right->parent = node->parent;
    if (node->parent == nullptr) {
        meta->free_root = right;
    } else if (node == node->parent->left) {
        node->parent->left = right;
    } else {
        node->parent->right = right;
    }

    right->left = node;
    node->parent = right;
}

void allocator_red_black_tree::rotate_right(block_metadata *node) noexcept
{
    auto *meta = reinterpret_cast<allocator_meta*>(_trusted_memory);
    auto *left = node->left;

    node->left = left->right;
    if (left->right != nullptr) left->right->parent = node;

    left->parent = node->parent;
    if (node->parent == nullptr) {
        meta->free_root = left;
    } else if (node == node->parent->right) {
        node->parent->right = left;
    } else {
        node->parent->left = left;
    }

    left->right = node;
    node->parent = left;
}

void allocator_red_black_tree::insert_fixup(block_metadata *node) noexcept
{
    auto *meta = reinterpret_cast<allocator_meta*>(_trusted_memory);

    while (node->parent != nullptr && node->parent->color == block_color::RED) {
        auto *parent = node->parent;
        auto *grandparent = parent->parent;

        if (parent == grandparent->left) {
            auto *uncle = grandparent->right;
            if (color_of(uncle) == block_color::RED) {
                parent->color = block_color::BLACK;
                uncle->color = block_color::BLACK;
                grandparent->color = block_color::RED;
                node = grandparent;
            } else {
                if (node == parent->right) {
                    node = parent;
                    rotate_left(node);
                    parent = node->parent;
                    grandparent = parent->parent;
                }
                parent->color = block_color::BLACK;
                grandparent->color = block_color::RED;
                rotate_right(grandparent);
            }
        } else {
            auto *uncle = grandparent->left;
            if (color_of(uncle) == block_color::RED) {
                parent->color = block_color::BLACK;
                uncle->color = block_color::BLACK;
                grandparent->color = block_color::RED;
                node = grandparent;
            } else {
                if (node == parent->left) {
                    node = parent;
                    rotate_right(node);
                    parent = node->parent;
                    grandparent = parent->parent;
                }
                parent->color = block_color::BLACK;
                grandparent->color = block_color::RED;
                rotate_left(grandparent);
            }
        }
    }

    static_cast<block_metadata*>(meta->free_root)->color = block_color::BLACK;
}

void allocator_red_black_tree::insert_free_block(block_metadata *block) noexcept
{
    auto *meta = reinterpret_cast<allocator_meta*>(_trusted_memory);

    block->occupied = false;
    block->color = block_color::RED;
    block->left = nullptr;
    block->right = nullptr;
    block->parent = nullptr;

    block_metadata *parent = nullptr;
    auto *current = static_cast<block_metadata*>(meta->free_root);

    while (current != nullptr) {
        parent = current;
        current = less_by_address(block, current) ? current->left : current->right;
    }

    block->parent = parent;
    if (parent == nullptr) {
        meta->free_root = block;
    } else if (less_by_address(block, parent)) {
        parent->left = block;
    } else {
        parent->right = block;
    }

    insert_fixup(block);
}

allocator_red_black_tree::block_metadata *allocator_red_black_tree::minimum(block_metadata *node) noexcept
{
    while (node != nullptr && node->left != nullptr) node = node->left;
    return node;
}

void allocator_red_black_tree::transplant(block_metadata *from, block_metadata *to) noexcept
{
    auto *meta = reinterpret_cast<allocator_meta*>(_trusted_memory);

    if (from->parent == nullptr) {
        meta->free_root = to;
    } else if (from == from->parent->left) {
        from->parent->left = to;
    } else {
        from->parent->right = to;
    }

    if (to != nullptr) to->parent = from->parent;
}

void allocator_red_black_tree::delete_fixup(block_metadata *node, block_metadata *parent) noexcept
{
    auto *meta = reinterpret_cast<allocator_meta*>(_trusted_memory);

    while (node != meta->free_root && color_of(node) == block_color::BLACK) {
        if (parent == nullptr) break;

        if (node == parent->left) {
            auto *sibling = parent->right;

            if (color_of(sibling) == block_color::RED) {
                sibling->color = block_color::BLACK;
                parent->color = block_color::RED;
                rotate_left(parent);
                sibling = parent->right;
            }

            if (color_of(sibling ? sibling->left : nullptr) == block_color::BLACK &&
                color_of(sibling ? sibling->right : nullptr) == block_color::BLACK) {
                set_color(sibling, block_color::RED);
                node = parent;
                parent = node->parent;
            } else {
                if (color_of(sibling ? sibling->right : nullptr) == block_color::BLACK) {
                    set_color(sibling ? sibling->left : nullptr, block_color::BLACK);
                    set_color(sibling, block_color::RED);
                    rotate_right(sibling);
                    sibling = parent->right;
                }

                set_color(sibling, parent->color);
                parent->color = block_color::BLACK;
                set_color(sibling ? sibling->right : nullptr, block_color::BLACK);
                rotate_left(parent);
                node = static_cast<block_metadata*>(meta->free_root);
                parent = nullptr;
            }
        } else {
            auto *sibling = parent->left;

            if (color_of(sibling) == block_color::RED) {
                sibling->color = block_color::BLACK;
                parent->color = block_color::RED;
                rotate_right(parent);
                sibling = parent->left;
            }

            if (color_of(sibling ? sibling->right : nullptr) == block_color::BLACK &&
                color_of(sibling ? sibling->left : nullptr) == block_color::BLACK) {
                set_color(sibling, block_color::RED);
                node = parent;
                parent = node->parent;
            } else {
                if (color_of(sibling ? sibling->left : nullptr) == block_color::BLACK) {
                    set_color(sibling ? sibling->right : nullptr, block_color::BLACK);
                    set_color(sibling, block_color::RED);
                    rotate_left(sibling);
                    sibling = parent->left;
                }

                set_color(sibling, parent->color);
                parent->color = block_color::BLACK;
                set_color(sibling ? sibling->left : nullptr, block_color::BLACK);
                rotate_right(parent);
                node = static_cast<block_metadata*>(meta->free_root);
                parent = nullptr;
            }
        }
    }

    set_color(node, block_color::BLACK);
}

void allocator_red_black_tree::remove_free_block(block_metadata *block) noexcept
{
    block_metadata *moved = block;
    block_color removed_color = moved->color;
    block_metadata *fix_node = nullptr;
    block_metadata *fix_parent = nullptr;

    if (block->left == nullptr) {
        fix_node = block->right;
        fix_parent = block->parent;
        transplant(block, block->right);
    } else if (block->right == nullptr) {
        fix_node = block->left;
        fix_parent = block->parent;
        transplant(block, block->left);
    } else {
        moved = minimum(block->right);
        removed_color = moved->color;
        fix_node = moved->right;

        if (moved->parent == block) {
            fix_parent = moved;
            if (fix_node != nullptr) fix_node->parent = moved;
        } else {
            fix_parent = moved->parent;
            transplant(moved, moved->right);
            moved->right = block->right;
            moved->right->parent = moved;
        }

        transplant(block, moved);
        moved->left = block->left;
        moved->left->parent = moved;
        moved->color = block->color;
    }

    block->parent = nullptr;
    block->left = nullptr;
    block->right = nullptr;

    if (removed_color == block_color::BLACK) {
        delete_fixup(fix_node, fix_parent);
    }
}

void allocator_red_black_tree::walk_free_tree(block_metadata *node, block_callback callback) const
{
    if (node == nullptr) return;
    walk_free_tree(node->left, callback);
    callback(node);
    walk_free_tree(node->right, callback);
}

allocator_red_black_tree::block_metadata *allocator_red_black_tree::find_suitable_block(size_t size) const
{
    auto *meta = reinterpret_cast<allocator_meta*>(_trusted_memory);
    block_metadata *target = nullptr;

    walk_free_tree(static_cast<block_metadata*>(meta->free_root), [&](block_metadata *block) {
        if (block->size < size) return;

        if (meta->mode == allocator_with_fit_mode::fit_mode::first_fit) {
            if (target == nullptr) target = block;
        } else if (meta->mode == allocator_with_fit_mode::fit_mode::the_best_fit) {
            if (target == nullptr || block->size < target->size ||
                (block->size == target->size && less_by_address(block, target))) {
                target = block;
            }
        } else {
            if (target == nullptr || block->size > target->size ||
                (block->size == target->size && less_by_address(block, target))) {
                target = block;
            }
        }
    });

    return target;
}

allocator_red_black_tree::block_metadata *allocator_red_black_tree::next_physical(block_metadata *block) const noexcept
{
    auto *next = reinterpret_cast<std::byte*>(block) + occupied_block_metadata_size + block->size;
    return next >= static_cast<std::byte*>(end_of_pool()) ? nullptr : reinterpret_cast<block_metadata*>(next);
}

allocator_red_black_tree::block_metadata *allocator_red_black_tree::previous_physical(block_metadata *block) const noexcept
{
    auto *begin = static_cast<std::byte*>(begin_of_pool());
    if (reinterpret_cast<std::byte*>(block) == begin) return nullptr;

    auto *current = reinterpret_cast<block_metadata*>(begin);
    block_metadata *previous = nullptr;

    while (current != nullptr && current != block) {
        previous = current;
        current = next_physical(current);
    }

    return current == block ? previous : nullptr;
}

bool allocator_red_black_tree::owns_block(block_metadata *block) const noexcept
{
    auto *begin = static_cast<std::byte*>(begin_of_pool());
    auto *end = static_cast<std::byte*>(end_of_pool());
    auto *address = reinterpret_cast<std::byte*>(block);

    if (address < begin || address + occupied_block_metadata_size > end) return false;

    for (auto *current = reinterpret_cast<block_metadata*>(begin);
         current != nullptr;
         current = next_physical(current)) {
        if (current == block && current->owner == _trusted_memory) return true;
    }

    return false;
}

void allocator_red_black_tree::release_trusted_memory() noexcept
{
    if (_trusted_memory == nullptr) return;

    auto *meta = reinterpret_cast<allocator_meta*>(_trusted_memory);
    auto *parent = meta->parent_allocator;
    size_t allocated_size = meta->allocated_size;

    meta->~allocator_meta();
    parent->deallocate(_trusted_memory, allocated_size);
    _trusted_memory = nullptr;
}

allocator_red_black_tree::~allocator_red_black_tree()
{
    release_trusted_memory();
}

allocator_red_black_tree::allocator_red_black_tree(allocator_red_black_tree &&other) noexcept
    : _trusted_memory(nullptr)
{
    if (other._trusted_memory == nullptr) return;

    auto *meta = reinterpret_cast<allocator_meta*>(other._trusted_memory);
    std::lock_guard<std::mutex> lock(meta->mtx);

    _trusted_memory = other._trusted_memory;
    other._trusted_memory = nullptr;
}

allocator_red_black_tree &allocator_red_black_tree::operator=(allocator_red_black_tree &&other) noexcept
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

allocator_red_black_tree::allocator_red_black_tree(
        size_t space_size,
        std::pmr::memory_resource *parent_allocator,
        allocator_with_fit_mode::fit_mode allocate_fit_mode)
    : _trusted_memory(nullptr)
{
    if (parent_allocator == nullptr) parent_allocator = std::pmr::get_default_resource();

    const size_t allocated_size = allocator_metadata_size + space_size + block_alignment() + 4 * free_block_metadata_size;
    _trusted_memory = parent_allocator->allocate(allocated_size);

    try {
        auto *meta = new (_trusted_memory) allocator_meta();
        meta->parent_allocator = parent_allocator;
        meta->mode = allocate_fit_mode;
        meta->allocated_size = allocated_size;
        meta->free_root = nullptr;

        void *pool_start = static_cast<std::byte*>(_trusted_memory) + allocator_metadata_size;
        size_t available = allocated_size - allocator_metadata_size;
        if (!align_pointer(pool_start, available, alignof(block_metadata), occupied_block_metadata_size) ||
            available <= occupied_block_metadata_size) {
            meta->~allocator_meta();
            parent_allocator->deallocate(_trusted_memory, allocated_size);
            _trusted_memory = nullptr;
            throw std::bad_alloc();
        }

        meta->total_size = available;

        auto *first_block = new (pool_start) block_metadata();
        first_block->size = available - occupied_block_metadata_size;
        first_block->occupied = false;
        first_block->parent = nullptr;
        first_block->left = nullptr;
        first_block->right = nullptr;
        first_block->color = block_color::BLACK;
        meta->free_root = first_block;
    } catch (...) {
        if (_trusted_memory != nullptr) {
            parent_allocator->deallocate(_trusted_memory, allocated_size);
            _trusted_memory = nullptr;
        }
        throw;
    }
}

bool allocator_red_black_tree::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    if (this == &other) return true;

    auto *other_allocator = dynamic_cast<const allocator_red_black_tree*>(&other);
    return other_allocator != nullptr && _trusted_memory == other_allocator->_trusted_memory;
}

[[nodiscard]] void *allocator_red_black_tree::do_allocate_sm(size_t size)
{
    if (_trusted_memory == nullptr) {
        throw std::logic_error("Cannot allocate with moved allocator");
    }

    auto *meta = reinterpret_cast<allocator_meta*>(_trusted_memory);
    std::lock_guard<std::mutex> lock(meta->mtx);

    const size_t requested = align_up(size == 0 ? 1 : size);
    auto *target = find_suitable_block(requested);
    if (target == nullptr) throw std::bad_alloc();

    remove_free_block(target);

    if (target->size >= requested + occupied_block_metadata_size + block_alignment()) {
        auto *remainder = reinterpret_cast<block_metadata*>(
                reinterpret_cast<std::byte*>(target) + occupied_block_metadata_size + requested);

        remainder = new (remainder) block_metadata();
        remainder->size = target->size - requested - occupied_block_metadata_size;
        target->size = requested;
        insert_free_block(remainder);
    }

    target->occupied = true;
    target->owner = _trusted_memory;
    target->color = block_color::BLACK;
    target->parent = nullptr;
    target->left = nullptr;
    target->right = nullptr;

    return reinterpret_cast<std::byte*>(target) + occupied_block_metadata_size;
}

void allocator_red_black_tree::do_deallocate_sm(void *at)
{
    if (at == nullptr) return;
    if (_trusted_memory == nullptr) {
        throw std::logic_error("Cannot deallocate with moved allocator");
    }

    auto *meta = reinterpret_cast<allocator_meta*>(_trusted_memory);
    std::lock_guard<std::mutex> lock(meta->mtx);

    auto *block = reinterpret_cast<block_metadata*>(
            static_cast<std::byte*>(at) - occupied_block_metadata_size);

    if (!owns_block(block)) {
        throw std::logic_error("Pointer does not belong to this allocator");
    }

    if (!block->occupied) {
        throw std::logic_error("Double free detected");
    }

    block->occupied = false;

    auto *previous = previous_physical(block);
    if (previous != nullptr && !previous->occupied) {
        remove_free_block(previous);
        previous->size += occupied_block_metadata_size + block->size;
        block = previous;
    }

    auto *next = next_physical(block);
    if (next != nullptr && !next->occupied) {
        remove_free_block(next);
        block->size += occupied_block_metadata_size + next->size;
    }

    insert_free_block(block);
}

void allocator_red_black_tree::set_fit_mode(allocator_with_fit_mode::fit_mode mode)
{
    if (_trusted_memory == nullptr) {
        throw std::logic_error("Cannot change fit mode of moved allocator");
    }

    auto *meta = reinterpret_cast<allocator_meta*>(_trusted_memory);
    std::lock_guard<std::mutex> lock(meta->mtx);
    meta->mode = mode;
}

std::vector<allocator_test_utils::block_info> allocator_red_black_tree::get_blocks_info() const
{
    if (_trusted_memory == nullptr) return {};

    auto *meta = reinterpret_cast<allocator_meta*>(_trusted_memory);
    std::lock_guard<std::mutex> lock(meta->mtx);
    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info> allocator_red_black_tree::get_blocks_info_inner() const
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

allocator_red_black_tree::rb_iterator allocator_red_black_tree::begin() const noexcept
{
    return rb_iterator(_trusted_memory);
}

allocator_red_black_tree::rb_iterator allocator_red_black_tree::end() const noexcept
{
    return rb_iterator();
}

bool allocator_red_black_tree::rb_iterator::operator==(const allocator_red_black_tree::rb_iterator &other) const noexcept
{
    return _block_ptr == other._block_ptr && _trusted == other._trusted;
}

bool allocator_red_black_tree::rb_iterator::operator!=(const allocator_red_black_tree::rb_iterator &other) const noexcept
{
    return !(*this == other);
}

allocator_red_black_tree::rb_iterator &allocator_red_black_tree::rb_iterator::operator++() & noexcept
{
    if (_block_ptr == nullptr || _trusted == nullptr) return *this;

    auto *meta = reinterpret_cast<allocator_meta*>(_trusted);
    auto *block = reinterpret_cast<block_metadata*>(_block_ptr);
    auto *next = static_cast<std::byte*>(_block_ptr) + occupied_block_metadata_size + block->size;

    void *begin = static_cast<std::byte*>(_trusted) + allocator_metadata_size;
    size_t available = meta->allocated_size - allocator_metadata_size;
    align_pointer(begin, available, alignof(block_metadata), occupied_block_metadata_size);
    auto *end = static_cast<std::byte*>(begin) + meta->total_size;

    if (next >= end) {
        _block_ptr = nullptr;
        _trusted = nullptr;
    } else {
        _block_ptr = next;
    }

    return *this;
}

allocator_red_black_tree::rb_iterator allocator_red_black_tree::rb_iterator::operator++(int)
{
    auto tmp = *this;
    ++(*this);
    return tmp;
}

size_t allocator_red_black_tree::rb_iterator::size() const noexcept
{
    if (_block_ptr == nullptr) return 0;
    return reinterpret_cast<block_metadata*>(_block_ptr)->size;
}

void *allocator_red_black_tree::rb_iterator::operator*() const noexcept
{
    if (_block_ptr == nullptr) return nullptr;
    return static_cast<std::byte*>(_block_ptr) + occupied_block_metadata_size;
}

allocator_red_black_tree::rb_iterator::rb_iterator()
    : _block_ptr(nullptr), _trusted(nullptr)
{
}

allocator_red_black_tree::rb_iterator::rb_iterator(void *trusted)
    : _block_ptr(nullptr), _trusted(trusted)
{
    if (trusted == nullptr) {
        _trusted = nullptr;
        return;
    }

    auto *meta = reinterpret_cast<allocator_meta*>(trusted);
    void *begin = static_cast<std::byte*>(trusted) + allocator_metadata_size;
    size_t available = meta->allocated_size - allocator_metadata_size;
    if (!align_pointer(begin, available, alignof(block_metadata), occupied_block_metadata_size)) {
        _trusted = nullptr;
        return;
    }

    _block_ptr = begin;
}

bool allocator_red_black_tree::rb_iterator::occupied() const noexcept
{
    return _block_ptr != nullptr && reinterpret_cast<block_metadata*>(_block_ptr)->occupied;
}
