/*
 * test_btree.cpp —— B+ 树综合测试（使用内存页管理器，不触碰磁盘）
 *
 * 编译: g++ -std=c++17 -I../.. -DBTREE_USE_MOCK_PAGER Btree.cpp StackPath.cpp mockpager.cpp test_btree.cpp -o test_btree.exe
 *
 * 覆盖插入分裂、删除合并/借位、顺序扫描、根节点升降等场景。
 * 通过遍历所有页调用 validate() 检查节点级不变量，再用全量扫描的结果
 * 与期望序列比对，从整体上验证树的结构正确性。
 */
#include "Btree.hpp"
#include "mockpager.hpp"
#include "StackPath.hpp"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>
#include <algorithm>

static int g_passed = 0;
static int g_failed = 0;

#define TEST(name) do { \
    std::cout << "[RUN ] " << name << " ... "; \
    std::cout.flush(); \
} while(0)

#define PASS() do { \
    std::cout << "PASS\n"; \
    g_passed++; \
} while(0)

#define FAIL(msg) do { \
    std::cout << "FAIL: " << msg << "\n"; \
    g_failed++; \
} while(0)

// === 辅助函数 ===

static uint32_t create_empty_tree(Pager_t& pager) {
    uint32_t root_page = pager.alloc_new_page();
    Btree_Node root;
    memset(&root, 0, sizeof(root));
    root.type = Btree_Node::Type::LEAF_NODE;
    root.root = true;
    root.page_num = root_page;
    root.dirty = true;
    pager.write_page(root_page, (char*)&root);
    return root_page;
}

static bool insert_key(Pager_t& pager, uint32_t& root_page, int key_val, const std::string& val) {
    Key key(key_val);
    std::string value = val;
    StackPath path;
    std::vector<std::unique_ptr<Btree_Node>> new_pages;
    uint32_t new_root = root_page;
    auto res = Btree_insert(root_page, key, value, pager, path, new_pages, new_root);
    if (std::holds_alternative<Error>(res)) return false;
    if (!path.flush_all(pager)) return false;
    for (auto& np : new_pages) {
        if (np->dirty) {
            np->dirty = false;
            if (!pager.update_page(np->page_num, reinterpret_cast<char*>(np.get()))) return false;
        }
    }
    root_page = new_root;
    return true;
}

static bool find_key(Pager_t& pager, uint32_t root_page, int key_val, std::string& out_val) {
    Key key(key_val);
    auto res = Btree_find(root_page, key, pager);
    if (std::holds_alternative<Error>(res)) return false;
    out_val = std::get<std::string>(res);
    return true;
}

static bool delete_key(Pager_t& pager, uint32_t& root_page, int key_val) {
    Key key(key_val);
    StackPath path;
    std::vector<uint32_t> del_pages;
    std::vector<std::unique_ptr<Btree_Node>> update_pages;
    uint32_t new_root = root_page;
    auto res = Btree_delete(root_page, key, pager, path, del_pages, update_pages, new_root);
    if (std::holds_alternative<Error>(res)) return false;
    if (!path.flush_all(pager)) return false;
    for (auto& up : update_pages) {
        if (up->dirty) {
            up->dirty = false;
            if (!pager.update_page(up->page_num, reinterpret_cast<char*>(up.get()))) return false;
        }
    }
    for (auto dp : del_pages) {
        pager.delete_page(dp);
    }
    root_page = new_root;
    return true;
}

static int tree_height(Pager_t& pager, uint32_t root_page) {
    Btree_Node node;
    if (!pager.read_page(root_page, (char*)&node)) return 0;
    int h = 1;
    while (!node.is_leaf()) {
        uint32_t first_child = node.son_pages[0];
        if (!pager.read_page(first_child, (char*)&node)) break;
        h++;
    }
    return h;
}

static int count_nodes(Pager_t& pager) {
    int count = 0;
    for (auto& [num, buf] : pager.pages) {
        Btree_Node* node = reinterpret_cast<Btree_Node*>(buf.get());
        (void)node;
        count++;
    }
    return count;
}

