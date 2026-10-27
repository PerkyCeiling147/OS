# Lab2 报告：第三棒 C —— QEMU MMU 调试（分支任务）

**负责人：** 2413635-曹紫煜

## 一、任务、环境与结论

### 1.1 本棒任务

1. 复验 A+B 基线：`make test` 通过、`make grade` 为 **20/20**。
2. 完成 lab2 分支任务：用 **guest GDB + host GDB** 双重调试，观察一次真实访存的
   TLB miss、Sv39 页表翻译和 TLB 回填。
3. 回答分支任务的六个问题，写入本文件。
4. 合并 A/B/C 三份报告，替换代码中的 `YOUR CODE`，做最终判分。

### 1.2 实际环境

| 组件 | 实际版本 / 说明 |
|---|---|
| WSL | Ubuntu 24.04.1 LTS，内核 6.6.114.1-microsoft-standard-WSL2 |
| 交叉编译器 | `riscv64-unknown-elf-gcc` 13.2.0 |
| QEMU | `qemu-system-riscv64` 8.2.2（Ubuntu `1:8.2.2+ds-0ubuntu1.18`） |
| 宿主机 GDB | GNU gdb 15.1（x86-64，调试 QEMU 进程） |
| guest GDB | `gdb-multiarch` 15.1（RISC-V，连接 QEMU gdbstub `:1234`） |
| QEMU 调试符号 | Ubuntu ddebs 的 `qemu-system-misc-dbgsym_8.2.2+ds-0ubuntu1.18_amd64.ddeb` |
| 阅读用源码 | QEMU upstream `v8.2.2`（`target/riscv/cpu_helper.c`、`accel/tcg/cputlb.c`） |

> **版本说明（不隐瞒差异）：** 交接文档建议自己编译 QEMU 10.2.1 调试版；本机
> WSL 源里只有 8.2.2，且完整编译一次要较长时间。这里采用交接文档允许的
> “替代方案”：安装与系统 QEMU **完全同版本**的 dbgsym 包，用 host GDB 直接
> 调试 `/usr/bin/qemu-system-riscv64`。MMU 的关键路径
> （`riscv_cpu_tlb_fill → get_physical_address → tlb_set_page_full`）在 8.2.2
> 与 10.x 中结构一致，差别主要是函数参数和行号；报告中的行号均以本机实际
> 命中的 8.2.2 调试信息为准。
>
> **关于地址的一个细节：** 双 GDB 实验用的是普通 `make` 构建的 `bin/kernel`
> （不带 `-D ucore_test`），此时 `pages = 0xffffffffc0207000`。最终
> `make test` 会编进 `tools/test_lab2.c`，内核变大，`pages` 随之后移到
> `0xffffffffc0208000`。两者翻译逻辑完全相同，只是页描述符数组的起始地址
> 不同；本报告的双 GDB 日志均来自普通构建。

### 1.3 结论摘要

- 基线复验：`make test` → `grading: 6 / 6 points`、`slub_check: passed`；
  `make grade` → **Total Score: 20/20**。
- 抓到一次 `sw zero,16(a5)` 的 **数据写 TLB miss**：
  `VA=0xffffffffc0207010 → PA=0x80207010`，`prot=7`，`mmu_idx=1`。
- Sv39 页表项为 `0x200000cf`，位于根页表 `PTE[511]`，是一个 **1 GiB 大页叶子**；
  因此源码里的三层 `for` 循环在本例只执行了第一层。报告同时给出完整三层走法。
- TLB 回填在 `tlb_set_page_full()` 完成，实际填入
  `phys_addr=0x80207000`、`prot=7`、`lg_page_size=12`（4 KiB）。
- 对照抓到开分页前的裸模式访问：`satp=0`、`VA=0x80200000`、取指；
  开分页后 `satp=0x8000000000080205`、高地址 `VA` 翻译到 `PA`。

---

## 二、双重 GDB 的操作方法

### 2.1 三个终端的分工

| 终端 | 角色 | 作用 |
|---|---|---|
| 1 | QEMU | 运行 `qemu-system-riscv64`，`-s -S` 打开 gdbstub 并冻结 CPU |
| 2 | guest GDB | 连 `:1234`，按 RISC-V 内核符号（`bin/kernel`）停在内核代码 |
| 3 | host GDB | 调试 **QEMU 自己**（x86-64 进程），在 QEMU 的 C 源码里下断点 |

本次为了脚本化复现，host GDB 用 `run` 直接启动 QEMU（带 `-s -S`），因此实际是
“两个 GDB 窗口 + 一个由 host GDB 拉起的 QEMU 进程”。如果答辩时希望终端 1
单独跑 `make debug`，也可以在 host GDB 里 `attach <qemu-pid>`（WSL 下需要
允许 ptrace）；本报告的证据均来自已验证的 `run` 方式，不把 attach 方式写成
已验证结论。

### 2.2 启动顺序

