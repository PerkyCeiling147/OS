# Lab2 报告：第一棒 A

## 一、分工与实验环境

第一棒负责物理内存管理的基础闭环：建立 WSL 交叉编译与 QEMU 环境，实现
`page_init()`，新增 Buddy System，并完成 Challenge、报告与交接。

目标环境如下：

| 组件 | 要求 | 用途 |
|---|---|---|
| WSL | Ubuntu，可执行 Linux 命令 | 编译与运行 |
| `riscv64-unknown-elf-gcc` | RISC-V 64 位交叉编译器 | 编译内核 |
| `qemu-system-riscv64` | 带 RISC-V `virt` 机型 | 启动内核 |
| `make`、`python3` | 构建与判分依赖 | `make qemu` / `make grade` |
| `gdb-multiarch` | 可由 `riscv64-unknown-elf-gdb` 软链接调用 | 下一棒调试 |

环境自检命令：

```bash
command -v riscv64-unknown-elf-gcc qemu-system-riscv64 make python3
command -v gdb-multiarch riscv64-unknown-elf-gdb
riscv64-unknown-elf-gcc --version
qemu-system-riscv64 --version
```

若只有 `gdb-multiarch`：

```bash
sudo ln -s "$(command -v gdb-multiarch)" /usr/local/bin/riscv64-unknown-elf-gdb
```

## 二、模块一：物理内存初始化

**负责人：** 成员 A

**修改位置：** `kern/mm/pmm.c` 的 `page_init()`

**接口：**

```c
static void page_init(void);
static void init_memmap(struct Page *base, size_t n);
void pmm_init(void);
```

### 2.1 Prompt 原文

本实验第一棒要求中的原始提示词节选如下：

> 实现 page_init()，按 8 条验收标准调通（make qemu 看 DTB 范围 +
> check_alloc_page() succeeded!，必要时用 make grade 看 Test 1）。

### 2.2 实现思路

1. 在 `dtb_init()` 已保存的 `memory_base`、`memory_size` 基础上得到物理内存
   区间 `[mem_base, mem_end)`；区间无效时直接 panic，避免后续无符号下溢。
2. 设置 `va_pa_offset = PHYSICAL_MEMORY_OFFSET`，使 `PADDR`、`page2pa`、
   `pa2page` 在同一虚实映射假设下工作。
3. 令 `npage = PPN(mem_end)`。Page 描述符数组从内核 `end` 之后按页对齐的虚拟
   地址开始，覆盖页号 `[nbase, npage)`，共有 `npage - nbase` 个描述符。
4. 先把全部描述符初始化为 `PG_reserved`。这会把 OpenSBI、内核镜像和描述符
   数组自身排除在可分配范围之外。
5. 可分配区从描述符数组的物理末端向上取整开始，到 DTB 报告的物理内存末端的
   完整页为止。把这个区间交给当前 `pmm_manager->init_memmap()`。
6. 最后执行当前管理器的 `check()`；通过后打印
   `check_alloc_page() succeeded!`。

核心边界关系为：

```text
mem_base                          DTB memory 起点，QEMU 为 0x80000000
0x80200000                        OpenSBI 之后、内核镜像起点
ROUNDUP(end, PGSIZE)              Page 描述符数组起点
ROUNDUP(descriptors_end, PGSIZE)  可分配内存起点
ROUNDDOWN(mem_end, PGSIZE)        可分配内存终点
```

### 2.3 八条验收标准对照

| 编号 | 验收条件 | 实现对应 |
|---:|---|---|
| 1 | `va_pa_offset == PHYSICAL_MEMORY_OFFSET` | 进入地址换算前赋值 |
| 2 | DTB 内存 base/size 非零 | 无效范围直接 panic |
| 3 | `npage == PPN(mem_base + mem_size)` 且大于 `nbase` | 使用 DTB 末端计算并检查下溢 |
| 4 | `pages` 非空、页对齐、位于内核 `end` 后 | 从 `ROUNDUP(end, PGSIZE)` 开始 |
| 5 | 空闲页数非零且不超过描述符总数 | 只把描述符数组之后的完整页交给 PMM |
| 6 | 内核尾页保持 reserved | 初始化时先全部置 reserved |
| 7 | 描述符数组占用的页保持 reserved | 可分配区从数组物理末端之后开始 |
| 8 | `page2pa(pa2page(x))` 往返一致 | `nbase`、`pages`、`npage` 使用同一页号基准 |

