# Lab2 报告：第二棒 B

## 一、任务范围

第二棒负责两套不同层次的分配器：

1. 在 `kern/mm/best_fit_pmm.c` 中把第一棒留下的桩替换为完整 Best-Fit
   物理页管理器，并让内核默认使用它。
2. 新增 `kern/mm/slub.c`、`kern/mm/slub.h`，在物理页管理之上建立对象级
   SLUB 缓存。

Best-Fit 回答“连续若干物理页从哪里找”；SLUB 回答“把页再切成固定大小对象，
如何快速复用”。两者构成页级分配和对象级分配的两层结构。

## 二、任务二：Best-Fit

**负责人：** 成员 B

**修改位置：**

- `kern/mm/best_fit_pmm.c`
- `kern/mm/pmm.c` 中的 `init_pmm_manager()`

### 2.1 Prompt

> 精读 kern/mm/default_pmm.c 的 init、init_memmap、alloc_pages、
> free_pages。保留 best_fit_pmm.c 中 basic_check 和 best_fit_check 不改，也不改
> free_area/free_list/nr_free 这三个测试依赖的标识符。实现完整 Best-Fit：
> 空闲链表按页地址排序，alloc_pages 扫描并选择 property 最小但不少于 n 的块；
> 余块从已选块尾部拆出并保持有序；释放时按地址插入，与前后物理相邻块合并，
> 每页只作为空闲块首页时置 PG_property。最后注册 best_fit_pmm_manager，并把
> init_pmm_manager 切换到它。

### 2.2 内部状态

框架测试直接引用以下状态，因此 B 没有改动名字：

```c
static free_area_t free_area;
#define free_list (free_area.free_list)
#define nr_free (free_area.nr_free)
```

`Page.property` 表示该空闲块包含的页数；只有块首页设置 `PG_property`。
`nr_free` 与所有空闲块页数之和相等。

### 2.3 分配与释放

`best_fit_alloc_pages(n)`：

1. 若 `n > nr_free`，立即返回 NULL。
2. 扫描按地址排序的空闲链表，在 `property >= n` 的块中选择 `property` 最小者。
3. 大小相同时保留地址较小者，行为稳定。
4. 整块大于 n 时，从块尾部拆出剩余部分；尾部地址大于原块，因此可挂回原块
   之前的链表位置并保持地址有序。
5. 分配块首页清除 `PG_property`，空闲页计数减去 n。

`best_fit_free_pages(base, n)`：

1. 检查归还区间不是 reserved，且每页都不是空闲块首页。
2. 清除页状态，把 `[base, base+n)` 作为新的空闲块按地址插入。
3. 若前一个空闲块正好结束于 `base`，合并。
4. 若合并后的块正好结束于后一个空闲块，再向后合并。
5. 被吸收块的 `PG_property` 被清除，避免同一页被识别为两个块。

### 2.4 测试覆盖

保留的 `basic_check()` 和 `best_fit_check()` 覆盖：

- 三个单页分配互不相同；
- 空闲链表被清空后必须分配失败；
- 1/2/3 页大小请求和地址顺序；
- 5 页分配后拆分与释放；
- Best-Fit 在多个候选块中选择最小足够块；
- 合并前后 `PageProperty`、`property` 和空闲页计数一致；
- 检查结束恢复原链表和原空闲页数。

宿主机直接用 4096 页的空闲池执行原 `best_fit_pmm_manager->check()`，结果通过；
随后执行了 20000 步确定性随机分配/释放压力测试，检查每次分配区间不越界、不与
仍存活区间重叠，并在最后恢复全部空闲页。管理器已经切换为：

```c
pmm_manager = &best_fit_pmm_manager;
```

因此官方 Test 2 应能看到 `memory management: best_fit_pmm_manager` 和
`[Test 2] Best-Fit: PASS`。

## 三、First-Fit 思考

First-Fit 从链表头开始，选择第一个大小足够的空闲块。若链表按地址排序，它会优先
使用低地址空洞，实现简单、通常扫描次数较少；但前面的中等块可能被反复切开，
留下许多小空洞。Best-Fit 要扫描完整链表，选择剩余空间最小的块，常能减少“大块
被小请求过早切碎”的情况，却也可能产生很多几乎无法使用的小尾块，并且每次分配
都要承担更多比较开销。

两者都只能直接合并物理相邻空闲块。模拟器里的页分配请求次数不多，Best-Fit 的
额外扫描成本可以接受；若分配请求很多，应结合按大小分桶、分离空闲链表或伙伴
系统降低查找成本。

## 四、任务四：SLUB 对象分配器

**新增文件：** `kern/mm/slub.c`、`kern/mm/slub.h`

### 4.1 两份 Prompt

Prompt 1（架构与实现）：

> 新建 kern/mm/slub.c 和 slub.h，在现有 pmm_manager 之上设计两层 SLUB：
> 下层每次向 Best-Fit 申请完整页，上层把一个页切成固定大小对象；对象自由链表
> 直接链接在空闲对象内部，避免每个对象保存额外链表节点。提供
> kmem_cache_create/alloc/free、kmalloc/kfree，缓存惰性创建，支持 16 到 4096
> 字节的 2 的幂大小类；缓存和 slab 描述符使用内核静态数组，不引入 malloc。

Prompt 2（测试与边界）：

> 为上述 SLUB 编写可执行自检，至少覆盖单 slab 与多 slab 分配、对象地址不重叠、
> 释放后复用、部分空闲和完全占用、通用 kmalloc/kfree、额外的全空 slab 归还给
> 页分配器，以及测试前后空闲物理页数恢复。还要验证对象对齐、对象属于正确
> cache、越界页/错误 cache 和重复释放会被断言拦截。测试不能改变 Best-Fit
> 管理器或官方 grading 行为。