```bash
# 终端 3：host GDB 先启动 QEMU（-S 冻结），并等待条件断点
cd /mnt/c/Users/华为/Documents/Qoder/2026-10-22/576d5e4e/lab2_new
gdb -q -x ../qemu-debug/host.gdb

# 终端 2：guest GDB 连上去，停到 pmm.c:132，再 continue
cd /mnt/c/Users/华为/Documents/Qoder/2026-10-22/576d5e4e/lab2_new
gdb-multiarch -q -x ../qemu-debug/guest.gdb
```

guest 脚本的关键内容：

```gdb
set pagination off
set confirm off
set remotetimeout unlimited
file bin/kernel
set architecture riscv:rv64
target remote localhost:1234
tbreak pmm.c:132
continue
printf "[GUEST] pc=0x%lx satp=0x%lx sp=0x%lx\n", $pc, $satp, $sp
printf "[GUEST] pages = 0x%lx, a5 = 0x%lx\n", pages, $a5
x/i $pc
printf "[GUEST] PTE[2]   = 0x%lx\n", *(unsigned long*)0xffffffffc0205010
printf "[GUEST] PTE[511] = 0x%lx\n", *(unsigned long*)0xffffffffc0205ff8
continue
```

`pmm.c:132` 是 `p->property = 0;`，GDB 停在**执行之前**；guest 一 `continue`，
QEMU 就需要为这条 `sw` 做地址翻译，host 侧正好接住 TLB miss。

### 2.3 host GDB 的关键断点

```gdb
set pagination off
set confirm off
set debuginfod enabled off
file /usr/bin/qemu-system-riscv64
symbol-file /mnt/c/.../qemu-debug/qemu-debug-patched.debug
directory /mnt/c/.../qemu-src
set substitute-path /usr/src/qemu-1:8.2.2+ds-0ubuntu1.18 /mnt/c/.../qemu-src
handle SIGPIPE nostop noprint

# 阶段 1：开分页前的裸模式访问
tbreak riscv_cpu_tlb_fill if address >= 0x80200000 && address < 0x80201000
# 阶段 2：pages[] 首页的写访问
tbreak riscv_cpu_tlb_fill if address >= 0xffffffffc0207000 && address < 0xffffffffc0208000

run -machine virt -display none -bios default -kernel bin/ucore.img \
    -s -S -serial file:/mnt/c/.../qemu-debug/qemu_serial.log

# ……阶段 1 打印后 continue……

# 阶段 2 停下后，再设置三个“翻译过程”临时断点
tbreak target/riscv/cpu_helper.c:970     # PTE 已装载
tbreak target/riscv/cpu_helper.c:1404    # get_physical_address 返回，准备回填 TLB
tbreak *tlb_set_page_full                # 组装 CPUTLBEntryFull
continue
```

> 为什么不是断 `tlb_set_page`？在 8.2.2 的优化构建里，`tlb_set_page()` 被
> 内联/尾调用进 `riscv_cpu_tlb_fill()`，断在它入口根本不触发；实际执行的是
> `tlb_set_page_full()`。这也是本次调试踩到的一个坑，详见第七节。

---

## 三、问题 1：QEMU 的关键调用路径、分支语句与一次 VA→PA

### 3.1 调用路径

TCG 生成的访存快路径在 TLB 命中时不进入任何 C 函数；只有 **TLB miss** 才走
慢路径：

```text
guest 指令 sw zero,16(a5)
  └─ TCG softmmu slow path
       └─ mmu_lookup1()                    accel/tcg/cputlb.c:1698
            ├─ tlb_hit(tlb_addr, addr)     快路径：命中则直接返回
            └─ tlb_fill()                  未命中：accel/tcg/cputlb.c:1306
                 └─ cpu->cc->tcg_ops->tlb_fill(...)       :1315
                      └─ riscv_cpu_tlb_fill()             target/riscv/cpu_helper.c:1290
                           ├─ get_physical_address()      target/riscv/cpu_helper.c:795
                           │    └─ Sv39 页表遍历（for 循环）
                           └─ tlb_set_page()              target/riscv/cpu_helper.c:1404
                                └─ tlb_set_page_with_attrs()   accel/tcg/cputlb.c:1278
                                     └─ tlb_set_page_full()    accel/tcg/cputlb.c:1113
```

host GDB 在本次实验中抓到的真实回栈（省略被优化掉的帧参数）：

```text
#0  riscv_cpu_tlb_fill (...) at target/riscv/cpu_helper.c:1293
#1  tlb_fill (...) at accel/tcg/cputlb.c:1315
#2  mmu_lookup1 (...) at accel/tcg/cputlb.c:1713
#3  ... code_gen_buffer
```

### 3.2 关键分支语句

**(1) TLB 命中判断与 miss 处理**（`accel/tcg/cputlb.c:1710`）：

```c
/* If the TLB entry is for a different page, reload and try again.  */
if (!tlb_hit(tlb_addr, addr)) {
    if (!victim_tlb_hit(cpu, mmu_idx, index, access_type,
                        addr & TARGET_PAGE_MASK)) {
        tlb_fill(cpu, addr, data->size, access_type, mmu_idx, ra);
        maybe_resized = true;
        ...
    }
    tlb_addr = tlb_read_idx(entry, access_type) & ~TLB_INVALID_MASK;
}
```

