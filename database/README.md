# database 内部架构

关系型数据库的实现说明。对外功能与构建方式见仓库根目录 `README.md`。

## 分层与文件

```
Main.cpp
  └── Parse      tokenize → parse → sense
        └── Execute    按命令类型分派
              └── Storage_engine   storage / pager / B+Tree
                    └── Help         error / serialize / show / cast
```

依赖方向自下而上单向。`Storage` 是门面，`Executor` 只通过它访问 Pager 与 SymbolTable。

| 模块 | 文件 | 行数 | 职责 |
|------|------|------|------|
| 入口 | `Main.cpp` | ~130 | 初始化三层组件，REPL 循环，三段式处理每条 SQL |
| 词法 | `Parse/tokenize.cpp` | ~160 | 字符串 → Token 序列 |
| 语法 | `Parse/parse.cpp` | ~680 | Token → AST（手写递归下降） |
| 语义 | `Parse/sense.cpp` | ~430 | 表/列存在性与类型校验，符号表重建 |
| 执行 | `Execute/executor.cpp` | ~610 | 命令分派、调用 B+ 树、统一落盘 |
| 门面 | `Storage_engine/storage.cpp` | ~50 | 持有 Pager 与 SymbolTable |
| 页缓存 | `Storage_engine/pager.cpp` | ~370 | 页读写、LRU 淘汰、位图分配 |
| 索引 | `Storage_engine/B+Tree/Btree.cpp` | ~1100 | 节点操作、分裂合并、树级 API |
| 路径栈 | `Storage_engine/B+Tree/StackPath.cpp` | ~65 | 根到叶路径，供级联分裂/合并回溯 |
| 序列化 | `Help/Serialize/serialize.cpp` | ~320 | 行与表元信息的二进制编解码 |
| 展示 | `Help/Show/show.cpp` | ~70 | 结果集 → ASCII 表格 |
| 错误 | `Help/Error/error.*` | ~50 | `Result<T> = std::variant<T, Error>` |

测试：`Storage_engine/B+Tree/test_btree.cpp`（25 个用例，用内存页管理器）、
`test_integration.cpp`（端到端）、`Help/Serialize/test.cpp`、`Parse/test.cpp`（交互式）。

## 错误传递

所有可能失败的函数返回 `Result<T> = std::variant<T, Error>`：

```c
struct Error {
    enum class Layer { Parser, Executor, Pager, Btree };
    Layer layer;
    std::string message;
};
```

调用方用 `std::holds_alternative<Error>(r)` 判定，错误可原样跨层上传。
`Error::error_message()` 把层名与消息拼成用户可见文本（如"存储层发生错误：..."）。

`B+Tree/error.hpp` 是同一份定义的副本，只为让 B+ 树单元测试不依赖整个源码树。
生产代码统一用 `Help/Error/error.hpp`，两者不可同时 include。

## 磁盘布局

数据库文件 `sql.db`，每页 4096 字节：

```
 page 0        主页面 mainpage
 page 1 ~ 511  数据页（位图中 1 表示已分配）
```

主页面内容：

```c
struct mainpage {
    uint32_t magic;          // 0x66666666，识别文件是否为本数据库
    uint32_t page_size;
    uint32_t version;
    uint32_t pages_num;
    uint32_t bitmap[16];     // 16 × 32 = 512 bit，对应 512 页
    uint32_t system_page;    // 系统表的 B+ 树根页号
    uint8_t  reserved[...];
};
```

每个数据页在内存中直接映射为一个 `Btree_Node`（`static_assert(sizeof(Btree_Node) == PAGE_SIZE)`），
读写页就是 `memcpy` 一整页，不做额外的编解码。

## Pager：页缓存与 LRU

```
          哈希表                         双向链表（LRU 顺序）
  page_num ──▶ Page*  ──┐          head ◀──▶ ... ◀──▶ tail
                        │         (最近使用)        (最久未用)
                        └── Page { page_num, data[4096], dirty, pre, next }
```

| 接口 | 行为 |
|------|------|
| `read_page` | 命中则 `move_to_head` 并拷贝；未命中则从磁盘读入，插入链表头，然后 `evict_if_needed` |
| `update_page` | 写入缓存并置 `dirty`，**不立即落盘** |
| `write_page` | 把某个脏页写回磁盘并清除 `dirty` |
| `delete_page` | 移出缓存 + 清位图 + 页计数减一 |
| `alloc_new_page` | 位图中找第一个空闲位（跳过 0），新页清零写盘，返回页号 |
| `flush_page` | 所有脏页 + 主页面落盘（析构与显式调用时） |

