#ifndef SYS_PROG_B_TREE_H
#define SYS_PROG_B_TREE_H

#include <iterator>
#include <utility>
#include <boost/container/static_vector.hpp>
#include <stack>
#include <pp_allocator.h>
#include <associative_container.h>
#include <not_implemented.h>
#include <initializer_list>
#include <new>
#include <optional>

template <typename tkey, typename tvalue, comparator<tkey> compare = std::less<tkey>, std::size_t t = 5>
class B_tree final : private compare // EBCO
{
public:

    using tree_data_type = std::pair<tkey, tvalue>;
    using tree_data_type_const = std::pair<const tkey, tvalue>;
    using value_type = tree_data_type_const;

private:

    static constexpr const size_t minimum_keys_in_node = t - 1;
    static constexpr const size_t maximum_keys_in_node = 2 * t - 1;

    // region comparators declaration

    inline bool compare_keys(const tkey& lhs, const tkey& rhs) const;
    inline bool compare_pairs(const tree_data_type& lhs, const tree_data_type& rhs) const;

    // endregion comparators declaration


    struct btree_node
    {
        boost::container::static_vector<tree_data_type, maximum_keys_in_node + 1> _keys;
        boost::container::static_vector<btree_node*, maximum_keys_in_node + 2> _pointers;
        btree_node() noexcept = default;

        bool is_terminate() const noexcept {
            return _pointers.empty();
        }
    };

    pp_allocator<value_type> _allocator;
    btree_node* _root;
    size_t _size;

    pp_allocator<value_type> get_allocator() const noexcept;

    std::stack<std::pair<btree_node**, size_t>> get_leftmost_path();
    std::stack<std::pair<btree_node**, size_t>> get_rightmost_path();

    std::stack<std::pair<btree_node* const*, size_t>> get_leftmost_path() const {
        std::stack<std::pair<btree_node* const*, size_t>> path;
        if (!_root) return path;
        
        btree_node* const* cur = &_root;
        path.push({cur, 0});
        
        while (!(*cur)->_pointers.empty()) {
            cur = &((*cur)->_pointers[0]);
            path.push({cur, 0});
        }
        return path;
    }

    std::stack<std::pair<btree_node* const*, size_t>> get_rightmost_path() const {
        std::stack<std::pair<btree_node* const*, size_t>> path;
        if (!_root) return path;
        
        btree_node* const* cur = &_root;
        path.push({cur, 0});
        
        while (!(*cur)->_pointers.empty()) {
            size_t idx = (*cur)->_pointers.size() - 1;
            cur = &((*cur)->_pointers[idx]);
            path.push({cur, idx});
        }
        return path;
    }

private:

    using mutable_path_entry = std::pair<btree_node**, size_t>;
    using const_path_entry = std::pair<btree_node* const*, size_t>;
    using mutable_path_type = std::stack<mutable_path_entry>;
    using const_path_type = std::stack<const_path_entry>;

    template <typename path_type>
    static btree_node* current_node_from_path(const path_type& path) noexcept {
        return path.empty() ? nullptr : *path.top().first;
    }   

    template <typename path_type>
    static void descend_leftmost(path_type& path) {
        auto* node = current_node_from_path(path);
        while (node && !node->_pointers.empty()) {
            path.push(typename path_type::value_type(&node->_pointers[0], 0));
            node = node->_pointers[0];
        }
    }

    template <typename path_type>
    static void descend_rightmost(path_type& path) {
    auto* node = current_node_from_path(path);
        while (node && !node->_pointers.empty()) {
            size_t idx = node->_pointers.size() - 1;
            path.push(typename path_type::value_type(&node->_pointers[idx], idx));
            node = node->_pointers[idx];
        }
    }

    template <typename path_type>
    static void increment_path(path_type& path, size_t& index) {
        if (path.empty()) return;
        auto* node = current_node_from_path(path);
        
        if (!node->_pointers.empty()) {
            path.push(typename path_type::value_type(&node->_pointers[index + 1], index + 1));
            descend_leftmost(path);
            index = 0;
            return;
        }
        
        if (index + 1 < node->_keys.size()) {
            ++index;
            return;
        }
        
        while (!path.empty()) {
            size_t child_idx = path.top().second;
            path.pop();
            if (path.empty()) { index = 0; return; }
            auto* parent = current_node_from_path(path);
            if (child_idx < parent->_keys.size()) {
                index = child_idx;
                return;
            }
        }
        index = 0;
    }

    template <typename path_type>
    static void decrement_path(path_type& path, size_t& index) {
        if (path.empty()) return;
        auto* node = current_node_from_path(path);
        
        if (!node->_pointers.empty()) {
            path.push(typename path_type::value_type(&node->_pointers[index], index));
            descend_rightmost(path);
            node = current_node_from_path(path);
            index = node->_keys.size() - 1;
            return;
        }
        
        if (index > 0) {
            --index;
            return;
        }
        
        while (!path.empty()) {
            size_t child_idx = path.top().second;
            path.pop();
            if (path.empty()) { index = 0; return; }
            if (child_idx > 0) {
                index = child_idx - 1;
                return;
            }
        }
        index = 0;
    }

    pp_allocator<btree_node> node_allocator() const noexcept {
        return pp_allocator<btree_node>(_allocator);
    }

    btree_node* allocate_node() {
        return node_allocator().template new_object<btree_node>();
    }

    void deallocate_node(btree_node* node) noexcept {
        if (node != nullptr) {
            node_allocator().template delete_object<btree_node>(node);
        }
    }

    void destroy_subtree(btree_node* node) noexcept {
        if (!node)  return;

        for (btree_node* child : node->_pointers) {
            destroy_subtree(child);
        }

        deallocate_node(node);
    }


    size_t find_key_index(const btree_node *node, const tkey& key) const {
        size_t left = 0, right = node->_keys.size();
        while (left < right) {
            size_t mid = left + (right - left) / 2;
            if (compare_keys(node->_keys[mid].first, key)) {
                left = mid + 1;
            } else {
                right = mid;
            }
        }

        return left;
    }