**(2) 翻译成功/失败分支**（`target/riscv/cpu_helper.c:1377`、`:1403`）：

```c
} else {
    /* Single stage lookup */
    ret = get_physical_address(env, &pa, &prot, address, NULL,
                               access_type, mmu_idx, true, false, false);
    ...
}

if (ret == TRANSLATE_SUCCESS) {
    tlb_set_page(cs, address & ~(tlb_size - 1), pa & ~(tlb_size - 1),
                 prot, mmu_idx, tlb_size);
    return true;
} else if (probe) {
    return false;
} else {
    raise_mmu_exception(env, address, access_type, pmp_violation,
                        first_stage_error, two_stage_lookup,
                        two_stage_indirect_error);
    cpu_loop_exit_restore(cs, retaddr);
}
```

**(3) 页表项合法性/叶子判断**（`target/riscv/cpu_helper.c:992`）：

```c
if (!(pte & PTE_V)) {
    /* Invalid PTE */
    return TRANSLATE_FAIL;
}
if (pte & (PTE_R | PTE_W | PTE_X)) {
    goto leaf;
}
/* Inner PTE, continue walking */
...
base = ppn << PGSHIFT;
```

### 3.3 一次真实访存的 VA→PA

guest 在 `pmm.c:132` 停下时，寄存器与变量如下（guest.log）：

```text
Temporary breakpoint 1, page_init () at kern/mm/pmm.c:132
132	        p->property = 0;

[GUEST] stopped before first store into pages[]
[GUEST] pc=0xffffffffc0201078 satp=0x8000000000080205 sp=0xffffffffc0204fa0
[GUEST] pages variable = 0xffffffffc0207000
[GUEST] a5 store base = 0xffffffffc0207000
=> 0xffffffffc0201078 <pmm_init+254>:	sw	zero,16(a5)
[GUEST] PTE[2]   = 0x200000cf
[GUEST] PTE[511] = 0x200000cf
```

因此这次写访问的**有效地址（EA）**为：

```text
EA = a5 + 16 = 0xffffffffc0207000 + 0x10 = 0xffffffffc0207010
```

host GDB 在 TLB miss 处抓到的正是这个地址：

```text
Thread 3 "qemu-system-ris" hit Breakpoint 1, riscv_cpu_tlb_fill
  (cs=..., address=18446744072637935632, size=4,
   access_type=MMU_DATA_STORE, mmu_idx=1, probe=false, retaddr=...)
  at target/riscv/cpu_helper.c:1293

[HOST] vaddr=0xffffffffc0207010 size=4 access_type=1 mmu_idx=1 probe=0
[HOST] satp = 0x8000000000080205
```

`address=18446744072637935632` 的十六进制就是 `0xffffffffc0207010`。
随后 host GDB 读到：

```text
[HOST] walk: pte_addr = 0x80205ff8
[HOST] walk: pte = 0x200000cf
[HOST] translate: pa = 0x80207010
[HOST] translate: prot = 7
```

最终 `VA 0xffffffffc0207010 → PA 0x80207010`。这不是根据页表“手算猜出来”的，
而是 QEMU `get_physical_address()` 返回、并在 `cpu_helper.c:1404` 处被 host GDB
直接读到的真实值；同一次 miss 的 PTE 与页表地址也一并抓到了。

### 3.4 地址分解

| 项目 | 值 | 说明 |
|---|---|---|
| 指令 | `sw zero,16(a5)` | 向 `a5+16` 写 4 字节 0 |
| `a5` | `0xffffffffc0207000` | `pages` 数组首地址（高地址虚拟地址） |
| 页内偏移 | `0x010` | 16 |
| VA | `0xffffffffc0207010` | 实际访存地址 |
| `satp` | `0x8000000000080205` | MODE=8（Sv39）、ASID=0、PPN=`0x80205` |
| 根页表物理地址 | `0x80205000` | `PPN << 12` |
| VPN[2] | `0x1ff`（511） | `(VA >> 30) & 0x1ff` |
| VPN[1] | `0x001` | `(VA >> 21) & 0x1ff` |
| VPN[0] | `0x007` | `(VA >> 12) & 0x1ff` |
| 根 PTE 地址 | `0x80205ff8` | `0x80205000 + 511*8` |
| PTE | `0x200000cf` | V/R/W/X/U/G/A/D 全为 1，PPN=`0x80000` |
| PA | `0x80207010` | 1 GiB 大页：`0x80000000 + (VA & 0x3fffffff)` |

---

## 四、问题 2：Sv39 页表翻译的关键步骤

### 4.1 `satp` 解码

```text
satp = 0x8000000000080205
       │        │       └─ PPN  = 0x80205  → 根页表 PA = 0x80205000
       │        └───────── ASID = 0
       └────────────────── MODE = 8      → Sv39，三级页表
```

Sv39 的 `levels = 3`、`ptidxbits = 9`、`ptesize = 8`（`cpu_helper.c:871`）。
每级用 9 位索引，页内偏移 12 位，因此一个叶子 PTE 最多可映射
`2^(12 + 9*(3-i-1))` 字节。

### 4.2 根页表项

VPN[2] = 511，所以根页表项地址为：