`evict_if_needed()` 在缓存超过 `MAX_CACHED_PAGES = 64` 时从尾部淘汰；
**淘汰前若页是脏的必须先写回**，否则数据丢失。

`is_page_allocated()` 用位图判断合法性，是读写前的统一防线；page 0 恒视为已分配。

Windows 用 `_lseek` + `_read`/`_write`，POSIX 用 `pread`/`pwrite`，
两套分支都封在本文件内，上层无感知。

## B+ 树

### 节点结构

```
struct Btree_Node {              // 恰好 4096 字节
    dirty, root, type
    key_number
    father, page_num            // 父页号、自身页号
    pre_leaf, next_leaf          // 叶子链表
    curval                       // values 已用字节数（仅叶子）
    son_pages[MAX_KEYS + 1]      // 17 个子页指针
    offsets[MAX_KEYS]            // 16 个值在 values 中的起始偏移
    keys[MAX_KEYS]               // 16 个键，各 64 字节
    values[PAGE_SIZE - HEAD_SIZE] // 行数据（仅叶子）
}
```

`MAX_KEYS = 16`，因此中间节点有 17 个子指针。键支持 int / float / varchar 三种，
比较运算符内部按 `type` 分派到整数比较、浮点比较或 `strcmp`。

### 键与值的定位

- **中间节点**：`Find_Key_Index` 二分找到第一个 `keys[i] >= key`；`Find_Next_Page`
  在相等时走 `son_pages[index + 1]`（右侧），保证等于分界值的记录落在右子树
- **叶子节点**：第 `i` 个值的长度 =

  ```
  (i + 1 < key_number ? offsets[i + 1] : curval) - offsets[i]
  ```

  `offsets[0]` 恒为 0，插入/删除时同步维护后续偏移——这是紧凑存储且不额外占空间的关键

### 插入

```
沿路径下探，每个节点压入 StackPath
    │
    ├─ 叶子有空位且键不重复 → Insert_Value，结束
    │
    └─ 否则 Split_chain 沿栈向上
         ├─ Page_Split：后半部分搬到新节点，分界键 new_page.keys[0] 上浮
         ├─ 父节点有空位 → 插入上浮键，结束
         ├─ 父节点也满 → 父节点成为下一轮分裂对象
         └─ 当前是根 → create_new_root，树高 +1，out_root_page 更新
    │
    └─ 分裂后重新判断：键可能已被分界键挤到新节点，插入目标需重算
```

`Insert_Value` 要在紧凑区中插入，三件事必须同时做对：
插入点之后的字节整体 `memmove` 后移、插入点之后的所有 `offsets` 加上 `val_size`、
`curval` 与 `key_number` 同步增加。

### 删除

```
沿路径下探到叶子
    │
    ├─ 叶子键数 > MAX_KEYS/2 → Delete_Value，结束（不会破坏平衡）
    │
    └─ 否则 Merge_chain 先修结构，再删除
         ├─ 向左兄弟借（兄弟键数 > MAX_KEYS/2）→ 成功后结束
         ├─ 向右兄弟借 → 同上
         └─ 都借不到 → 与兄弟合并，父节点删一个键
               └─ 父节点可能因此欠载 → 继续向上
               └─ 叶子合并要修正相邻叶子的 pre/next，保持链表完整
         └─ 父节点键数归零且是根 → 唯一子节点升为新根，树高 -1
```

**顺序很重要**：先修结构再删值，因为 `Merge_chain` 计算父节点分隔键时叶子内容还是完整的。

`Merge_chain` 用 `update_pages` / `del_pages` 收集"要回写"和"要释放"的页，
由调用方统一落盘，避免级联过程中反复 IO。

### 清空整表

`Btree_delete_range` 用显式栈做迭代式后序遍历（避免递归爆栈）：
每个栈元素带一个 `flag`，`false` 表示还没展开子节点，展开后置 `true`；
`true` 时处置自身——根节点 `clear()` 保留页，其余加入 `del_pages`。
用于 `delete`（无 where）与 `drop table`。

### 全表扫描