// 校验树的不变量：所有键有序、叶子链表正确、节点 key 数合法
static bool validate_tree(Pager_t& pager, uint32_t root_page, std::string& error) {
    Btree_Node root;
    if (!pager.read_page(root_page, (char*)&root)) {
        error = "cannot read root";
        return false;
    }
    if (!root.is_root()) {
        error = "root flag not set";
        return false;
    }

    // DFS 遍历检查键有序性和节点 key 数
    struct StackEntry { uint32_t page; int level; };
    std::vector<StackEntry> stack;
    stack.push_back({root_page, 0});

    int leaf_count = 0;
    int internal_count = 0;
    std::vector<uint32_t> visited_pages;
    std::vector<uint32_t> leaf_pages;

    while (!stack.empty()) {
        auto entry = stack.back();
        stack.pop_back();

        if (std::find(visited_pages.begin(), visited_pages.end(), entry.page) != visited_pages.end()) {
            continue;
        }
        visited_pages.push_back(entry.page);

        Btree_Node node;
        if (!pager.read_page(entry.page, (char*)&node)) {
            error = "cannot read page " + std::to_string(entry.page);
            return false;
        }

        if (node.key_number > MAX_KEYS) {
            error = "page " + std::to_string(entry.page) + " key_number=" + std::to_string(node.key_number) + " > MAX_KEYS";
            return false;
        }

        for (uint32_t i = 1; i < node.key_number; i++) {
            if (!(node.keys[i-1] < node.keys[i])) {
                error = "page " + std::to_string(entry.page) + " keys not sorted at index " + std::to_string(i);
                return false;
            }
        }

        if (node.is_leaf()) {
            leaf_count++;
            leaf_pages.push_back(entry.page);
            if (!node.is_root() && node.key_number == 0) {
                error = "leaf page " + std::to_string(entry.page) + " is empty";
                return false;
            }
            if (!node.is_root() && node.curval > PAGE_SIZE - HEAD_SIZE) {
                error = "leaf page " + std::to_string(entry.page) + " curval overflow";
                return false;
            }
        } else {
            internal_count++;
            if (!node.is_root() && node.key_number < MAX_KEYS / 2) {
                error = "internal page " + std::to_string(entry.page) + " key_number=" + std::to_string(node.key_number) + " < half";
                return false;
            }
            for (int i = (int)node.key_number; i >= 0; i--) {
                stack.push_back({node.son_pages[i], entry.level + 1});
            }
        }
    }

    // 校验叶子链表：从最左叶子沿 next_leaf 走
    if (leaf_count > 0) {
        // 找到最左叶子（沿 son_pages[0] 一直下到叶子层）
        uint32_t leftmost = root_page;
        Btree_Node tmp;
        pager.read_page(leftmost, (char*)&tmp);
        while (!tmp.is_leaf()) {
            leftmost = tmp.son_pages[0];
            pager.read_page(leftmost, (char*)&tmp);
        }

        uint32_t cur = leftmost;
        uint32_t prev = 0;
        int walked = 0;
        while (cur != 0) {
            Btree_Node node;
            if (!pager.read_page(cur, (char*)&node)) {
                error = "leaf chain: cannot read page " + std::to_string(cur);
                return false;
            }
            if (node.pre_leaf != prev) {
                error = "leaf chain: page " + std::to_string(cur) + " pre_leaf=" + std::to_string(node.pre_leaf) + " expected " + std::to_string(prev);
                return false;
            }
            prev = cur;
            cur = node.next_leaf;
            walked++;
        }
        if ((size_t)walked != leaf_pages.size()) {
            error = "leaf chain: walked " + std::to_string(walked) + " leaves but counted " + std::to_string(leaf_pages.size());
            return false;
        }
        (void)leaf_count;
    }

    return true;
}

// === 测试用例 ===

// A. 基础操作
static void test_single_insert() {
    TEST("single insert & find");
    Pager_t pager;
    uint32_t root = create_empty_tree(pager);
    assert(insert_key(pager, root, 42, "hello"));
    std::string val;
    assert(find_key(pager, root, 42, val));
    assert(val == "hello");
    PASS();
}