```text
0x80205000 + 511 * 8 = 0x80205ff8
```

host GDB 读到的正是这个地址和值：

```text
[HOST] walk: pte_addr = 0x80205ff8
[HOST] walk: pte = 0x200000cf
```

`0x200000cf` 的位分解：

| 位域 | 值 | 含义 |
|---|---|---|
| PPN[43:10] | `0x80000` | 物理页号 |
| D (bit 7) | 1 | Dirty |
| A (bit 6) | 1 | Accessed |
| G (bit 5) | 1 | Global |
| U (bit 4) | 1 | User |
| X (bit 3) | 1 | Executable |
| W (bit 2) | 1 | Writable |
| R (bit 1) | 1 | Readable |
| V (bit 0) | 1 | Valid |

`PPN = 0x80000` 对应的物理基址是 `0x80000 << 12 = 0x80000000`。

### 4.3 为什么本例只走了一层循环

源码的遍历是一个 `for (i = 0; i < levels; i++, ptshift -= ptidxbits)`
（`cpu_helper.c:922`）。每一层做四件事：

1. 从 VA 中取出 9 位索引 `idx`；
2. 算出 PTE 地址 `pte_addr = base + idx * ptesize`；
3. 用 `address_space_ldq()` 读出 PTE（`cpu_helper.c:967`）；
4. 若 `PTE_V=0` 失败；若 `R|W|X` 任一为 1，跳到 `leaf`；否则
   `base = ppn << PGSHIFT`，继续下一层（`cpu_helper.c:992-1004`）。

一般 Sv39 的 4 KiB 映射会走三层：

```text
i=0：根表 base=0x80205000，idx=VPN[2]=511，读根 PTE；
     非叶子 → base = 下一级表基址，ptshift: 18 → 9
i=1：二级表，idx=VPN[1]=1，读 PTE；
     非叶子 → base = 下一级表基址，ptshift: 9 → 0
i=2：三级表，idx=VPN[0]=7，读 PTE；
     叶子 → ptshift=0，得到 4 KiB 页
```

但本例根 PTE 已经是叶子：`pte & (R|W|X) != 0`，直接 `goto leaf`，所以
**循环只执行了 i=0 一次**。此时 `ptshift = (levels-1)*ptidxbits = 18`，叶子
覆盖 `2^(12+18) = 2^30 = 1 GiB`，是一个 1 GiB 大页。

`leaf` 处还会检查大页对齐：

```c
if (ppn & ((1ULL << ptshift) - 1)) {
    /* Misaligned PPN */
    return TRANSLATE_FAIL;
}
```

`PPN=0x80000`，`ptshift=18`，`0x80000 & 0x3ffff == 0`，对齐成立，因此这个
1 GiB 大页合法。

### 4.4 最终物理地址

`cpu_helper.c:1139` 的大页公式为：

```c
*physical = (((ppn & ~napot_mask) | (vpn & napot_mask) |
              (vpn & (((target_ulong)1 << ptshift) - 1))
             ) << PGSHIFT) | (addr & ~TARGET_PAGE_MASK);
```

本例没有 Svnapot（`napot_mask=0`），`ptshift=18`，所以：

```text
vpn        = VA >> 12
vpn & 0x3ffff = (VA >> 12) & 0x3ffff = 0x0207
physical   = (0x80000 | 0x0207) << 12 | (VA & 0xfff)
           = 0x80207000 | 0x010
           = 0x80207010
```

等价地，按 1 GiB 大页理解：

```text
PA = 0x80000000 + (0xffffffffc0207010 & 0x3fffffff)
   = 0x80000000 + 0x0207010
   = 0x80207010
```

这也解释了为什么 VPN[1]、VPN[0] 没有继续查表：它们变成了 1 GiB 页内的
偏移位。

### 4.5 启动页表的两个 1 GiB 映射

guest 读到的两个 PTE 都是 `0x200000cf`：

| 页表项 | 虚拟地址范围 | 物理地址范围 | 作用 |
|---|---|---|---|
| `PTE[2]` | `0x80000000`–`0xBFFFFFFF` | `0x80000000`–`0xBFFFFFFF` | 开启 `satp` 后，`entry.S` 仍按物理 PC 取指时的恒等映射 |
| `PTE[511]` | `0xFFFFFFFFC0000000`–`0xFFFFFFFFFFFFFFFF` | `0x80000000`–`0xBFFFFFFF` | 跳到 `KERNBASE` 后的内核高地址映射；本次 `sw` 用的就是它 |

`entry.S` 在 `csrw satp` 之后立刻执行 `sfence.vma`，然后 `jr` 到
`kern_init` 的虚拟地址；在这之前，取指仍使用物理地址，所以 `PTE[2]` 的恒等
映射不能省。这也是交接文档里“不要回退 entry.S 第 2 项”的原因。

---

## 五、问题 3：QEMU 模拟 TLB 查找的 C 代码

### 5.1 快路径：先查 TLB

`mmu_lookup1()`（`accel/tcg/cputlb.c:1698`）是 TCG 慢路径查表的入口：