    size_t find_key_index_strict(const btree_node* node, const tkey& key) const {
        size_t left = 0, right = node->_keys.size();
        while (left < right) {
            size_t mid = left + (right - left) / 2;
            
            if (!compare_keys(key, node->_keys[mid].first)) {
                left = mid + 1;
            } else {
                right = mid;
            }
        }
    
        return left;
    }

    void split_child(btree_node* parent, size_t child_index) {
        btree_node *full_child = parent->_pointers[child_index];
        btree_node *new_right = allocate_node();

        constexpr size_t median_idx = t;
        tree_data_type median = std::move(full_child->_keys[median_idx]);

        for (size_t i = median_idx + 1; i < full_child->_keys.size(); ++i) {
            new_right->_keys.push_back(std::move(full_child->_keys[i]));
        }

        full_child->_keys.resize(median_idx);

        if (!full_child->_pointers.empty()) { 
            for (size_t i = median_idx + 1; i < full_child->_pointers.size(); ++i) {
                new_right->_pointers.push_back(full_child->_pointers[i]);
            }
            full_child->_pointers.resize(median_idx + 1);
        }

        parent->_pointers.insert(parent->_pointers.begin() + static_cast<ptrdiff_t>(child_index + 1), new_right);
        parent->_keys.insert(parent->_keys.begin() + static_cast<ptrdiff_t>(child_index), std::move(median));
    }

    void insert_into_subtree(btree_node* node, tree_data_type data) {
        size_t i = find_key_index(node, data.first);
        
        if (node->_pointers.empty()) {
            node->_keys.insert(node->_keys.begin() + static_cast<ptrdiff_t>(i), std::move(data));
            return;
        }

        insert_into_subtree(node->_pointers[i], std::move(data));
        
        if (node->_pointers[i]->_keys.size() > maximum_keys_in_node) {
            split_child(node, i);
        }
    }

    btree_node* copy_subtree(const btree_node* src) {
        if (!src) return nullptr;
        btree_node* dst = allocate_node();
        try {
            dst->_keys = src->_keys;
            for (const auto* child : src->_pointers) {
                dst->_pointers.push_back(copy_subtree(child));
            }
        } catch (...) {
            destroy_subtree(dst);
            throw;
        }
        return dst;
    }

    friend void swap(B_tree& lhs, B_tree& rhs) noexcept {
        using std::swap;
        swap(static_cast<compare&>(lhs), static_cast<compare&>(rhs));
        swap(lhs._allocator, rhs._allocator);
        swap(lhs._root, rhs._root);
        swap(lhs._size, rhs._size);
    }


    // pair<tkey, tvalue>& -> pair<const tkey, tvalue>&
    static value_type& as_value(tree_data_type& x) noexcept {
        return *std::launder(reinterpret_cast<value_type*>(&x));
    }

    static const value_type& as_value(const tree_data_type& x) noexcept {
        return *std::launder(reinterpret_cast<const value_type*>(&x));
    }


    template <typename from_stack_type, typename to_stack_type>
    static to_stack_type convert_path_stack(const from_stack_type& source) {
        std::vector<typename from_stack_type::value_type> tmp;
        auto src_copy = source;
        while (!src_copy.empty()) {
            tmp.push_back(src_copy.top());
            src_copy.pop();
        }
        to_stack_type result;
        for (auto it = tmp.rbegin(); it != tmp.rend(); ++it) {
            result.push(typename to_stack_type::value_type(it->first, it->second));
        }
        return result;
    }

public:
    
    class key_not_found : public std::out_of_range {
    public:
        explicit key_not_found(const std::string& msg) 
            : std::out_of_range("B_tree: " + msg) {}
    };

private:


    // helper methods for erase
    tree_data_type& get_min_key(btree_node* node) {
        while (!node->_pointers.empty()) {
            node = node->_pointers.front();
        }
        return node->_keys.front();
    }

    tree_data_type& get_max_key(btree_node* node) {
        while (!node->_pointers.empty()) {
            node = node->_pointers.back();
        }
        return node->_keys.back();
    }

    void borrow_from_left(btree_node* parent, size_t child_index) {
        btree_node* child = parent->_pointers[child_index];
        btree_node* left = parent->_pointers[child_index - 1];
        
        child->_keys.insert(child->_keys.begin(), std::move(parent->_keys[child_index - 1]));
        
        if (!left->_pointers.empty()) {
            child->_pointers.insert(child->_pointers.begin(), left->_pointers.back());
            left->_pointers.pop_back();
        }
        
        parent->_keys[child_index - 1] = std::move(left->_keys.back()); 
        left->_keys.pop_back();
    }

    void borrow_from_right(btree_node* parent, size_t child_index) {
        btree_node* child = parent->_pointers[child_index];
        btree_node* right = parent->_pointers[child_index + 1];
        
        child->_keys.push_back(std::move(parent->_keys[child_index]));
        
        if (!right->_pointers.empty()) {
            child->_pointers.push_back(right->_pointers.front());
            right->_pointers.erase(right->_pointers.begin());
        }
        
        parent->_keys[child_index] = std::move(right->_keys.front());
        right->_keys.erase(right->_keys.begin());
    }

    void merge_children(btree_node* parent, size_t left_child_index) {
        btree_node* left  = parent->_pointers[left_child_index];
        btree_node* right = parent->_pointers[left_child_index + 1];

        left->_keys.push_back(std::move(parent->_keys[left_child_index]));
        for (auto& k : right->_keys) left->_keys.push_back(std::move(k));
        for (auto* p : right->_pointers) left->_pointers.push_back(p);

        parent->_keys.erase(parent->_keys.begin() + static_cast<ptrdiff_t>(left_child_index));
        parent->_pointers.erase(parent->_pointers.begin() + static_cast<ptrdiff_t>(left_child_index + 1));
        
        deallocate_node(right);
    }