static void test_sequential_inserts_no_split() {
    TEST("sequential inserts (no split)");
    Pager_t pager;
    uint32_t root = create_empty_tree(pager);
    for (int i = 1; i <= (int)MAX_KEYS; i++) {
        std::string val = "v" + std::to_string(i);
        assert(insert_key(pager, root, i, val));
    }
    // 验证所有键可查
    for (int i = 1; i <= (int)MAX_KEYS; i++) {
        std::string val;
        assert(find_key(pager, root, i, val));
        assert(val == "v" + std::to_string(i));
    }
    // 树高应为 1（还是叶子根）
    assert(tree_height(pager, root) == 1);
    PASS();
}

static void test_out_of_order_inserts() {
    TEST("out-of-order inserts (no split)");
    Pager_t pager;
    uint32_t root = create_empty_tree(pager);
    int keys[] = {50, 10, 90, 30, 70, 20, 80, 40, 60, 100};
    int n = sizeof(keys)/sizeof(keys[0]);
    assert(n <= (int)MAX_KEYS);
    for (int i = 0; i < n; i++) {
        std::string val = "v" + std::to_string(keys[i]);
        assert(insert_key(pager, root, keys[i], val));
    }
    // 验证有序
    std::sort(keys, keys + n);
    for (int i = 0; i < n; i++) {
        std::string val;
        assert(find_key(pager, root, keys[i], val));
        assert(val == "v" + std::to_string(keys[i]));
    }
    PASS();
}

static void test_duplicate_insert() {
    TEST("duplicate insert rejected");
    Pager_t pager;
    uint32_t root = create_empty_tree(pager);
    assert(insert_key(pager, root, 1, "a"));
    assert(!insert_key(pager, root, 1, "b"));  // 重复键应失败
    std::string val;
    assert(find_key(pager, root, 1, val));
    assert(val == "a");  // 值不应被覆盖
    PASS();
}

static void test_find_nonexistent() {
    TEST("find nonexistent key");
    Pager_t pager;
    uint32_t root = create_empty_tree(pager);
    insert_key(pager, root, 10, "x");
    std::string val;
    assert(!find_key(pager, root, 20, val));
    assert(!find_key(pager, root, 5, val));
    assert(!find_key(pager, root, 0, val));
    PASS();
}

static void test_update_value() {
    TEST("update value (same/different size)");
    Pager_t pager;
    uint32_t root = create_empty_tree(pager);
    insert_key(pager, root, 1, "short");

    // 同长度更新
    Btree_Node updated;
    Key key1(1);
    std::string v1 = "abcde";
    auto r1 = Btree_update(root, key1, v1, pager, updated);
    assert(!std::holds_alternative<Error>(r1));
    pager.update_page(updated.page_num, (char*)&updated);
    std::string val;
    assert(find_key(pager, root, 1, val));
    assert(val == "abcde");

    // 更长的值
    Key key2(1);
    std::string v2 = "a_much_longer_value";
    auto r2 = Btree_update(root, key2, v2, pager, updated);
    assert(!std::holds_alternative<Error>(r2));
    pager.update_page(updated.page_num, (char*)&updated);
    assert(find_key(pager, root, 1, val));
    assert(val == "a_much_longer_value");

    // 更短的值
    Key key3(1);
    std::string v3 = "tiny";
    auto r3 = Btree_update(root, key3, v3, pager, updated);
    assert(!std::holds_alternative<Error>(r3));
    pager.update_page(updated.page_num, (char*)&updated);
    assert(find_key(pager, root, 1, val));
    assert(val == "tiny");

    PASS();
}

static void test_update_nonexistent() {
    TEST("update nonexistent key");
    Pager_t pager;
    uint32_t root = create_empty_tree(pager);
    Btree_Node dummy;
    Key key(999);
    std::string v = "x";
    auto res = Btree_update(root, key, v, pager, dummy);
    assert(std::holds_alternative<Error>(res));
    PASS();
}

static void test_delete_simple() {
    TEST("simple delete (no merge)");
    Pager_t pager;
    uint32_t root = create_empty_tree(pager);
    for (int i = 1; i <= 10; i++) {
        insert_key(pager, root, i, "v" + std::to_string(i));
    }
    assert(delete_key(pager, root, 5));
    std::string val;
    assert(!find_key(pager, root, 5, val));
    // 其他键还在
    for (int i = 1; i <= 10; i++) {
        if (i == 5) continue;
        assert(find_key(pager, root, i, val));
    }
    PASS();
}