```c
vaddr addr = data->addr;
uintptr_t index = tlb_index(cpu, mmu_idx, addr);
CPUTLBEntry *entry = tlb_entry(cpu, mmu_idx, addr);
uint64_t tlb_addr = tlb_read_idx(entry, access_type);

/* If the TLB entry is for a different page, reload and try again.  */
if (!tlb_hit(tlb_addr, addr)) {
    if (!victim_tlb_hit(cpu, mmu_idx, index, access_type,
                        addr & TARGET_PAGE_MASK)) {
        tlb_fill(cpu, addr, data->size, access_type, mmu_idx, ra);
        ...
    }
    tlb_addr = tlb_read_idx(entry, access_type) & ~TLB_INVALID_MASK;
}
```

要点：

- QEMU 的 TLB 按 `mmu_idx`（M/S/U 等地址空间）分开，每个 `mmu_idx` 内再按
  虚页号索引，并按访问类型（取指/读/写）保存不同的 tag/权限位。
- `tlb_hit(tlb_addr, addr)` 只比较“这个表项是否属于当前页”；命中就直接用
  表项里的 `addend` 算 host 地址，不进入任何页表遍历。
- 未命中时先尝试 `victim_tlb_hit()`（被替换表项的二次机会），还没有才调用
  `tlb_fill()`。
- `tlb_fill()` 再通过 `cpu->cc->tcg_ops->tlb_fill` 分派到具体架构的
  `riscv_cpu_tlb_fill()`（`cputlb.c:1315`）。

### 5.2 慢路径：查页表并回填

`riscv_cpu_tlb_fill()` 在单级翻译时调用
`get_physical_address()`（`cpu_helper.c:1377`），成功后：

```c
if (ret == TRANSLATE_SUCCESS) {
    tlb_set_page(cs, address & ~(tlb_size - 1), pa & ~(tlb_size - 1),
                 prot, mmu_idx, tlb_size);
    return true;
}
```

`tlb_set_page()` 依次调用 `tlb_set_page_with_attrs()` 和
`tlb_set_page_full()`，最终组装一个 `CPUTLBEntryFull` 并写入 TLB。

本次 host GDB 在 `tlb_set_page_full()` 入口读到的实际参数：

```text
[HOST] TLB fill entry: tlb_set_page_full (registers, before prologue)
[HOST] tlb_set_page_full: cpu=0x5555568f15c0 mmu_idx=1 vaddr=0xffffffffc0207000 full=0x7fffa7dfdd30
[HOST] full: xlat_section=0x0 phys_addr=0x80207000 attrs=0x1 prot=7 lg_page_size=12
```

字段含义（`CPUTLBEntryFull`，见 QEMU 8.2 的 `include/exec/cpu-defs.h`）：

| 字段 | 值 | 含义 |
|---|---|---|
| `phys_addr` | `0x80207000` | 页对齐后的物理地址 |
| `attrs` | `0x1` | 内存事务属性（普通 RAM） |
| `prot` | `7` | `PAGE_READ | PAGE_WRITE | PAGE_EXEC` |
| `lg_page_size` | `12` | 表项覆盖 `2^12 = 4 KiB` |

`tlb_set_page_full()` 随后用 `full->phys_addr` 和 TCG 的 `addend` 建立
“虚拟页 → host 地址”的快路径。下一次访问同一页时，`tlb_hit()` 命中，不再
进入 `riscv_cpu_tlb_fill()`。

### 5.3 一个容易忽略的细节：1 GiB PTE 却回填 4 KiB 表项

页表项是 1 GiB 大页，但 `lg_page_size=12`，即 QEMU 只回填了一个 4 KiB 的
TLB 表项。原因是 `riscv_cpu_tlb_fill()` 里的：

```c
tlb_size = pmp_get_tlb_size(env, pa);
```

而 `pmp_get_tlb_size()`（`target/riscv/pmp.c:627`）在“没有 PMP 规则”时直接
返回 `TARGET_PAGE_SIZE`：

```c
/*
 * If PMP is not supported or there are no PMP rules, the TLB page will not
 * be split into regions with different permissions by PMP so we set the
 * size to TARGET_PAGE_SIZE.
 */
if (!riscv_cpu_cfg(env)->pmp || !pmp_get_num_rules(env)) {
    return TARGET_PAGE_SIZE;
}
```

所以 QEMU 的 softmmu TLB 在这里按页粒度缓存，而不是按 1 GiB 大页缓存。
这是 QEMU 的实现选择，不是 RISC-V 规范的要求；真实 CPU 完全可以把整个
1 GiB 叶子缓存成一个大页 TLB 表项。第 6 节会展开对比。

### 5.4 页表/TLB 什么时候被刷新

- `entry.S` 的 `sfence.vma` 在 QEMU 中由 `helper_tlb_flush()` 处理
  （`target/riscv/op_helper.c:383`），最终调用 `tlb_flush()`。
- 写 `satp` 时，`write_satp()`（`target/riscv/csr.c:2997`）在分页模式或
  映射发生变化时也会 `tlb_flush()`（`csr.c:3022`）。