    // child have >= t keys
    void ensure_child_has_enough_keys(btree_node* parent, size_t& child_index) {
        btree_node* child = parent->_pointers[child_index];
        if (child->_keys.size() >= t) return;

        if (child_index > 0 && parent->_pointers[child_index - 1]->_keys.size() >= t) {
            borrow_from_left(parent, child_index);
            return;
        }

        if (child_index + 1 < parent->_pointers.size() && parent->_pointers[child_index + 1]->_keys.size() >= t) {
            borrow_from_right(parent, child_index);
            return;
        }

        if (child_index + 1 < parent->_pointers.size()) {
            merge_children(parent, child_index);
        } else {
            merge_children(parent, child_index - 1);
            --child_index;
        }
    }

    bool try_erase_from_node(btree_node* node, const tkey& key) {
        size_t idx = find_key_index(node, key);
        bool found = (idx < node->_keys.size() && 
                      !compare_keys(node->_keys[idx].first, key) && 
                      !compare_keys(key, node->_keys[idx].first));

        if (found) {
            if (node->_pointers.empty()) {
                node->_keys.erase(node->_keys.begin() + static_cast<ptrdiff_t>(idx));
                return true;
            }

            if (node->_pointers[idx]->_keys.size() >= t) {
                node->_keys[idx] = get_max_key(node->_pointers[idx]);
                return try_erase_from_node(node->_pointers[idx], node->_keys[idx].first);
            }
            if (node->_pointers[idx + 1]->_keys.size() >= t) {
                node->_keys[idx] = get_min_key(node->_pointers[idx + 1]);
                return try_erase_from_node(node->_pointers[idx + 1], node->_keys[idx].first);
            }
            merge_children(node, idx);
            return try_erase_from_node(node->_pointers[idx], key);
        }

        if (node->_pointers.empty()) return false;

        ensure_child_has_enough_keys(node, idx);
        return try_erase_from_node(node->_pointers[idx], key);
    }

public:

    // region constructors declaration

    explicit B_tree(const compare& cmp = compare(), pp_allocator<value_type> = pp_allocator<value_type>());

    explicit B_tree(pp_allocator<value_type> alloc, const compare& comp = compare());

    template<input_iterator_for_pair<tkey, tvalue> iterator>
    explicit B_tree(iterator begin, iterator end, const compare& cmp = compare(), pp_allocator<value_type> = pp_allocator<value_type>());

    B_tree(std::initializer_list<std::pair<tkey, tvalue>> data, const compare& cmp = compare(), pp_allocator<value_type> = pp_allocator<value_type>());

    // endregion constructors declaration

    // region five declaration

    B_tree(const B_tree& other);

    B_tree(B_tree&& other) noexcept;

    B_tree& operator=(const B_tree& other);

    B_tree& operator=(B_tree&& other) noexcept;

    ~B_tree() noexcept;

    // endregion five declaration

    // region iterators declaration

    class btree_iterator;
    class btree_reverse_iterator;
    class btree_const_iterator;
    class btree_const_reverse_iterator;

    class btree_iterator final
    {
        std::stack<std::pair<btree_node**, size_t>> _path;
        size_t _index;

    public:
        using value_type = tree_data_type_const;
        using reference = value_type&;
        using pointer = value_type*;
        using iterator_category = std::bidirectional_iterator_tag;
        using difference_type = ptrdiff_t;
        using self = btree_iterator;

        friend class B_tree;
        friend class btree_reverse_iterator;
        friend class btree_const_iterator;
        friend class btree_const_reverse_iterator;

        reference operator*() const noexcept;
        pointer operator->() const noexcept;

        self& operator++();
        self operator++(int);

        self& operator--();
        self operator--(int);

        bool operator==(const self& other) const noexcept;
        bool operator!=(const self& other) const noexcept;

        size_t depth() const noexcept;
        size_t current_node_keys_count() const noexcept;
        bool is_terminate_node() const noexcept;
        size_t index() const noexcept;

        explicit btree_iterator(const std::stack<std::pair<btree_node**, size_t>>& path = std::stack<std::pair<btree_node**, size_t>>(), size_t index = 0);

    };

    class btree_const_iterator final
    {
        std::stack<std::pair<btree_node* const*, size_t>> _path;
        size_t _index;

    public:

        using value_type = tree_data_type_const;
        using reference = const value_type&;
        using pointer = const value_type*;
        using iterator_category = std::bidirectional_iterator_tag;
        using difference_type = ptrdiff_t;
        using self = btree_const_iterator;

        friend class B_tree;
        friend class btree_reverse_iterator;
        friend class btree_iterator;
        friend class btree_const_reverse_iterator;

        btree_const_iterator(const btree_iterator& it) noexcept;

        reference operator*() const noexcept;
        pointer operator->() const noexcept;

        self& operator++();
        self operator++(int);

        self& operator--();
        self operator--(int);

        bool operator==(const self& other) const noexcept;
        bool operator!=(const self& other) const noexcept;

        size_t depth() const noexcept;
        size_t current_node_keys_count() const noexcept;
        bool is_terminate_node() const noexcept;
        size_t index() const noexcept;

        explicit btree_const_iterator(const std::stack<std::pair<btree_node* const*, size_t>>& path = std::stack<std::pair<btree_node* const*, size_t>>(), size_t index = 0);
    };

    class btree_reverse_iterator final
    {
        std::stack<std::pair<btree_node**, size_t>> _path;
        size_t _index;

    public:

        using value_type = tree_data_type_const;
        using reference = value_type&;
        using pointer = value_type*;
        using iterator_category = std::bidirectional_iterator_tag;
        using difference_type = ptrdiff_t;
        using self = btree_reverse_iterator;

        friend class B_tree;
        friend class btree_iterator;
        friend class btree_const_iterator;
        friend class btree_const_reverse_iterator;

        btree_reverse_iterator(const btree_iterator& it) noexcept;
        operator btree_iterator() const noexcept;