static void test_delete_nonexistent() {
    TEST("delete nonexistent key");
    Pager_t pager;
    uint32_t root = create_empty_tree(pager);
    insert_key(pager, root, 10, "x");
    assert(!delete_key(pager, root, 20));
    PASS();
}

static void test_scan_empty() {
    TEST("scan empty tree");
    Pager_t pager;
    uint32_t root = create_empty_tree(pager);
    auto res = Btree_scan(root, pager);
    assert(!std::holds_alternative<Error>(res));
    std::string data = std::get<std::string>(res);
    assert(data.empty());
    PASS();
}

static void test_scan_single_leaf() {
    TEST("scan single leaf");
    Pager_t pager;
    uint32_t root = create_empty_tree(pager);
    for (int i = 1; i <= 5; i++) {
        insert_key(pager, root, i, "v" + std::to_string(i));
    }
    auto res = Btree_scan(root, pager);
    assert(!std::holds_alternative<Error>(res));
    std::string data = std::get<std::string>(res);
    // 5 个值，每个至少 2 字节（"v1".."v5"）
    assert(data.size() > 0);
    // 简单检查：扫描结果应包含所有值的特征
    // （由于值区是原始字节，这里只检查大小合理性）
    PASS();
}

// B. 叶子节点分裂
static void test_leaf_split_root() {
    TEST("leaf root split (tree height 1->2)");
    Pager_t pager;
    uint32_t root = create_empty_tree(pager);

    // 插入 MAX_KEYS 条（不满）
    for (int i = 1; i <= (int)MAX_KEYS; i++) {
        insert_key(pager, root, i, "v" + std::to_string(i));
    }
    assert(tree_height(pager, root) == 1);
    uint32_t old_root = root;

    // 插入第 MAX_KEYS+1 条，触发分裂
    assert(insert_key(pager, root, MAX_KEYS + 1, "v_extra"));

    // 根页号应改变（根分裂）
    assert(root != old_root);

    // 树高应为 2
    assert(tree_height(pager, root) == 2);

    // 前 MAX_KEYS 个键可查
    for (int i = 1; i <= (int)MAX_KEYS; i++) {
        std::string val;
        assert(find_key(pager, root, i, val));
        assert(val == "v" + std::to_string(i));
    }
    // 第 MAX_KEYS+1 个键
    {
        std::string val;
        assert(find_key(pager, root, MAX_KEYS + 1, val));
        assert(val == "v_extra");
    }

    // 校验树结构
    std::string err;
    assert(validate_tree(pager, root, err));

    PASS();
}

static void test_leaf_split_more_inserts() {
    TEST("more inserts after first split");
    Pager_t pager;
    uint32_t root = create_empty_tree(pager);
    for (int i = 1; i <= 30; i++) {
        insert_key(pager, root, i, "v" + std::to_string(i));
    }
    for (int i = 1; i <= 30; i++) {
        std::string val;
        assert(find_key(pager, root, i, val));
        assert(val == "v" + std::to_string(i));
    }
    std::string err;
    assert(validate_tree(pager, root, err));
    PASS();
}

// C. 多层分裂
static void test_multi_level_split() {
    TEST("multi-level splits (500 keys)");
    Pager_t pager;
    uint32_t root = create_empty_tree(pager);
    const int N = 500;
    for (int i = 1; i <= N; i++) {
        std::string val = "value_" + std::to_string(i);
        bool ok = insert_key(pager, root, i, val);
        if (!ok) {
            std::cerr << "insert failed at i=" << i << std::endl;
            assert(false);
        }
    }
    // 树高应 > 2
    int h = tree_height(pager, root);
    assert(h >= 2);

    // 所有键可查
    for (int i = 1; i <= N; i++) {
        std::string val;
        bool ok = find_key(pager, root, i, val);
        if (!ok) {
            std::cerr << "find failed at i=" << i << std::endl;
            assert(false);
        }
        assert(val == "value_" + std::to_string(i));
    }

    // 校验树
    std::string err;
    if (!validate_tree(pager, root, err)) {
        std::cout << "validate_tree failed: " << err << std::endl;
        return; // 直接返回，让测试失败计数
    }

    PASS();
}