此外，`best_fit_pmm.c` 中补齐了框架测试直接引用的内部状态：

```c
static free_area_t free_area;
#define free_list (free_area.free_list)
#define nr_free (free_area.nr_free)
```

它先消除了 `free_list` / `nr_free` 未定义导致的编译门错误；Best-Fit 管理器
本身由第二棒实现。

### 2.4 验证记录

- 源码按上述 8 条检查逐项核对，页号、虚拟地址和物理地址没有混用。
- `pmm.c`、`best_fit_pmm.c`、`buddy_pmm.c` 已通过宿主机 GCC 语法检查。
- 附件中的启动页表同时包含恒等映射项 2 和高地址映射项 511，避免开启
  `satp` 后仍按物理 PC 取指时发生缺页。
- 当前自动化会话被系统拒绝访问 WSL 服务，返回 `E_ACCESSDENIED`，因此本报告
  不把本次会话未重新执行的 `make qemu` 冒充为实测结果。交付前仍需在实际
  WSL 中按 `HANDOVER.md` 的 B 首日清单复跑并保存终端截图。

## 三、任务三：Buddy System

**新增文件：** `kern/mm/buddy_pmm.c`、`kern/mm/buddy_pmm.h`

**管理器名称：** `buddy_pmm_manager`

### 3.1 两份提示词

提示词一（实现）：

> 在新建的 kern/mm/buddy_pmm.c 和 buddy_pmm.h 中实现基于伙伴系统的
> pmm_manager。MAX_ORDER=15，每个 order 一条空闲链表；Page.property 保存
> order，free_area[order].nr_free 保存该阶空闲块数。alloc_pages(n) 向上取整到
> 2^k，必要时分裂；free_pages 按伙伴页号 XOR 判定并逐级合并。所有页号必须
> 检查描述符数组边界，最后注册 buddy_pmm_manager，并保留一种可单独切换验证
> 的入口。

提示词二（测试与纠错）：

> 为上述 Buddy 实现设计自检，覆盖单页、2 的幂、非 2 的幂取整、分裂后的剩余
> 块、相邻但非伙伴不合并、多级连续合并、页耗尽、区间不重叠，以及
> sum(块数×2^order)=nr_free_pages() 的一致性。测试结束后必须恢复原空闲页数；
> 若发现块对齐或合并错误，先定位是 init_memmap 的分解、split 的高半块还是
> free 的伙伴判定问题，再给出通过结果。

### 3.2 数据结构与算法

```c
#define MAX_ORDER 15
static free_area_t free_area[MAX_ORDER + 1];
```

order 为 `i` 的块包含 `2^i` 页。`Page.property` 保存 order，而不是页数；
`free_area[i].nr_free` 保存块数，而不是页数。因此：

```text
空闲页数 = sum(free_area[i].nr_free << i)
```

首次建池时，把 `[base, base+n)` 按页号自然对齐拆成若干 2 的幂块。分配时从
所需 order 向上找到第一个非空链表，依次把高半块挂回低一阶链表，最后返回
低半块。释放时先找到同阶伙伴：

```text
buddy_ppn = ppn XOR (1 << order)
```

只有伙伴页号在描述符数组范围内，且伙伴是同阶空闲块首页时才能合并。合并后的
首页取两个伙伴中地址较小者，再继续向更高阶合并。

### 3.3 测试覆盖

| 类别 | 覆盖内容 |
|---|---|
| 分配 | 1 页、2 页、3 页、5 页；验证向上取整和返回块对齐 |
| 分裂 | 从 order 4 块取 1 页后，剩余高半块必须依次落到 order 3/2/1/0 |
| 非伙伴 | 地址相邻但 XOR 条件不满足的页不得合并 |
| 合并 | 成对释放后连续从 order 0 合并到 order 4 |
| 耗尽 | 逐页拿空后返回 NULL，不重复分配，`nr_free_pages() == 0` |
| 不重叠 | 同时分配 8 个不同大小块，所有占用区间两两不相交 |
| 一致性 | 链表长度、块首 property、对齐、已合并性和空闲页总数逐步校验 |
| 恢复 | 测试结束空闲池恢复到初始页数 |