- `write_satp()` 的注释还特别说明：即使 `satp.MODE=Bare`，QEMU 也会把这些
  访问送进 TLB 仿真以获得性能；这与真实 CPU 在 Bare 模式下“没有地址翻译”
  的直觉不同，也是第 6 节要对比的点。

---

## 六、问题 4：QEMU 模拟 TLB 与真实 CPU TLB 的对比

### 6.1 分页前后的两组实测

**(a) 开分页前：Bare 模式**

host GDB 在 `0x80200000` 的第一次取指处抓到：

```text
[HOST] PHASE 1: before paging is enabled (bare mode)
[HOST] vaddr=0x80200000 access_type=2 mmu_idx=1 satp=0x0
```

`access_type=2` 是 `MMU_INST_FETCH`，`satp=0` 表示 Bare。此时
`get_physical_address()` 会走 `MODE == Bare` 的短路分支：

```c
if (mode == PRV_M || !riscv_cpu_cfg(env)->mmu) {
    *physical = addr;
    ...
}
...
case VM_1_10_MBARE:
    *physical = addr;
    ...
```

也就是说 `VA == PA`。QEMU 仍然为这次取指建立了一个 TLB 表项（否则每次取指
都要走慢路径），但翻译本身是恒等的。

**(b) 开分页后：Sv39**

同一次实验第二次命中条件断点：

```text
[HOST] PHASE 2: TLB miss -> riscv_cpu_tlb_fill
[HOST] vaddr=0xffffffffc0207010 size=4 access_type=1 mmu_idx=1 probe=0
[HOST] satp = 0x8000000000080205
[HOST] translate: pa = 0x80207010
[HOST] full: phys_addr=0x80207000 attrs=0x1 prot=7 lg_page_size=12
```

`access_type=1` 是 `MMU_DATA_STORE`。同一个内核、同一套 QEMU 代码，仅仅因为
`satp` 从 0 变成 `0x8000000000080205`，`0xffffffffc0207010` 就被翻译到了
`0x80207010`；而在 Bare 模式下 `0x80200000` 就是它自己。

### 6.2 对比表

| 维度 | QEMU TCG softmmu TLB | 真实 CPU TLB |
|---|---|---|
| 本质 | QEMU 为加速 JIT 代码访存而维护的软件表项 | MMU 内的硬件 CAM/SRAM |
| 查找 | `mmu_lookup1 → tlb_hit()` 比较 tag | 硬件并行比较，周期级完成 |
| 未命中 | 跳到 C 函数 `tlb_fill → riscv_cpu_tlb_fill`，软件查页表 | 硬件 page-table walker 自动查表；或 trap 到软件（如某些简化实现） |
| 页大小 | 本次 `lg_page_size=12`（4 KiB）；由 `pmp_get_tlb_size()` 决定 | 可缓存 4 KiB / 2 MiB / 1 GiB，取决于微架构和 PTE 大页 |
| 上下文区分 | `mmu_idx` 分开 M/S/U；ASID 由软件在 `satp` 中维护 | ASID/VMID 硬件标记，切换地址空间可避免全刷 |
| 刷新 | `sfence.vma → helper_tlb_flush → tlb_flush()`；写 `satp` 也可能刷新 | `sfence.vma` 直接刷新硬件 TLB |
| 权限 | `prot`/`attrs` 位由 `get_physical_address()` 计算 | PTE 的 R/W/X/U + PMP 由硬件检查 |
| 目的 | 让仿真跑得快；不追求周期精确 | 加速真实程序访存，必须严格符合 ISA |

### 6.3 关键差异的理解

1. **QEMU 的 TLB 是“翻译缓存”，不是“硬件 TLB 的周期级模型”。**
   QEMU 在 TLB miss 时用 C 代码查页表、更新 PTE 的 A/D 位、写回 TLB；
   真实 CPU 这些动作由硬件完成。把 QEMU 的 `riscv_cpu_tlb_fill()` 当成
   “MMU 的硬件行为”是概念混淆；它只是 QEMU 对硬件行为的**功能模拟**。
2. **大页的缓存粒度不同。** 本例 PTE 是 1 GiB 叶子，但 QEMU 只回填 4 KiB
   表项；真实 CPU 可能用一个 1 GiB TLB 表项覆盖整个大页，从而显著减少
   TLB 项数和 miss 次数。
3. **Bare 模式的语义不同。** 真实 CPU 在 `satp.MODE=Bare` 时没有地址翻译；
   QEMU 为了性能仍把 Bare 访问送进 TLB 仿真，因此我们能在 `satp=0` 时也
   看到 `riscv_cpu_tlb_fill()`。读 QEMU 日志时不能把“有 tlb_fill”误认为
   “分页已开启”。
4. **ASID/VMID 的处理不同。** QEMU 用 `mmu_idx` 区分地址空间，ASID 只是
   `satp` 的一部分；真实 CPU 用 ASID 标记 TLB 表项，进程切换时未必需要
   全刷 TLB。

---

## 七、问题 5：调试细节与踩坑记录

### 7.1 QEMU 没有调试符号：从 ddeb 到可读源码

系统自带的 `/usr/bin/qemu-system-riscv64` 是 stripped 的，host GDB 只能看到
地址。解决过程：