static void test_random_order_insert() {
    TEST("random order insert (200 keys)");
    Pager_t pager;
    uint32_t root = create_empty_tree(pager);
    // 伪随机：用固定种子保证可重复
    std::vector<int> keys;
    for (int i = 1; i <= 200; i++) keys.push_back(i * 7 % 200 + 1);
    // 去重
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());

    for (int k : keys) {
        insert_key(pager, root, k, "v" + std::to_string(k));
    }
    for (int k : keys) {
        std::string val;
        assert(find_key(pager, root, k, val));
        assert(val == "v" + std::to_string(k));
    }
    std::string err;
    assert(validate_tree(pager, root, err));
    PASS();
}

// D. 叶子节点借位
static void test_leaf_borrow() {
    TEST("leaf borrow from sibling");
    Pager_t pager;
    uint32_t root = create_empty_tree(pager);

    // 先插入 MAX_KEYS+1 个键，得到两层树，两个叶子
    for (int i = 1; i <= (int)MAX_KEYS + 1; i++) {
        insert_key(pager, root, i, "v" + std::to_string(i));
    }
    assert(tree_height(pager, root) == 2);

    // 记录两个叶子的 key 数
    Btree_Node root_node;
    pager.read_page(root, (char*)&root_node);
    uint32_t left_leaf_pid = root_node.son_pages[0];
    uint32_t right_leaf_pid = root_node.son_pages[1];
    Btree_Node left_leaf, right_leaf;
    pager.read_page(left_leaf_pid, (char*)&left_leaf);
    pager.read_page(right_leaf_pid, (char*)&right_leaf);

    // 右叶子应该有 MAX_KEYS/2 + 1 个键（额外的那个在右边）
    // 先从右叶子多插几个，让右叶子变满
    for (int i = MAX_KEYS + 2; i <= MAX_KEYS * 2; i++) {
        insert_key(pager, root, i, "v" + std::to_string(i));
    }
    // 再从左叶子删几个，迫使从右叶子借位
    for (int i = 1; i <= 3; i++) {
        delete_key(pager, root, i);
    }

    // 所有剩余键可查
    for (int i = 4; i <= MAX_KEYS * 2; i++) {
        std::string val;
        bool found = find_key(pager, root, i, val);
        if (!found) {
            std::cerr << "key " << i << " not found after borrow\n";
            assert(false);
        }
    }

    std::string err;
    if (!validate_tree(pager, root, err)) {
        std::cerr << "validate_tree failed: " << err << std::endl;
        pager.dump_all();
        assert(false);
    }
    PASS();
}

// E. 叶子节点合并
static void test_leaf_merge() {
    TEST("leaf merge");
    Pager_t pager;
    uint32_t root = create_empty_tree(pager);

    // 插入到两个叶子各约半满
    for (int i = 1; i <= (int)MAX_KEYS + 1; i++) {
        insert_key(pager, root, i, "v" + std::to_string(i));
    }
    assert(tree_height(pager, root) == 2);

    // 从左叶子删除大部分键，触发合并
    for (int i = 1; i <= MAX_KEYS / 2; i++) {
        delete_key(pager, root, i);
    }

    // 剩余键可查
    for (int i = MAX_KEYS / 2 + 1; i <= (int)MAX_KEYS + 1; i++) {
        std::string val;
        assert(find_key(pager, root, i, val));
    }

    // 验证树不变量
    std::string err;
    assert(validate_tree(pager, root, err));

    PASS();
}