### 4.2 两层结构

```text
Best-Fit pmm_manager
    └── alloc_pages(1) 获得一个 4 KiB 物理页
          └── SLUB cache
                ├── object_size 对齐到 16 字节
                ├── slab 描述符记录 page/free_list/free_count
                └── 页内对象通过单向 free_list 复用
```

下层的连续性、保留页和伙伴/空洞问题由 PMM 处理；上层只把拿到的页视为一段连续
虚拟内存，按固定对象大小切分。对象空闲时，其首地址直接存放“下一个空闲对象”
的指针，因此没有额外逐对象元数据开销。

### 4.3 缓存与 slab 生命周期

- `kmem_cache_create()` 把对象大小向上对齐到 16 字节，并计算
  `objects_per_slab = PGSIZE / object_size`。缓存首次分配时才真正取页。
- 新 slab 初始化出一条完整对象自由链表，并挂到 cache 的 slab 列表。
- 分配从第一个还有空闲对象的 slab 弹出自由链表头。
- 释放把对象压回链表；描述符中的 `free_count` 用于判断是否变为空 slab。
- cache 保留一个空 slab 作为预留，额外全空 slab 立即归还 Best-Fit，避免空闲页
  长期被空缓存占用。
- `kmalloc/kfree` 在 16、32、…、4096 字节大小类上选择缓存；`kfree` 通过对象
  所属物理页反查 slab 和 cache。

### 4.4 测试覆盖

| 测试项 | 说明 |
|---|---|
| 多 slab | 64 字节对象分配 80 个，超过一个页的容量，必须创建至少 2 个 slab |
| 不重叠 | 80 个同时存活对象两两比较地址，不能重复 |
| 对齐 | 所有返回对象地址按 16 字节对齐 |
| 释放与复用 | 释放一半对象后重新申请，验证自由链表还能继续服务 |
| 部分/完整占用 | 分配直到已有 slab 无空位，再触发新页 |
| 空 slab 回收 | 全部释放后只保留一个空 slab，额外 slab 的物理页归还 PMM |
| 通用接口 | `kmalloc(1..3008)` 与 `kfree` 覆盖多个大小类 |
| 页计数恢复 | 清空所有测试缓存后 `nr_free_pages()` 回到测试前数值 |
| 边界保护 | 越界对象、cache 不匹配、重复释放通过断言拒绝 |

测试实现挂在：

```c
#ifdef ucore_test
    slub_check();
#endif
```

普通 `make` 和官方 `make grade` 不会运行 SLUB 自检，因此不改变评分输出；
`make test` 使用 `-D ucore_test`，会额外打印 `slub_check: passed`。

本次宿主机用 64 页模拟物理内存、Best-Fit 作为下层管理器，执行了原
`slub_check()`：

```text
slub_check: passed (80 object cache test)
host slub check: PASS
```

同时，原 `best_fit_pmm_manager->check()` 在 4096 页宿主池上通过。当前会话仍因
WSL 服务访问被拒，无法在内核目标上重新执行 `make test` 和 `make grade`；接手
的第三棒必须把它作为首个验收项。

## 五、知识点与操作系统原理对照

| 本实验知识点 | OS 原理中的对应概念 | 关系与差异 |
|---|---|---|
| Best-Fit 空闲链表 | 连续内存分配、动态分区、空闲链表 | 都维护空闲区，但本实验单位是 4 KiB 页，通用分配器通常以字节为单位 |
| `Page.property` 块长度 | 分区描述符/边界标记 | 本实验只在首页记录长度；通用系统常用边界标记支持双向合并 |
| 物理页 `alloc/free` | 页框分配器、物理内存管理 | 本实验尚未建立虚拟内存对象，只管理系统物理页 |
| Buddy 分裂/合并 | 伙伴系统 | 本实验直接使用页号 XOR 找伙伴，真实系统还要并发锁、迁移和 NUMA |
| SLUB cache | slab/slub 对象分配器、内核堆 | 思路一致：从页中切对象并维护 per-cache freelist；本实验没有 per-CPU 缓存和复杂着色 |
| `kmalloc/kfree` | 内核动态内存分配 | 本实验只支持固定大小类；真实 `kmalloc` 还要处理大对象、回收与调试元数据 |
| 双层分配 | 页分配器 + 对象分配器 | 小对象避免按整页浪费，页分配器负责底层连续空间 |

## 六、本实验未覆盖但重要的 OS 知识点

1. 并发安全的空闲链表和 slab 操作。真实内核有 per-CPU cache、无锁/自旋锁、
   中断上下文和跨 CPU 释放，本实验单核且运行时关闭了这些竞争。
2. NUMA、NUMA-aware 分配和内存热插拔。
3. 物理内存热区、内存空洞、多内存节点和保留区域完整解析。
4. 大对象复合页、页回收、swap、内存 cgroup 和 OOM 策略。
5. slab 对象析构函数、slab 合并、cache 着色、redzone、poisoning、KASAN 和
   内存泄漏追踪。
6. Best-Fit 的索引结构优化，例如按大小分桶、分离链表和 O(1) 近似查找。
7. 虚拟内存缺页、按需分页和用户态堆分配；本实验只做到物理页/内核对象层。

## 七、小结

Best-Fit 在页级解决“选哪一个连续块”的问题，SLUB 在对象级解决“页内如何切分和
复用”的问题。两层都依赖同一组基本不变量：不会分配同一块两次、元数据与链表
一致、测试后资源能够完整归还。B 棒的代码已经冻结，下一步应由 C 在有调试权限
的 WSL 环境中重新跑通 `make test`、`make grade` 并进行 QEMU 地址翻译观察。