本次实际执行的宿主级验证使用 32768 页的空闲池，调用原
`buddy_pmm.c` 中的 `buddy_pmm_manager->check()`，得到：

```text
buddy_check: passed (32768 free pages)
host buddy check: PASS
```

这说明分配、分裂、伙伴合并与数据一致性逻辑可执行通过。最终内核级验收仍需在
WSL 中临时把 `init_pmm_manager()` 指向 `&buddy_pmm_manager`，看到
`memory management: buddy_pmm_manager` 和 `buddy_check: passed` 后，再切回
`&default_pmm_manager` 交给第二棒。

## 四、Challenge：操作系统如何自行获取可用物理内存范围

操作系统本身不能凭经验假定所有 RAM 都从固定地址开始、长度固定为 128 MiB。
尤其在同一内核支持不同开发板或虚拟机配置时，错误假定会把设备 MMIO 或固件
区域当成普通内存。常见信息来源和作用如下：

1. **固件传递的 DTB**。本实验的 OpenSBI 通过 `a1` 把 DTB 物理地址交给内核。
   DTB 的 `/memory` 节点含 `device_type = "memory"` 和 `reg = <base size>`；
   若机器有多个非连续 RAM 区间，`reg` 可以包含多组 base/size。内核应遍历全部
   区间，而不是只取第一组。
2. **DTB 的保留内存表**。`/memreserve/` 和 `/reserved-memory` 描述固件、安全
   监控器、共享内存等不可分配区域。物理区间检测后还必须先标记这些区域为
   reserved。
3. **固件/UEFI 内存描述符**。在 UEFI 或 ACPI 平台，固件通过
   `GetMemoryMap`、EFI memory map 或 ACPI 表报告 `Available`、
   `Reserved`、`ACPI Reclaimable` 等区域，顺序与属性比单一内存大小更重要。
4. **设备树以外还有设备枚举**。PCI BAR 等 MMIO 地址不能因为落在地址空间内
   就被认作 RAM；可用内存只能来自平台明确声明的 RAM 区域。

本实验的处理顺序是“先发现、再去重、再扣除”：

```text
OpenSBI 把 DTB PA 放入 a1
  -> entry.S 保存 boot_dtb
  -> dtb_init() 更新对齐后解析 memory/reg
  -> page_init() 建立覆盖全部物理页的描述符
  -> 全部页先标为 reserved
  -> 只把描述符数组之后到 RAM 末端的完整页交给 PMM
```

让所有描述符先 reserved，再只“认领”真正可分配的页，比先假定整段内存空闲、
再逐个补扣异常区更安全：漏扣一个区域只损失可用内存，而漏标一个已用区域则会
导致内核写入固件或设备内存。更完整的通用实现还应解析所有 `/memory` 区间和
DTB 保留表，并在建立伙伴系统前剔除内存空洞。

## 五、整体逻辑

本章从启动到物理页管理形成一条闭环：

```text
OpenSBI 跳转内核并传入 hartid、DTB
  -> entry.S 建启动页表，打开 Sv39
  -> dtb_init 获取物理内存范围
  -> pmm_init 选择 pmm_manager
  -> page_init 建 Page 描述符并初始化空闲池
  -> alloc/free_pages 统一通过函数指针分派
  -> check_alloc_page 验证分配器
```

`struct pmm_manager` 把“内存管理策略”和“物理内存发现/地址换算”分开。
`page_init()` 只负责给出正确的初始空闲区间，First-Fit、Best-Fit 或 Buddy 各自
维护内部数据结构。这样第二棒只需替换管理器实现并切换注册指针，无需重写启动
内存发现逻辑。

第一棒的关键边界是“哪些页永远不能分出去”。只有先确定这个边界，后续任何块
分配算法的正确性才有意义。Buddy 在这里用于验证页级管理策略可以独立接入，
但第一棒最后仍切回 `default_pmm_manager`，为第二棒实现 Best-Fit 保留清晰基线。

## 六、小结

第一棒完成了从固件内存描述到内核 Page 数组的映射，并实现了可直接自测的页级
Buddy 管理器。最重要的经验是：物理地址、内核虚拟地址、页号和描述符下标必须
各自沿正确路径转换；任何一次混用都会在页表或描述符边界上放大成随机写坏内存。