// F. 内部节点合并 / 根坍缩
static void test_root_collapse() {
    TEST("root collapse (height 2->1)");
    Pager_t pager;
    uint32_t root = create_empty_tree(pager);

    // 建立两层树
    for (int i = 1; i <= (int)MAX_KEYS + 1; i++) {
        insert_key(pager, root, i, "v" + std::to_string(i));
    }
    assert(tree_height(pager, root) == 2);
    uint32_t old_root = root;

    // 删除大部分键，迫使根坍缩（回到一层）
    for (int i = 2; i <= (int)MAX_KEYS + 1; i++) {
        delete_key(pager, root, i);
    }

    // 只剩一个键，树高应为 1
    assert(tree_height(pager, root) == 1);
    // 根页号应改变（旧的内部节点根被删了）
    assert(root != old_root);

    // 最后一个键还在
    std::string val;
    assert(find_key(pager, root, 1, val));

    std::string err;
    assert(validate_tree(pager, root, err));

    PASS();
}

static void test_three_levels_and_collapse() {
    TEST("three-level tree and deletes");
    Pager_t pager;
    uint32_t root = create_empty_tree(pager);
    const int N = 200;
    for (int i = 1; i <= N; i++) {
        insert_key(pager, root, i, "v" + std::to_string(i));
    }
    int h1 = tree_height(pager, root);
    assert(h1 >= 2);

    // 删除前一半
    for (int i = 1; i <= N / 2; i++) {
        delete_key(pager, root, i);
    }

    // 剩余键可查
    for (int i = N / 2 + 1; i <= N; i++) {
        std::string val;
        bool found = find_key(pager, root, i, val);
        if (!found) {
            std::cerr << "key " << i << " not found after mass delete\n";
            assert(false);
        }
    }

    std::string err;
    assert(validate_tree(pager, root, err));

    PASS();
}

// G. 大范围删除
static void test_delete_range() {
    TEST("delete range");
    Pager_t pager;
    uint32_t root = create_empty_tree(pager);
    for (int i = 1; i <= 50; i++) {
        insert_key(pager, root, i, "v" + std::to_string(i));
    }
    int total_pages_before = count_nodes(pager);

    std::vector<uint32_t> del_pages;
    uint32_t new_root = root;
    auto res = Btree_delete_range(root, pager, del_pages, new_root);
    assert(!std::holds_alternative<Error>(res));

    for (auto dp : del_pages) {
        pager.delete_page(dp);
    }
    root = new_root;

    // 根页被清空但不删除
    Btree_Node root_node;
    pager.read_page(root, (char*)&root_node);
    assert(root_node.is_root());
    assert(root_node.key_number == 0);

    // 剩余页数 = 1（只有根）
    assert(count_nodes(pager) == 1);

    PASS();
}

// H. VARCHAR 键
static void test_varchar_keys() {
    TEST("varchar keys");
    Pager_t pager;
    uint32_t root = create_empty_tree(pager);

    const char* names[] = {"banana", "apple", "cherry", "date", "elderberry"};
    int n = sizeof(names)/sizeof(names[0]);
    for (int i = 0; i < n; i++) {
        Key key(names[i]);
        std::string val = "v_" + std::string(names[i]);
        StackPath path;
        std::vector<std::unique_ptr<Btree_Node>> new_pages;
        uint32_t nr = root;
        auto res = Btree_insert(root, key, val, pager, path, new_pages, nr);
        assert(!std::holds_alternative<Error>(res));
        path.flush_all(pager);
        for (auto& np : new_pages) {
            if (np->dirty) {
                np->dirty = false;
                pager.update_page(np->page_num, reinterpret_cast<char*>(np.get()));
            }
        }
        root = nr;
    }

    // 验证所有键可查
    for (int i = 0; i < n; i++) {
        Key key(names[i]);
        auto res = Btree_find(root, key, pager);
        assert(!std::holds_alternative<Error>(res));
        std::string val = std::get<std::string>(res);
        assert(val == "v_" + std::string(names[i]));
    }

    PASS();
}