        reference operator*() const noexcept;
        pointer operator->() const noexcept;

        self& operator++();
        self operator++(int);

        self& operator--();
        self operator--(int);

        bool operator==(const self& other) const noexcept;
        bool operator!=(const self& other) const noexcept;

        size_t depth() const noexcept;
        size_t current_node_keys_count() const noexcept;
        bool is_terminate_node() const noexcept;
        size_t index() const noexcept;

        explicit btree_reverse_iterator(const std::stack<std::pair<btree_node**, size_t>>& path = std::stack<std::pair<btree_node**, size_t>>(), size_t index = 0);
    };

    class btree_const_reverse_iterator final
    {
        std::stack<std::pair<btree_node* const*, size_t>> _path;
        size_t _index;

    public:

        using value_type = tree_data_type_const;
        using reference = const value_type&;
        using pointer = const value_type*;
        using iterator_category = std::bidirectional_iterator_tag;
        using difference_type = ptrdiff_t;
        using self = btree_const_reverse_iterator;

        friend class B_tree;
        friend class btree_reverse_iterator;
        friend class btree_const_iterator;
        friend class btree_iterator;

        btree_const_reverse_iterator(const btree_reverse_iterator& it) noexcept;
        operator btree_const_iterator() const noexcept;

        reference operator*() const noexcept;
        pointer operator->() const noexcept;

        self& operator++();
        self operator++(int);

        self& operator--();
        self operator--(int);

        bool operator==(const self& other) const noexcept;
        bool operator!=(const self& other) const noexcept;

        size_t depth() const noexcept;
        size_t current_node_keys_count() const noexcept;
        bool is_terminate_node() const noexcept;
        size_t index() const noexcept;

        explicit btree_const_reverse_iterator(const std::stack<std::pair<btree_node* const*, size_t>>& path = std::stack<std::pair<btree_node* const*, size_t>>(), size_t index = 0);
    };

    friend class btree_iterator;
    friend class btree_const_iterator;
    friend class btree_reverse_iterator;
    friend class btree_const_reverse_iterator;

    // endregion iterators declaration

    // region element access declaration

    /*
     * Returns a reference to the mapped value of the element with specified key. If no such element exists, an exception of type std::out_of_range is thrown.
     */
    tvalue& at(const tkey&);
    const tvalue& at(const tkey&) const;

    /*
     * If key not exists, makes default initialization of value
     */
    tvalue& operator[](const tkey& key);
    tvalue& operator[](tkey&& key);

    // endregion element access declaration
    // region iterator begins declaration

    btree_iterator begin();
    btree_iterator end();

    btree_const_iterator begin() const;
    btree_const_iterator end() const;

    btree_const_iterator cbegin() const;
    btree_const_iterator cend() const;

    btree_reverse_iterator rbegin();
    btree_reverse_iterator rend();

    btree_const_reverse_iterator rbegin() const;
    btree_const_reverse_iterator rend() const;

    btree_const_reverse_iterator crbegin() const;
    btree_const_reverse_iterator crend() const;

    // endregion iterator begins declaration

    // region lookup declaration

    size_t size() const noexcept;
    bool empty() const noexcept;

    /*
     * Returns end() if not exist
     */

    btree_iterator find(const tkey& key);
    btree_const_iterator find(const tkey& key) const;

    btree_iterator lower_bound(const tkey& key);
    btree_const_iterator lower_bound(const tkey& key) const;

    btree_iterator upper_bound(const tkey& key);
    btree_const_iterator upper_bound(const tkey& key) const;

    bool contains(const tkey& key) const;

    // endregion lookup declaration

    // region modifiers declaration

    void clear() noexcept;

    /*
     * Does nothing if key exists, delegates to emplace.
     * Second return value is true, when inserted
     */
    std::pair<btree_iterator, bool> insert(const tree_data_type& data);
    std::pair<btree_iterator, bool> insert(tree_data_type&& data);

    template <typename ...Args>
    std::pair<btree_iterator, bool> emplace(Args&&... args);

    /*
     * Updates value if key exists, delegates to emplace.
     */
    btree_iterator insert_or_assign(const tree_data_type& data);
    btree_iterator insert_or_assign(tree_data_type&& data);

    template <typename ...Args>
    btree_iterator emplace_or_assign(Args&&... args);

    /*
     * Return iterator to node next ro removed or end() if key not exists
     */
    btree_iterator erase(btree_iterator pos);
    btree_iterator erase(btree_const_iterator pos);

    btree_iterator erase(btree_iterator beg, btree_iterator en);
    btree_iterator erase(btree_const_iterator beg, btree_const_iterator en);


    btree_iterator erase(const tkey& key);

    // endregion modifiers declaration
};

template<std::input_iterator iterator, comparator<typename std::iterator_traits<iterator>::value_type::first_type> compare = std::less<typename std::iterator_traits<iterator>::value_type::first_type>,
        std::size_t t = 5, typename U>
B_tree(iterator begin, iterator end, const compare &cmp = compare(), pp_allocator<U> = pp_allocator<U>()) -> B_tree<typename std::iterator_traits<iterator>::value_type::first_type, typename std::iterator_traits<iterator>::value_type::second_type, compare, t>;