1. 用 `apt-cache policy qemu-system-misc` 确认版本为
   `1:8.2.2+ds-0ubuntu1.18`；
2. 从 `ddebs.ubuntu.com` 下载同版本的
   `qemu-system-misc-dbgsym_8.2.2+ds-0ubuntu1.18_amd64.ddeb`；
3. `dpkg-deb -x` 解出 `.debug` 文件，按 build-id 与二进制匹配；
4. 该 `.debug` 通过 `.gnu_debugaltlink` 指向一个 dwz 公共调试文件，而该文件
   的绝对路径在解包后不存在；用 `objcopy --dump-section` 取出该段，改写为
   工作区内的实际路径，再 `objcopy --update-section` 写回；
5. host GDB 里用 `symbol-file` 加载补好的 debug 文件，并用
   `set substitute-path /usr/src/qemu-1:8.2.2+ds-0ubuntu1.18 <本地源码>` 把
   debug 信息中的源码路径映射到本地的 `qemu-src/`。

### 7.2 `env` 被优化掉

在 `riscv_cpu_tlb_fill` 里直接 `p env->satp` 会报：

```text
value has been optimized out
```

改为从参数 `cs` 反推：

```gdb
p/x ((RISCVCPU *)cs)->env.satp
```

### 7.3 `printf` 引用被优化掉的变量会让整个脚本中断

host 脚本里原本有一行：

```gdb
printf "[HOST] address=0x%lx pa=0x%lx prot=%d ret=%d tlb_size=0x%lx\n", \
       address, pa, prot, ret, tlb_size
```

`ret` 在 -O2 下被优化掉，GDB 报 `value has been optimized out` 后**直接终止
sourced command file**，后面的 `tbreak tlb_set_page` 与 `continue` 都没执行。
解决：把可能被优化掉的变量用 Python `try/except` 包起来，或者只打印确定
可用的局部变量：

```gdb
python
def show(e, fmt="%#x"):
    try:
        print("[HOST] walk: %s = %s" % (e, fmt % int(gdb.parse_and_eval(e))))
    except Exception as ex:
        print("[HOST] walk: %s unavailable (%s)" % (e, ex))
show("pte_addr")
show("pte")
end
```

### 7.4 `tlb_set_page` 断点不触发

原计划在 `tlb_set_page()` 入口读 `addr/paddr/prot/mmu_idx/size`，但断点根本
不命中。反汇编后确认：在 8.2.2 的优化构建里，`tlb_set_page()` 被内联/尾调用
进 `riscv_cpu_tlb_fill()`，实际执行的是 `tlb_set_page_full()`。改为：

```gdb
tbreak *tlb_set_page_full
```

并在入口用寄存器读参数、按 `CPUTLBEntryFull` 的字段偏移读结构体：

```text
xlat_section  offset 0x00
phys_addr     offset 0x08
attrs         offset 0x10
prot          offset 0x14
lg_page_size  offset 0x15
```

这次才抓到 `phys_addr=0x80207000, prot=7, lg_page_size=12`。

### 7.5 1 GiB 大页 ≠ 三层循环

一开始按“Sv39 一定走三层”去解释，和实测 PTE `0x200000cf` 对不上。把 PTE
拆开后才意识到：根表项本身就是叶子，`ptshift=18`，是 1 GiB 大页，循环只跑
一层。这个坑提醒我们：**报告必须写实际观察到的路径，不能把教科书上的
典型路径直接套到启动页表上。**

### 7.6 QEMU 版本与加载方式

本机 QEMU 8.2.2 的 OpenSBI 是 `fw_dynamic`，只有 `-kernel` 会把
`next_addr` 填成 `0x80200000`。`tools/qemu-compat.sh` 的自动判定已改为
QEMU >= 8 使用 `-kernel`，因此直接执行即可；只有强制旧方式时才设置
`QEMU_LOADER_MODE=loader`：

```bash
make test
make grade
```

`make grade` 会先 `make clean`，`bin/kernel`、`bin/ucore.img` 会被删掉；
做 GDB 调试前要先 `make -j4` 重新生成，否则 guest GDB 的 `file bin/kernel`
会失败。

### 7.7 guest GDB 的两个小坑

- `make gdb` 调的是 `riscv64-unknown-elf-gdb`，本机只有 `gdb-multiarch`。
  调试时直接使用 `gdb-multiarch`；交卷前已补一个用户级软链，不需要 sudo：

  ```bash
  mkdir -p ~/.local/bin
  ln -s "$(command -v gdb-multiarch)" ~/.local/bin/riscv64-unknown-elf-gdb
  export PATH="$HOME/.local/bin:$PATH"   # 已写入 ~/.bashrc
  ```

  验证 `riscv64-unknown-elf-gdb --version` 后，`make gdb` 可直接使用。
  若希望系统级安装，仍可执行
  `sudo ln -s "$(command -v gdb-multiarch)" /usr/local/bin/riscv64-unknown-elf-gdb`。
- 连接 gdbstub 后若长时间没有数据，GDB 可能报 `Ignoring packet error`；
  guest 脚本里加 `set remotetimeout unlimited` 可避免。