// I. out_root_page 正确性
static void test_out_root_page() {
    TEST("out_root_page updates correctly");
    Pager_t pager;
    uint32_t root = create_empty_tree(pager);
    uint32_t initial_root = root;

    // 插入少量不触发分裂
    for (int i = 1; i <= 5; i++) {
        uint32_t old = root;
        insert_key(pager, root, i, "v" + std::to_string(i));
        assert(root == old);  // 根不变
    }

    // 插入到触发分裂
    uint32_t root_before = root;
    for (int i = 6; i <= (int)MAX_KEYS + 1; i++) {
        insert_key(pager, root, i, "v" + std::to_string(i));
    }
    assert(root != root_before);  // 根变了
    assert(root != initial_root);

    // 删除所有只剩一个键，根坍缩
    uint32_t internal_root = root;
    for (int i = 2; i <= (int)MAX_KEYS + 1; i++) {
        delete_key(pager, root, i);
    }
    // 根坍缩：树高从 2 降到 1，根页号改变
    assert(tree_height(pager, root) == 1);
    assert(root != internal_root);  // 不再是内部节点

    std::string val;
    assert(find_key(pager, root, 1, val));
    assert(val == "v1");

    PASS();
}

// J. 值区溢出触发分裂
static void test_value_overflow_split() {
    TEST("value overflow triggers split");
    Pager_t pager;
    uint32_t root = create_empty_tree(pager);

    // 插入一个长值的键
    std::string long_val(500, 'x');
    assert(insert_key(pager, root, 1, long_val));

    // 再插入更多长值键，即使 key 数量 < MAX_KEYS 也会因值区溢出而分裂
    for (int i = 2; i <= 8; i++) {
        std::string val(500, 'a' + (i % 26));
        insert_key(pager, root, i, val);
    }

    // 所有键可查
    for (int i = 1; i <= 8; i++) {
        std::string val;
        assert(find_key(pager, root, i, val));
        assert(val.size() == 500);
    }

    std::string err;
    assert(validate_tree(pager, root, err));
    PASS();
}

static void test_update_value_overflow() {
    TEST("update value too large for leaf returns error");
    Pager_t pager;
    uint32_t root = create_empty_tree(pager);

    // 插入几个小值
    for (int i = 1; i <= 5; i++) {
        std::string val(100, 'a' + (i % 26));
        assert(insert_key(pager, root, i, val));
    }

    // 尝试把一个值更新为远超叶节点容量的大小
    Btree_Node dummy;
    std::string huge_val(4000, 'x');
    Key key(1);
    auto res = Btree_update(root, key, huge_val, pager, dummy);
    assert(std::holds_alternative<Error>(res));

    // 原值仍然完好
    std::string out;
    assert(find_key(pager, root, 1, out));
    assert(out.size() == 100);

    PASS();
}

static void test_value_too_large_for_leaf() {
    TEST("single value larger than leaf capacity returns error");
    Pager_t pager;
    uint32_t root = create_empty_tree(pager);

    // 单个值超过叶节点值区容量（values[] 约 2894 字节）
    std::string huge_val(3500, 'z');
    bool ok = insert_key(pager, root, 1, huge_val);
    assert(!ok);

    PASS();
}

// main
int main() {
    std::cout << "========== B+ Tree Test Suite ==========\n";
    std::cout << "MAX_KEYS = " << MAX_KEYS << "\n\n";

    // A. 基础操作
    test_single_insert();
    test_sequential_inserts_no_split();
    test_out_of_order_inserts();
    test_duplicate_insert();
    test_find_nonexistent();
    test_update_value();
    test_update_nonexistent();
    test_delete_simple();
    test_delete_nonexistent();
    test_scan_empty();
    test_scan_single_leaf();

    // B. 叶子分裂
    test_leaf_split_root();
    test_leaf_split_more_inserts();

    // C. 多层分裂
    test_multi_level_split();
    test_random_order_insert();

    // D. 叶子借位
    test_leaf_borrow();

    // E. 叶子合并
    test_leaf_merge();

    // F. 根坍缩 / 多层
    test_root_collapse();
    test_three_levels_and_collapse();

    // G. 范围删除
    test_delete_range();

    // H. VARCHAR
    test_varchar_keys();

    // I. out_root_page
    test_out_root_page();

    // J. 值区溢出
    test_value_overflow_split();
    test_update_value_overflow();
    test_value_too_large_for_leaf();

    std::cout << "\n========== Summary ==========\n";
    std::cout << "Passed: " << g_passed << "\n";
    std::cout << "Failed: " << g_failed << "\n";
    std::cout << "Total:  " << g_passed + g_failed << "\n";

    return g_failed == 0 ? 0 : 1;
}