template<typename tkey, typename tvalue, comparator<tkey> compare = std::less<tkey>, std::size_t t = 5, typename U>
B_tree(std::initializer_list<std::pair<tkey, tvalue>> data, const compare &cmp = compare(), pp_allocator<U> = pp_allocator<U>()) -> B_tree<tkey, tvalue, compare, t>;

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::compare_pairs(const B_tree::tree_data_type &lhs,
                                                     const B_tree::tree_data_type &rhs) const
{
    return compare_keys(lhs.first, rhs.first);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::compare_keys(const tkey &lhs, const tkey &rhs) const
{
    return compare::operator()(lhs, rhs);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
pp_allocator<typename B_tree<tkey, tvalue, compare, t>::value_type> B_tree<tkey, tvalue, compare, t>::get_allocator() const noexcept
{
    return _allocator;
}

// region constructors implementation

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::B_tree(
        const compare& cmp,
        pp_allocator<value_type> alloc) : compare(cmp), _allocator(std::move(alloc)), _root(nullptr), _size(0)
{
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::B_tree(
        pp_allocator<value_type> alloc,
        const compare& comp) : B_tree(comp, std::move(alloc))
{
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
template<input_iterator_for_pair<tkey, tvalue> iterator>
B_tree<tkey, tvalue, compare, t>::B_tree(
        iterator begin,
        iterator end,
        const compare& cmp,
        pp_allocator<value_type> alloc) : compare(cmp), _allocator(alloc), _root(nullptr), _size(0)
{
    try {
        for (; begin != end; ++begin) insert(*begin);
    } catch (...) {
        clear();
        throw;
    }
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::B_tree(
        std::initializer_list<std::pair<tkey, tvalue>> data,
        const compare& cmp,
        pp_allocator<value_type> alloc) : B_tree(data.begin(), data.end(), cmp, alloc)
{
}

// endregion constructors implementation

// region five implementation

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::~B_tree() noexcept
{
    clear();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::B_tree(const B_tree& other) : compare(other), _allocator(other._allocator), _root(nullptr), _size(0)
{
    if (other._root) {
        _root = copy_subtree(other._root);
        _size = other._size;
    }
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>& B_tree<tkey, tvalue, compare, t>::operator=(const B_tree& other)
{
    if (this != &other) {
        B_tree temp(other);
        swap(*this, temp);
    }
    return *this;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::B_tree(B_tree&& other) noexcept 
    : compare(static_cast<const compare&>(other)), _allocator(std::move(other._allocator)), _root(other._root), _size(other._size)
{
    other._root = nullptr;
    other._size = 0;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>& B_tree<tkey, tvalue, compare, t>::operator=(B_tree&& other) noexcept
{
    if (this != &other) {
        clear();
        static_cast<compare&>(*this) = std::move(static_cast<const compare&>(other));
        _allocator = std::move(other._allocator);
        _root = other._root;
        _size = other._size;
        other._root = nullptr;
        other._size = 0;
    }
    return *this;
}

// endregion five implementation

// region iterators implementation

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::btree_iterator::btree_iterator(
        const std::stack<std::pair<btree_node**, size_t>>& path, size_t index) : _path(path), _index(index)
{
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator::reference
B_tree<tkey, tvalue, compare, t>::btree_iterator::operator*() const noexcept
{
    auto& raw = (*_path.top().first)->_keys[_index];
    return B_tree::as_value(raw);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator::pointer
B_tree<tkey, tvalue, compare, t>::btree_iterator::operator->() const noexcept
{
    return std::addressof(B_tree::as_value((*_path.top().first)->_keys[_index]));
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator&
B_tree<tkey, tvalue, compare, t>::btree_iterator::operator++()
{
    B_tree::increment_path(_path, _index);
    return *this;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator
B_tree<tkey, tvalue, compare, t>::btree_iterator::operator++(int)
{
    self tmp = *this;
    ++(*this);
    return tmp; 
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator&
B_tree<tkey, tvalue, compare, t>::btree_iterator::operator--()
{
    B_tree::decrement_path(_path, _index);
    return *this;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator
B_tree<tkey, tvalue, compare, t>::btree_iterator::operator--(int)
{
    self tmp = *this;
    --(*this);
    return tmp;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::btree_iterator::operator==(const self& other) const noexcept
{
    if (_path.empty() && other._path.empty()) return true;
    if (_path.empty() || other._path.empty()) return false;

    return _path.top().first == other._path.top().first && _index == other._index;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::btree_iterator::operator!=(const self& other) const noexcept
{
    return !(*this == other);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::btree_iterator::depth() const noexcept
{
    return _path.empty() ? 0 : (_path.size() - 1);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::btree_iterator::current_node_keys_count() const noexcept
{
    const auto* node = B_tree::current_node_from_path(_path);
    return node ? node->_keys.size() : 0;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::btree_iterator::is_terminate_node() const noexcept
{
    const auto* node = B_tree::current_node_from_path(_path);
    return node && node->is_terminate();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::btree_iterator::index() const noexcept
{
    return _index;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::btree_const_iterator::btree_const_iterator(
        const std::stack<std::pair<btree_node* const*, size_t>>& path, size_t index) : _path(path), _index(index)
{
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::btree_const_iterator::btree_const_iterator(
    const btree_iterator& it) noexcept
    : _path(convert_path_stack<mutable_path_type, const_path_type>(it._path)),
      _index(it._index)
{
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator::reference
B_tree<tkey, tvalue, compare, t>::btree_const_iterator::operator*() const noexcept
{
    const auto& raw = (*_path.top().first)->_keys[_index];
    return B_tree::as_value(raw);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator::pointer
B_tree<tkey, tvalue, compare, t>::btree_const_iterator::operator->() const noexcept
{
    return std::addressof(operator*());
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator&
B_tree<tkey, tvalue, compare, t>::btree_const_iterator::operator++() 
{
    B_tree::increment_path(_path, _index);
    return *this;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator
B_tree<tkey, tvalue, compare, t>::btree_const_iterator::operator++(int)
{
    self tmp = *this;
    ++(*this);
    return tmp;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator&
B_tree<tkey, tvalue, compare, t>::btree_const_iterator::operator--() {
    B_tree::decrement_path(_path, _index);
    return *this;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator
B_tree<tkey, tvalue, compare, t>::btree_const_iterator::operator--(int)
{
    self tmp = *this;
    --(*this);
    return tmp;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::btree_const_iterator::operator==(const self& other) const noexcept
{
    if (_path.empty() && other._path.empty()) return true;
    if (_path.empty() || other._path.empty()) return false;
    return _path.top().first == other._path.top().first && _index == other._index;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::btree_const_iterator::operator!=(const self& other) const noexcept
{
    return !(*this == other);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::btree_const_iterator::depth() const noexcept
{
    return _path.empty() ? 0 : (_path.size() - 1);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::btree_const_iterator::current_node_keys_count() const noexcept
{
    const auto* node = B_tree::current_node_from_path(_path);
    return node ? node->_keys.size() : 0;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::btree_const_iterator::is_terminate_node() const noexcept
{
    const auto* node = B_tree::current_node_from_path(_path);
    return node && node->is_terminate();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::btree_const_iterator::index() const noexcept
{
    return _index;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::btree_reverse_iterator(
        const std::stack<std::pair<btree_node**, size_t>>& path, size_t index) : _path(path), _index(index)
{
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::btree_reverse_iterator(
        const btree_iterator& it) noexcept : _path(it._path), _index(it._index)
{
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::operator B_tree<tkey, tvalue, compare, t>::btree_iterator() const noexcept
{
    return btree_iterator(_path, _index);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::reference
B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::operator*() const noexcept
{
    auto& raw = (*_path.top().first)->_keys[_index];
    return B_tree::as_value(raw);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::pointer
B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::operator->() const noexcept
{
    return std::addressof(operator*());
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator&
B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::operator++()
{
    B_tree::decrement_path(_path, _index);
    return *this;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator
B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::operator++(int)
{
    self tmp = *this;
    ++(*this);
    return tmp;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator&
B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::operator--()
{
    B_tree::increment_path(_path, _index);
    return *this;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator
B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::operator--(int)
{
    self tmp = *this;
    --(*this);
    return tmp;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::operator==(const self& other) const noexcept
{
    if (_path.empty() && other._path.empty()) return true;
    if (_path.empty() || other._path.empty()) return false;
    return _path.top().first == other._path.top().first && _index == other._index;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::operator!=(const self& other) const noexcept
{
    return !(*this == other);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::depth() const noexcept
{
    return _path.empty() ? 0 : (_path.size() - 1);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::current_node_keys_count() const noexcept
{
    const auto* node = B_tree::current_node_from_path(_path);
    return node ? node->_keys.size() : 0;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::is_terminate_node() const noexcept
{
    const auto* node = B_tree::current_node_from_path(_path);
    return node && node->is_terminate();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator::index() const noexcept
{
    return _index;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::btree_const_reverse_iterator(
        const std::stack<std::pair<btree_node* const*, size_t>>& path, size_t index) : _path(path), _index(index)
{
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::btree_const_reverse_iterator(
        const btree_reverse_iterator& it) noexcept : _path(convert_path_stack<mutable_path_type, const_path_type>(it._path)), _index(it._index)
{
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::operator B_tree<tkey, tvalue, compare, t>::btree_const_iterator() const noexcept
{
    return btree_const_iterator(_path, _index);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::reference
B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::operator*() const noexcept
{
    const auto& raw = (*_path.top().first)->_keys[_index];
    return B_tree::as_value(raw);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::pointer
B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::operator->() const noexcept
{
    return std::addressof(operator*());
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator&
B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::operator++()
{
    B_tree::decrement_path(_path, _index);
    return *this;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator
B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::operator++(int)
{
    self tmp = *this;
    ++(*this);
    return tmp;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator&
B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::operator--()
{
    B_tree::increment_path(_path, _index);
    return *this;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator
B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::operator--(int)
{
    self tmp = *this;
    --(*this);
    return tmp;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::operator==(const self& other) const noexcept
{
    if (_path.empty() && other._path.empty()) return true;
    if (_path.empty() || other._path.empty()) return false;
    return _path.top().first == other._path.top().first && _index == other._index;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::operator!=(const self& other) const noexcept
{
    return !(*this == other);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::depth() const noexcept
{
    return _path.empty() ? 0 : (_path.size() - 1);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::current_node_keys_count() const noexcept
{
    const auto* node = B_tree::current_node_from_path(_path);
    return node ? node->_keys.size() : 0;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::is_terminate_node() const noexcept
{
    const auto* node = B_tree::current_node_from_path(_path);
    return node && node->is_terminate();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator::index() const noexcept
{
    return _index;
}

// endregion iterators implementation

// region element access implementation

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
tvalue& B_tree<tkey, tvalue, compare, t>::at(const tkey& key)
{
    auto it = find(key);
    if (it == end()) throw key_not_found("B_tree::at: key not found");
    return it->second;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
const tvalue& B_tree<tkey, tvalue, compare, t>::at(const tkey& key) const
{
    auto it = find(key);
    if (it == end()) throw key_not_found("B_tree::at: key not found");
    return it->second;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
tvalue& B_tree<tkey, tvalue, compare, t>::operator[](const tkey& key)
{
    return emplace(key, tvalue{}).first->second;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
tvalue& B_tree<tkey, tvalue, compare, t>::operator[](tkey&& key)
{
    return emplace(std::move(key), tvalue{}).first->second;
}

// endregion element access implementation

// region iterator begins implementation

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator B_tree<tkey, tvalue, compare, t>::begin()
{
    if (!_root) return btree_iterator();
    return btree_iterator(get_leftmost_path(), 0);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator B_tree<tkey, tvalue, compare, t>::end()
{
    return btree_iterator();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator B_tree<tkey, tvalue, compare, t>::begin() const
{
    return cbegin();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator B_tree<tkey, tvalue, compare, t>::end() const
{
    return cend();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator B_tree<tkey, tvalue, compare, t>::cbegin() const
{
    return btree_const_iterator(get_leftmost_path(), 0);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator B_tree<tkey, tvalue, compare, t>::cend() const
{
    return btree_const_iterator();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator B_tree<tkey, tvalue, compare, t>::rbegin()
{
    if (!_root) return rend();
    auto path = get_rightmost_path();
    size_t idx = current_node_from_path(path)->_keys.size() - 1;
    return btree_reverse_iterator(path, idx);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_reverse_iterator B_tree<tkey, tvalue, compare, t>::rend()
{
    return btree_reverse_iterator();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator B_tree<tkey, tvalue, compare, t>::rbegin() const
{
    if (!_root) return crend();
    auto path = get_rightmost_path();
    size_t idx = current_node_from_path(path)->_keys.size() - 1;
    return btree_const_reverse_iterator(path, idx);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator B_tree<tkey, tvalue, compare, t>::rend() const
{
    return btree_const_reverse_iterator();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator B_tree<tkey, tvalue, compare, t>::crbegin() const
{
    return rbegin();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_reverse_iterator B_tree<tkey, tvalue, compare, t>::crend() const
{
    return rend();
}

// endregion iterator begins implementation

// region lookup implementation

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
size_t B_tree<tkey, tvalue, compare, t>::size() const noexcept
{
    return _size;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::empty() const noexcept
{
    return _size == 0;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator B_tree<tkey, tvalue, compare, t>::find(const tkey& key)
{
    if (!_root) return end();

    btree_node** cur_ptr = &_root;
    size_t child_index = 0;
    std::stack<std::pair<btree_node**, size_t>> path;

    while (*cur_ptr != nullptr) {
        btree_node* node = *cur_ptr;
        path.push({cur_ptr, child_index});

        size_t idx = find_key_index(node, key);
        
        if (idx < node->_keys.size() && 
            !compare_keys(node->_keys[idx].first, key) && 
            !compare_keys(key, node->_keys[idx].first)) {
            return btree_iterator(path, idx);
        }

        if (node->_pointers.empty()) return end();

        child_index = idx;
        cur_ptr = &(node->_pointers[idx]);
    }
    
    return end();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator B_tree<tkey, tvalue, compare, t>::find(const tkey& key) const
{
    if (!_root) return cend();

    btree_node* const* cur_ptr = &_root;
    size_t child_index = 0;
    const_path_type path;

    while (*cur_ptr != nullptr) {
        btree_node* node = *cur_ptr;
        path.push({cur_ptr, child_index});

        size_t idx = find_key_index(node, key);
        
        if (idx < node->_keys.size() && 
            !compare_keys(node->_keys[idx].first, key) && 
            !compare_keys(key, node->_keys[idx].first)) {
            return btree_const_iterator(path, idx);
        }

        if (node->_pointers.empty()) return cend();

        child_index = idx;
        cur_ptr = &(node->_pointers[idx]);
    }
    return cend();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator B_tree<tkey, tvalue, compare, t>::lower_bound(const tkey& key)
{
    if (!_root) return end();

    btree_node** cur_ptr = &_root;
    size_t child_index = 0;
    mutable_path_type path;
    std::optional<std::pair<mutable_path_type, size_t>> candidate;

    while (*cur_ptr != nullptr) {
        btree_node* node = *cur_ptr;
        path.push({cur_ptr, child_index});
        
        size_t idx = find_key_index(node, key);
        
        if (idx < node->_keys.size()) {
            candidate.emplace(path, idx);
        }
        
        if (node->_pointers.empty()) break;
        
        child_index = idx;
        cur_ptr = &(node->_pointers[idx]);
    }
    
    return candidate ? btree_iterator(candidate->first, candidate->second) : end();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator B_tree<tkey, tvalue, compare, t>::lower_bound(const tkey& key) const
{
    if (!_root) return end();

    btree_node* const* cur_ptr = &_root;
    size_t child_index = 0;
    const_path_type path;
    std::optional<std::pair<const_path_type, size_t>> candidate;

    while (*cur_ptr != nullptr) {
        btree_node* node = *cur_ptr;
        path.push({cur_ptr, child_index});
        
        size_t idx = find_key_index(node, key);
        
        if (idx < node->_keys.size()) {
            candidate.emplace(path, idx);
        }
        
        if (node->_pointers.empty()) break;
        
        child_index = idx;
        cur_ptr = &(node->_pointers[idx]);
    }
    
    return candidate ? btree_const_iterator(candidate->first, candidate->second) : cend();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator B_tree<tkey, tvalue, compare, t>::upper_bound(const tkey& key)
{
    if (!_root) return end();

    btree_node** cur_ptr = &_root;
    size_t child_index = 0;
    mutable_path_type path;
    std::optional<std::pair<mutable_path_type, size_t>> candidate;

    while (*cur_ptr != nullptr) {
        btree_node* node = *cur_ptr;
        path.push({cur_ptr, child_index});
        
        size_t idx = find_key_index(node, key);
        
        if (idx < node->_keys.size()) {
            candidate.emplace(path, idx);
        }
        
        if (node->_pointers.empty()) break;
        
        child_index = idx;
        cur_ptr = &(node->_pointers[idx]);
    }
    
    return candidate ? btree_iterator(candidate->first, candidate->second) : end();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_const_iterator B_tree<tkey, tvalue, compare, t>::upper_bound(const tkey& key) const
{
    if (!_root) return cend();

    btree_node* const* cur_ptr = &_root;
    size_t child_index = 0;
    const_path_type path;
    std::optional<std::pair<const_path_type, size_t>> candidate;

    while (*cur_ptr != nullptr) {
        btree_node* node = *cur_ptr;
        path.push({cur_ptr, child_index});
        
        size_t idx = find_key_index(node, key);
        
        if (idx < node->_keys.size()) {
            candidate.emplace(path, idx);
        }
        
        if (node->_pointers.empty()) break;
        
        child_index = idx;
        cur_ptr = &(node->_pointers[idx]);
    }
    
    return candidate ? btree_const_iterator(candidate->first, candidate->second) : cend();
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
bool B_tree<tkey, tvalue, compare, t>::contains(const tkey& key) const
{
    if (!_root) return false;
    
    btree_node* cur = _root;
    while (cur) {
        size_t idx = find_key_index(cur, key);
        
        if (idx < cur->_keys.size() && !compare_keys(cur->_keys[idx].first, key) && !compare_keys(key, cur->_keys[idx].first)) {
            return true;
        }
        
        if (cur->_pointers.empty()) break;
        cur = cur->_pointers[idx];
    }
    return false;
}

// endregion lookup implementation

// region modifiers implementation

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
void B_tree<tkey, tvalue, compare, t>::clear() noexcept
{
    if (_root) {
        destroy_subtree(_root);
        _root = nullptr;
    }
    _size = 0;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
std::pair<typename B_tree<tkey, tvalue, compare, t>::btree_iterator, bool>
B_tree<tkey, tvalue, compare, t>::insert(const tree_data_type& data)
{
    if (contains(data.first)) return {find(data.first), false};

    if (!_root) {
        _root = allocate_node();
        _root->_keys.push_back(data);
        ++_size;
        return {begin(), true};
    }

    insert_into_subtree(_root, data);

    if (_root->_keys.size() > maximum_keys_in_node) {
        btree_node* new_root = allocate_node();
        new_root->_pointers.push_back(_root);
        _root = new_root;
        split_child(_root, 0);
    }

    ++_size;
    return {find(data.first), true};
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
std::pair<typename B_tree<tkey, tvalue, compare, t>::btree_iterator, bool>
B_tree<tkey, tvalue, compare, t>::insert(tree_data_type&& data)
{
    if (contains(data.first)) return {find(data.first), false};
    tkey key = data.first;
    
    if (!_root) {
        _root = allocate_node();
        _root->_keys.push_back(std::move(data));
        ++_size;
        return {begin(), true};
    }
    
    insert_into_subtree(_root, std::move(data));

    if (_root->_keys.size() > maximum_keys_in_node) {
        btree_node* new_root = allocate_node();
        new_root->_pointers.push_back(_root);
        _root = new_root;
        split_child(_root, 0);
    }

    ++_size;
    return {find(key), true};
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
template<typename... Args>
std::pair<typename B_tree<tkey, tvalue, compare, t>::btree_iterator, bool>
B_tree<tkey, tvalue, compare, t>::emplace(Args&&... args)
{
    return insert(tree_data_type(std::forward<Args>(args)...));
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator
B_tree<tkey, tvalue, compare, t>::insert_or_assign(const tree_data_type& data)
{
    auto it = find(data.first);
    if (it != end()) {
        it->second = data.second;
        return it;
    }
    return insert(data).first;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator
B_tree<tkey, tvalue, compare, t>::insert_or_assign(tree_data_type&& data)
{
    auto it = find(data.first);
    if (it != end()) {
        it->second = std::move(data.second);
        return it;
    }
    return insert(std::move(data)).first;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
template<typename... Args>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator
B_tree<tkey, tvalue, compare, t>::emplace_or_assign(Args&&... args)
{
    tree_data_type data(std::forward<Args>(args)...);
    return insert_or_assign(std::move(data));
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator
B_tree<tkey, tvalue, compare, t>::erase(btree_iterator pos)
{
    if (pos == end()) return end();
    return erase(pos->first);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator
B_tree<tkey, tvalue, compare, t>::erase(btree_const_iterator pos)
{
    if (pos == cend()) return end();
    return erase(pos->first);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator
B_tree<tkey, tvalue, compare, t>::erase(btree_iterator beg, btree_iterator en)
{
    if (beg == en) return en;
    
    std::optional<tkey> end_key;
    if (en != this->end()) {
        end_key = en->first;
    }
    
    while (beg != this->end()) {
        if (end_key.has_value() && !compare_keys(beg->first, *end_key) && !compare_keys(*end_key, beg->first)) {
            break;
        }
        beg = erase(beg);
    }
    return beg;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator
B_tree<tkey, tvalue, compare, t>::erase(btree_const_iterator beg, btree_const_iterator en)
{
    if (beg == en) return (en == cend()) ? end() : find(en->first);
    
    std::optional<tkey> end_key;
    if (en != this->cend()) {
        end_key = en->first;
    }
    
    btree_iterator current = (beg == this->cend()) ? this->end() : find(beg->first);
    
    while (current != this->end()) {
        if (end_key.has_value() && !compare_keys(current->first, *end_key) && !compare_keys(*end_key, current->first)) {
            break;
        }
        current = erase(current);
    }
    return current;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
typename B_tree<tkey, tvalue, compare, t>::btree_iterator
B_tree<tkey, tvalue, compare, t>::erase(const tkey& key)
{
    if (!_root) return end();
    if (!try_erase_from_node(_root, key)) return end();

    --_size;

    if (_root->_keys.empty()) {
        if (_root->_pointers.empty()) {
            deallocate_node(_root);
            _root = nullptr;
        } else {
            btree_node* old = _root;
            _root = _root->_pointers[0];
            deallocate_node(old);
        }
    }
    return lower_bound(key);
}

// endregion modifiers implementation



template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
std::stack<std::pair<typename B_tree<tkey, tvalue, compare, t>::btree_node**, size_t>>
B_tree<tkey, tvalue, compare, t>::get_leftmost_path() {
    std::stack<std::pair<btree_node**, size_t>> path;
    if (!_root) return path;
    
    btree_node** cur = &_root;
    path.push({cur, 0});
    
    while (!(*cur)->_pointers.empty()) {
        cur = &((*cur)->_pointers[0]);
        path.push({cur, 0});
    }
    return path;
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>
std::stack<std::pair<typename B_tree<tkey, tvalue, compare, t>::btree_node**, size_t>>
B_tree<tkey, tvalue, compare, t>::get_rightmost_path() {
    std::stack<std::pair<btree_node**, size_t>> path;
    if (!_root) return path;
    
    btree_node** cur = &_root;
    path.push({cur, 0});
    
    while (!(*cur)->_pointers.empty()) {
        size_t idx = (*cur)->_pointers.size() - 1;
        cur = &((*cur)->_pointers[idx]);
        path.push({cur, idx});
    }
    return path;
}

#endif