---

## 八、问题 6：LLM 辅助调试记录

### 8.1 使用的工具

本次分支调试使用 **Qoder 桌面 Agent**（Qoder Desktop 的 Agent 会话）辅助
完成命令生成、QEMU 源码定位、GDB 脚本改写和日志解读；具体底层模型版本以
产品实际配置为准，实验中没有单独记录，因此不在这里编造型号。

### 8.2 场景、提示词与结果

| 场景 | 我给 LLM 的关键信息 | LLM 的建议 | 实际结果与我的修正 |
|---|---|---|---|
| 没有 QEMU 调试符号 | 系统 QEMU 8.2.2，`file`/`info line` 只有地址 | 下载同版本 ddeb；按 build-id 找 `.debug`；用 `symbol-file` 加载 | 可用；但 `.gnu_debugaltlink` 指向的 dwz 文件缺失，后来用 `objcopy` 改写该段路径才恢复行号 |
| host GDB 脚本中断 | 贴出 `value has been optimized out` 和完整脚本 | 去掉 `ret`；把易被优化掉的变量改成 Python `try/except` | 解决；脚本继续执行到 `tlb_set_page_full` |
| 取 `tlb_set_page` 参数 | 断点不命中，贴出 `info break` 和反汇编 | 该函数被内联，改断 `tlb_set_page_full`，按结构体偏移读字段 | 抓到 `phys_addr/prot/lg_page_size` |
| 解释页表遍历 | 贴出 `pte=0x200000cf`、`pte_addr=0x80205ff8`、`pa=0x80207010` | 第一次按“三层循环”解释，和 PTE 对不上 | 我按 `ptshift=18` 和 PPN 重新计算，确认是 1 GiB 大页，只走一层；LLM 随后修正了说法 |
| QEMU 启动后停在 0x0 | OpenSBI 日志 `Next Address : 0x0`、`-d int` 全是 `fault_fetch` | 检查 `qemu-compat.sh` 的加载模式 | QEMU 8.2.2 也要 `-kernel`；自动判定已改为 QEMU >= 8 用 `-kernel` |

### 8.3 我认为有效的协作方式

1. **给原始输出，不给二手总结。** 例如直接贴 GDB 的
   `value has been optimized out`、`info break`、反汇编，而不是说“断点没
   生效”。原始错误信息里往往已经包含函数名、行号和优化状态。
2. **一次只改一个变量。** 先修 `ret`，再修 `tlb_set_page`，再修页表解释；
   同时改三处会让失败原因不可归因。
3. **让 LLM 解释“为什么”，而不是只要命令。** 例如问“为什么
   `tlb_set_page` 不触发”，得到的“被内联/尾调用”解释能直接指导下一步
   断点位置。
4. **对 LLM 的结论做数值校验。** 这次最关键的一次纠错，是把
   `0x200000cf` 拆成 PPN 和 flags、再算 `ptshift=18`，发现它不可能是三层
   4 KiB 映射。LLM 的第一版解释看起来合理，但和实测 PTE 矛盾，必须以
   实测为准。

### 8.4 我认为 LLM 容易出错的地方

- **把典型路径当成实际路径。** “Sv39 三层页表”是教科书典型情形，但启动
  页表用 1 GiB 大页，实际只走一层；LLM 不会主动检查 PTE 的大页位。
- **忽略编译优化。** host GDB 里很多局部变量是 `<optimized out>`，LLM 生
  成的脚本若直接 `printf` 这些变量，会让整个脚本中断。
- **把 QEMU 的软件 TLB 当成硬件 TLB。** 需要不断用“这是仿真器实现，不是
  周期精确硬件”来校准。

---

## 九、复验结果

最终在真实 WSL 环境中执行（`qemu-compat.sh` 自动选 `-kernel`）：

```bash
cd lab2_new
make clean
make test
make grade
```

关键输出：

```text
memory management: best_fit_pmm_manager
grading: 6 / 6 points
check_alloc_page() succeeded!
slub_check: passed (80 object cache test)

check page init: OK
check best fit: OK
Total Score: 20/20
```

`make grade` 的完整输出见最终报告第五节；本节只保留与分支任务和基线相关的
关键行。

---

## 十、小结

这次分支任务真正学到的不是“GDB 怎么用”，而是**地址翻译链路的分层**：

```text
guest 指令的 EA
  → TCG 查 QEMU 的软件 TLB（mmu_lookup1 / tlb_hit）
  → miss 时进入 riscv_cpu_tlb_fill
  → get_physical_address 按 satp 逐级查页表（本例 1 GiB 叶子只查一级）
  → tlb_set_page_full 回填 phys_addr/prot/lg_page_size
  → TCG 用翻译结果完成这次 store
```

同时也要分清三套地址：guest 的虚拟地址、guest 的物理地址、QEMU 进程自己的
host 地址。host GDB 看到的是 QEMU 的 C 变量和 host 指针；guest GDB 看到的
才是 `satp`、`PTE` 和内核虚拟地址。两套 GDB 各看一层，合起来才是完整的
“指令 → MMU → 内存”路径。
