# Allocator 内部架构

用户态内存分配器的实现说明。对外功能与构建方式见仓库根目录 `README.md`。

## 文件构成

| 文件 | 职责 |
|------|------|
| `allocator.h` | 公开接口：`my_malloc` / `my_free` / `my_realloc`，以及调试接口 `allocator_get_stats` / `allocator_validate` |
| `allocator.c` | 全部实现 |
| `Memory Allocator.h` | 旧入口，仅转发到 `allocator.h`，保留是为了不破坏既有的包含路径 |

## 核心数据结构

### 块头

每个用户块前面藏一个定长块头（`block_header`），用户拿到的指针始终指向块头之后：

```
     块头(32B)              用户区(size)
  ┌──────────────────┬──────────────────────────┐
  │ magic            │                          │
  │ size             │      用户数据            │
  │ prev_size        │                          │
  │ next / prev      │                          │
  │ flags            │                          │
  └──────────────────┴──────────────────────────┘
  ↑                  ↑
  块头地址            用户指针 = 块头 + header_size()
```

字段含义：

- `magic` —— 完整性校验，用于识别野指针和检测越界写
- `size` —— **用户区**字节数（不含块头），因此下一块 = 本块 + `header_size()` + `size`
- `prev_size` —— 前一块的用户区字节数，`0` 表示本段首块；这是向低地址合并的唯一线索
- `next` / `prev` —— 空闲链表指针，**仅空闲块有效**
- `flags` —— 空闲标志位

块头大小被向上对齐到 `ALIGNMENT`，因为所有块大小都是 16 的倍数、段起点也按 16 对齐，
所以块头天然对齐，进而保证用户指针天然对齐。

### 堆段（arena）

分配器不假设"program break 紧跟自己上次的结尾"。`brk` 是进程级资源，
libc 的 `malloc` / `printf` 同样会用它扩展堆；一旦别的代码推进了 break，
按"上一段结尾 + 新块"的方式拼接就会覆盖别人的内存。

因此堆记录为若干段：

```
  段0                          段1                    段2
  ┌────────────────────┐ 间隙 ┌────────────────┐  间隙 ┌──────
  │ 块 块 块 块 ... 块 │      │ 块 块 ... 块   │       │ 块 ...
  └────────────────────┘      └────────────────┘       └──────
  ↑                    ↑      ↑                ↑
  begin                end    begin            end
```

- 段内块首尾相接，可以前向/后向合并
- 跨段的块互不相邻，绝不合并
- `find_arena()` 用二分之外的线性扫描判断地址归属；`block_next()` / `block_prev()` 都先过这道检查，
  段边界之外一律返回 `NULL`，从根本上杜绝越界访问

全局状态只有四个变量：段表 `g_arenas` / `g_arena_count`、最新一段的最后一块 `g_heap_tail`、空闲链表头 `g_free_list`。
分配器不可重入，也不线程安全。

## 三个核心流程

### 分配 `my_malloc`

```
align_up(size)                    ← 用户请求向上取整到 16 的倍数
    │
    ├─ 遍历 g_free_list（first-fit）
    │     找到 size 足够大的空闲块
    │       ├─ 从链表摘除
    │       ├─ split_block：切成 need + 尾部残余
    │       │     残余作为新的空闲块插回链表，并与后继空闲块合并
    │       └─ 返回 user_ptr
    │
    └─ 空闲链表无合适块 → grow_heap
           ├─ sbrk(0) 取当前 break
           ├─ break 是否紧邻上一段结尾？
           │    是 → 直接续在上一段末尾（extend）
           │    否 → 开新段，并额外 sbrk 出对齐填充（一次调用完成，避免两次之间的竞态）
           ├─ 在新位置构造块头，更新段表与 g_heap_tail
           └─ 返回 user_ptr
```

关键细节：

- `split_block` 只在残余部分足够容纳"块头 + `MIN_SPLIT`"时才分裂，否则整块给出，避免产生无法再分配的小碎片
- 分裂后必须修正**后继块**的 `prev_size`（它原本指向前一个整块，现在紧邻的是残余块），
  否则将来向低地址合并会算出越界地址
- 只有 `grow_heap` 得到的块会立即返回；从空闲链表取出的块保证已经对齐

### 释放 `my_free`

```
live_header(ptr)                  ← 四重校验，任一不通过就直接返回（不崩溃）
    ├─ 指针非空且按 16 对齐
    ├─ 块头也按 16 对齐，且确实落在某个段内
    └─ magic 正确，且当前不是空闲块（拒绝重复释放）
    │
    ├─ 标记为空闲
    ├─ merge_with_prev：前一块空闲 → 摘出链表，合并，修正新后继的 prev_size
    ├─ merge_with_next：后一块空闲 → 同上
    └─ 插回空闲链表头部
```

`merge_*` 的前提约定：调用时 `h` 已标记 `BLOCK_FREE` 但**尚未进入空闲链表**，
所以这两个函数只需负责把邻居从链表里摘出来，合并完成后由 `my_free` 统一插入一次。

野指针、未对齐指针、重复释放都被静默忽略——宁可泄漏一次，也不破坏整条块链。

### 重分配 `my_realloc`

按代价从低到高依次尝试：

```
1. need <= 当前 size
      split_block 削减尾部 → 原地址返回（原地收缩）

2. need > 当前 size，但 size + 紧邻后继空闲块 足够
      merge_with_next 吃掉后继 → split_block 切到 need → 原地址返回（原地扩容）

3. 都不行
      my_malloc 新块 → memcpy 拷贝 min(旧 size, 新 size) → my_free 旧块
      新块分配失败时，旧块保持原样不动，调用者仍拥有它
```

拷贝长度取 `min(h->size, size)`，而不是直接拷 `size`，避免读取旧块之外的字节。

## 调试与校验

`allocator_validate()` 遍历每一段里的每个块，检查以下不变量，任一违反即返回对应的错误码：

| 不变量 | 错误码 |
|--------|--------|
| 段首尾非空且 `begin < end`，段之间不重叠 | `ERR_HEAP_TRUNCATED` |
| 段起点与用户指针都按 16 对齐 | `ERR_BAD_ALIGN` |
| 块头 magic 正确 | `ERR_BAD_MAGIC` |
| 块大小非 0 且按 16 对齐 | `ERR_BAD_SIZE` |
| `prev_size` 与前一块实际大小一致 | `ERR_BAD_PREV_SIZE` |
| 块不越出段尾，且整段能被块严格铺满 | `ERR_BLOCK_OVERRUN` / `ERR_HEAP_TRUNCATED` |
| 不存在两个物理相邻的空闲块 | `ERR_ADJACENT_FREE` |
| 已分配块不携带链表指针 | `ERR_STRAY_LINK` |
| 空闲链表无环、双向一致、且元素数恰好等于堆内空闲块数 | `ERR_FREE_LIST` |

`allocator_get_stats()` 遍历全堆汇总堆字节数、已分配/空闲字节数与块数，供压力测试对拍使用。

## 已知约束

- **不线程安全**：全局状态无任何保护，多线程需调用方自行加锁
- **不要与 libc malloc 交替使用**：libc 可能 trim 顶层 chunk，把 break 收缩到本分配器段内，
  使段表记录的地址范围失效。推荐整体替换 malloc
- **无合并跨段、无回收段**：一旦 `sbrk` 扩展出来就不会归还给系统
- **段数上限 64**：`MAX_ARENAS`，超出后 `my_malloc` 返回 `NULL`
- **无 mmap 后备**：大块分配同样走 `sbrk`