`Btree_scan` 先沿最左子指针下到最左叶子，再沿 `next_leaf` 遍历整条叶子链表，
把每页的 `values` 区依次拼接。结果天然按主键升序。

### StackPath 的巧思

`path` 是 `vector<unique_ptr<Btree_Node>>`，而 `curtop` 是"逻辑栈顶"：

```c
void StackPath::pop() { curtop--; }    // 只移动索引，不销毁对象
```

被弹出的节点仍留在 vector 里，`flush_all()` 才能遍历到"曾经在栈上、已被弹出但改过"的父节点。
若 `pop()` 真的销毁对象，级联分裂/合并对父节点的修改就全丢了。

## 系统表与元数据自举

系统表本身也是一棵 B+ 树，`mainpage.system_page` 存它的根页号。键是表名，值是表元信息：

```
值布局： [RowHeader][int:name_len][string:table_name][int:page_num][int:column_num]
                   [int:size1][string:columns(逗号分隔)][int:size2][string:types(逗号分隔)]
```

创建流程（`execute_create`）：

1. 分配新页作该表 B+ 树根（空叶子）
2. 符号表登记表定义与根页号
3. 系统根页号为 0（首次 create）→ 先分配并初始化系统表根
4. `serialize_system` 序列化元信息，`Btree_insert` 写入系统树
5. 系统树若因分裂换了根，回写 `mainpage.system_page`

加载流程（`InitSymbolTable`）：

1. 新库或 `system_page == 0` → 返回空符号表
2. `Btree_scan` 全量扫描系统树
3. 逐行 `deserialize_system`，每行还原一张表的列定义与根页号
4. 解析失败的行直接跳过，尽量多恢复

## 行存储格式

```
[RowHeader][int:val][size][string:val][RowHeader][...]
|offset[0]                            |offset[1]

RowHeader = { uint16 length, uint8 column_num, uint8 NULLbitmap }
```

- 定长列（INT / FLOAT）固定 4 字节
- 变长列（TEXT / VARCHAR）前面带一个 int 长度
- `NULLbitmap` 第 `i` 位为 1 表示第 `i` 列为 NULL，该列不占空间

`deserialize` 支持投影：只需 `need` 中列出的列，其余列仅按格式跳过、不做字符串转换。
跳读变长列时必须先读出尺寸字段才能正确推进指针。

## 各命令的执行路径

| 命令 | 主要步骤 |
|------|----------|
| `create` | 分配根页 → 建空叶子 → 符号表登记 → 元信息写入系统树 |
| `insert` | 首列为键 → 序列化成行 → `Btree_insert` → flush 路径与新页 → 根页号可能更新 |
| `select` 带 where | `Btree_find` 精确查一行 → `deserialize` 投影 → 组装 Info |
| `select` 无 where | `Btree_scan` 全表 → `deserialize` → 组装 Info |
| `update` | `Btree_find` 读整行 → 改一列 → 重新序列化 → `Btree_update` 写回 |
| `delete` 带 where | `Btree_delete` → flush 路径、更新页、释放待删页 |
| `delete` 无 where | `Btree_delete_range` → 释放所有非根页 |
| `drop` | 清空并释放数据页（含根页）→ 从系统树删元信息 → 符号表移除 |
| `show tables` | 遍历符号表根页映射，输出表名与第 0 列（主键）信息 |

所有写操作的最后一步都是同一套落盘序列：
**先 `path.flush_all()`，再写 `new_pages` / `update_pages`，最后释放 `del_pages`。**

## 约束与简化

| 约束 | 原因 |
|------|------|
| 主键固定为第 1 列 | 简化建表与索引逻辑，无需解析主键声明 |
| 谓词只支持主键单个等值 | 不支持范围扫描与多条件；`parse_expr` 直接拒绝 `op != '='` |
| `update` 必须带 where | 全表更新的语义与索引维护成本未处理 |
| 单文件 512 页上限 | `mainpage.bitmap` 固定 16 个 uint32 |
| 缓存 64 页 | `MAX_CACHED_PAGES`，超出走 LRU 淘汰 |
| VARCHAR 键上限 47 字节 | `Key.str[48]`，超长静默截断 |
| 无事务、无并发控制 | 单进程单线程 REPL |
| `Main.cpp` 不处理 `SIGINT` 与异常退出 | 依赖 `Pager_t` 析构中的 `flush_page()` 落盘 |
