#ifndef MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_RED_BLACK_TREE_H
#define MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_RED_BLACK_TREE_H

#include <pp_allocator.h>
#include <allocator_test_utils.h>
#include <allocator_with_fit_mode.h>
#include <mutex>
#include <cstddef>
#include <functional>

class allocator_red_black_tree final:
    public smart_mem_resource,
    public allocator_test_utils,
    public allocator_with_fit_mode
{

private:

    enum class block_color : unsigned char
    { RED, BLACK };

    struct allocator_meta
    {
        std::pmr::memory_resource *parent_allocator;
        allocator_with_fit_mode::fit_mode mode;
        size_t total_size;
        size_t allocated_size;
        std::mutex mtx;
        void *free_root;
    };

    struct alignas(std::max_align_t) block_metadata
    {
        size_t size;
        bool occupied;
        block_color color;
        block_metadata *parent;
        block_metadata *left;
        block_metadata *right;
        void* owner;
    };

    void *_trusted_memory;

    static constexpr const size_t allocator_metadata_size = sizeof(allocator_meta);
    static constexpr const size_t occupied_block_metadata_size = sizeof(block_metadata);
    static constexpr const size_t free_block_metadata_size = sizeof(block_metadata);

public:
    
    ~allocator_red_black_tree() override;
    
    allocator_red_black_tree(
        allocator_red_black_tree const &other) = delete;
    
    allocator_red_black_tree &operator=(
        allocator_red_black_tree const &other) = delete;
    
    allocator_red_black_tree(
        allocator_red_black_tree &&other) noexcept;
    
    allocator_red_black_tree &operator=(
        allocator_red_black_tree &&other) noexcept;

public:
    
    explicit allocator_red_black_tree(
            size_t space_size,
            std::pmr::memory_resource *parent_allocator = nullptr,
            allocator_with_fit_mode::fit_mode allocate_fit_mode = allocator_with_fit_mode::fit_mode::first_fit);

private:
    
    [[nodiscard]] void *do_allocate_sm(
        size_t size) override;
    
    void do_deallocate_sm(
        void *at) override;

    bool do_is_equal(const std::pmr::memory_resource&) const noexcept override;

    std::vector<allocator_test_utils::block_info> get_blocks_info() const override;
    
    inline void set_fit_mode(allocator_with_fit_mode::fit_mode mode) override;

private:

    using block_callback = std::function<void(block_metadata*)>;

    void *begin_of_pool() const noexcept;

    void *end_of_pool() const noexcept;

    static block_color color_of(block_metadata *node) noexcept;

    static void set_color(block_metadata *node, block_color color) noexcept;

    static bool less_by_address(block_metadata *lhs, block_metadata *rhs) noexcept;

    void rotate_left(block_metadata *node) noexcept;

    void rotate_right(block_metadata *node) noexcept;

    void insert_fixup(block_metadata *node) noexcept;

    void insert_free_block(block_metadata *block) noexcept;

    static block_metadata *minimum(block_metadata *node) noexcept;

    void transplant(block_metadata *from, block_metadata *to) noexcept;

    void delete_fixup(block_metadata *node, block_metadata *parent) noexcept;

    void remove_free_block(block_metadata *block) noexcept;

    void walk_free_tree(block_metadata *node, block_callback callback) const;

    block_metadata *find_suitable_block(size_t size) const;

    block_metadata *next_physical(block_metadata *block) const noexcept;

    block_metadata *previous_physical(block_metadata *block) const noexcept;

    bool owns_block(block_metadata *block) const noexcept;

    void release_trusted_memory() noexcept;

    std::vector<allocator_test_utils::block_info> get_blocks_info_inner() const override;

    class rb_iterator
    {
        void* _block_ptr;
        void* _trusted;

    public:

        using iterator_category = std::forward_iterator_tag;
        using value_type = void*;
        using reference = void*&;
        using pointer = void**;
        using difference_type = ptrdiff_t;

        bool operator==(const rb_iterator&) const noexcept;

        bool operator!=(const rb_iterator&) const noexcept;

        rb_iterator& operator++() & noexcept;

        rb_iterator operator++(int n);

        size_t size() const noexcept;

        void* operator*() const noexcept;

        bool occupied()const noexcept;

        rb_iterator();

        rb_iterator(void* trusted);
    };

    friend class rb_iterator;

    rb_iterator begin() const noexcept;
    rb_iterator end() const noexcept;

};

#endif //MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_RED_BLACK_TREE_H
