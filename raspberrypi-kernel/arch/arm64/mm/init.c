// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on arch/arm/mm/init.c
 *
 * Copyright (C) 1995-2005 Russell King
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/errno.h>
#include <linux/swap.h>
#include <linux/init.h>
#include <linux/cache.h>
#include <linux/mman.h>
#include <linux/nodemask.h>
#include <linux/initrd.h>
#include <linux/gfp.h>
#include <linux/memblock.h>
#include <linux/sort.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/dma-direct.h>
#include <linux/dma-map-ops.h>
#include <linux/efi.h>
#include <linux/swiotlb.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/kexec.h>
#include <linux/crash_dump.h>
#include <linux/hugetlb.h>

#include <asm/boot.h>
#include <asm/fixmap.h>
#include <asm/kasan.h>
#include <asm/kernel-pgtable.h>
#include <asm/kexec.h>
#include <asm/memory.h>
#include <asm/numa.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <linux/sizes.h>
#include <asm/tlb.h>
#include <asm/alternative.h>

#define ARM64_ZONE_DMA_BITS	30

/*
 * We need to be able to catch inadvertent references to memstart_addr
 * that occur (potentially in generic code) before arm64_memblock_init()
 * executes, which assigns it its actual value. So use a default value
 * that cannot be mistaken for a real physical address.
 */
s64 memstart_addr __ro_after_init = -1;
EXPORT_SYMBOL(memstart_addr);

/*
 * We create both ZONE_DMA and ZONE_DMA32. ZONE_DMA covers the first 1G of
 * memory as some devices, namely the Raspberry Pi 4, have peripherals with
 * this limited view of the memory. ZONE_DMA32 will cover the rest of the 32
 * bit addressable memory area.
 */
phys_addr_t arm64_dma_phys_limit __ro_after_init;
phys_addr_t arm64_dma32_phys_limit __ro_after_init;

#ifndef CONFIG_KEXEC_CORE
static void __init reserve_crashkernel(void)
{
}
#endif

/*
 * The main usage of linux,usable-memory-range is for crash dump kernel.
 * Originally, the number of usable-memory regions is one. Now there may
 * be two regions, low region and high region.
 * To make compatibility with existing user-space and older kdump, the low
 * region is always the last range of linux,usable-memory-range if exist.
 */
#define MAX_USABLE_RANGES	2

#ifdef CONFIG_CRASH_DUMP
static int __init early_init_dt_scan_elfcorehdr(unsigned long node,
		const char *uname, int depth, void *data)
{
	const __be32 *reg;
	int len;

	if (depth != 1 || strcmp(uname, "chosen") != 0)
		return 0;

	reg = of_get_flat_dt_prop(node, "linux,elfcorehdr", &len);
	if (!reg || (len < (dt_root_addr_cells + dt_root_size_cells)))
		return 1;

	elfcorehdr_addr = dt_mem_next_cell(dt_root_addr_cells, &reg);
	elfcorehdr_size = dt_mem_next_cell(dt_root_size_cells, &reg);

	return 1;
}

/*
 * reserve_elfcorehdr() - reserves memory for elf core header
 *
 * This function reserves the memory occupied by an elf core header
 * described in the device tree. This region contains all the
 * information about primary kernel's core image and is used by a dump
 * capture kernel to access the system memory on primary kernel.
 */
static void __init reserve_elfcorehdr(void)
{
	of_scan_flat_dt(early_init_dt_scan_elfcorehdr, NULL);

	if (!elfcorehdr_size)
		return;

	if (memblock_is_region_reserved(elfcorehdr_addr, elfcorehdr_size)) {
		pr_warn("elfcorehdr is overlapped\n");
		return;
	}

	memblock_reserve(elfcorehdr_addr, elfcorehdr_size);

	pr_info("Reserving %lldKB of memory at 0x%llx for elfcorehdr\n",
		elfcorehdr_size >> 10, elfcorehdr_addr);
}
#else
static void __init reserve_elfcorehdr(void)
{
}
#endif /* CONFIG_CRASH_DUMP */

#ifdef CONFIG_QUICK_KEXEC
static int __init parse_quick_kexec(char *p)
{
	if (!p)
		return 0;

	quick_kexec_res.end = PAGE_ALIGN(memparse(p, NULL));

	return 0;
}
early_param("quickkexec", parse_quick_kexec);

static void __init reserve_quick_kexec(void)
{
	unsigned long long mem_start, mem_len;

	mem_len = quick_kexec_res.end;
	if (mem_len == 0)
		return;

	/* Current arm64 boot protocol requires 2MB alignment */
	mem_start = memblock_find_in_range(0, arm64_dma32_phys_limit,
			mem_len, SZ_2M);
	if (mem_start == 0) {
		pr_warn("cannot allocate quick kexec mem (size:0x%llx)\n",
			mem_len);
		quick_kexec_res.end = 0;
		return;
	}

	memblock_reserve(mem_start, mem_len);
	pr_info("quick kexec mem reserved: 0x%016llx - 0x%016llx (%lld MB)\n",
		mem_start, mem_start + mem_len,	mem_len >> 20);

	quick_kexec_res.start = mem_start;
	quick_kexec_res.end = mem_start + mem_len - 1;
}
#endif

/*
 * Return the maximum physical address for a zone with a given address size
 * limit. It currently assumes that for memory starting above 4G, 32-bit
 * devices will use a DMA offset.
 */
static phys_addr_t __init max_zone_phys(unsigned int zone_bits)
{
	phys_addr_t offset = memblock_start_of_DRAM() & GENMASK_ULL(63, zone_bits);
	return min(offset + (1ULL << zone_bits), memblock_end_of_DRAM());
}

static void __init zone_sizes_init(unsigned long min, unsigned long max)
{
	unsigned long max_zone_pfns[MAX_NR_ZONES]  = {0};

#ifdef CONFIG_ZONE_DMA
	max_zone_pfns[ZONE_DMA] = PFN_DOWN(arm64_dma_phys_limit);
#endif
#ifdef CONFIG_ZONE_DMA32
	max_zone_pfns[ZONE_DMA32] = PFN_DOWN(arm64_dma32_phys_limit);
#endif
	max_zone_pfns[ZONE_NORMAL] = max;

	free_area_init(max_zone_pfns);
}

int pfn_valid(unsigned long pfn)
{
	phys_addr_t addr = pfn << PAGE_SHIFT;

	if ((addr >> PAGE_SHIFT) != pfn)
		return 0;

#ifdef CONFIG_SPARSEMEM
	if (pfn_to_section_nr(pfn) >= NR_MEM_SECTIONS)
		return 0;

	if (!valid_section(__pfn_to_section(pfn)))
		return 0;
#endif
	return memblock_is_map_memory(addr);
}
EXPORT_SYMBOL(pfn_valid);

static phys_addr_t memory_limit = PHYS_ADDR_MAX;

/*
 * Limit the memory size that was specified via FDT.
 */
static int __init early_mem(char *p)
{
	if (!p)
		return 1;

	memory_limit = memparse(p, &p) & PAGE_MASK;
	pr_notice("Memory limited to %lldMB\n", memory_limit >> 20);

	return 0;
}
early_param("mem", early_mem);

static int __init early_init_dt_scan_usablemem(unsigned long node,
		const char *uname, int depth, void *data)
{
	struct memblock_region *usable_rgns = data;
	const __be32 *reg, *endp;
	int len, nr = 0;

	if (depth != 1 || strcmp(uname, "chosen") != 0)
		return 0;

	reg = of_get_flat_dt_prop(node, "linux,usable-memory-range", &len);
	if (!reg || (len < (dt_root_addr_cells + dt_root_size_cells)))
		return 1;

	endp = reg + (len / sizeof(__be32));
	while ((endp - reg) >= (dt_root_addr_cells + dt_root_size_cells)) {
		usable_rgns[nr].base = dt_mem_next_cell(dt_root_addr_cells, &reg);
		usable_rgns[nr].size = dt_mem_next_cell(dt_root_size_cells, &reg);

		if (++nr >= MAX_USABLE_RANGES)
			break;
	}

	return 1;
}

static void __init fdt_enforce_memory_region(void)
{
	struct memblock_region usable_rgns[MAX_USABLE_RANGES] = {
		{ .size = 0 },
		{ .size = 0 }
	};

	of_scan_flat_dt(early_init_dt_scan_usablemem, &usable_rgns);

	/*
	 * The first range of usable-memory regions is for crash dump
	 * kernel with only one region or for high region with two regions,
	 * the second range is dedicated for low region if exist.
	 */
	if (usable_rgns[0].size)
		memblock_cap_memory_range(usable_rgns[0].base, usable_rgns[0].size);
	if (usable_rgns[1].size)
		memblock_add(usable_rgns[1].base, usable_rgns[1].size);
}

#ifdef CONFIG_ARM64_CPU_PARK
struct cpu_park_info park_info = {
	.start = 0,
	.len = PARK_SECTION_SIZE * NR_CPUS,
	.start_v = 0,
};

static int __init parse_park_mem(char *p)
{
	if (!p)
		return 0;

	park_info.start = PAGE_ALIGN(memparse(p, NULL));
	if (park_info.start == 0)
		pr_info("cpu park mem params[%s]", p);

	return 0;
}
early_param("cpuparkmem", parse_park_mem);

static int __init reserve_park_mem(void)
{
	if (park_info.start == 0 || park_info.len == 0)
		return 0;

	park_info.start = PAGE_ALIGN(park_info.start);
	park_info.len = PAGE_ALIGN(park_info.len);

	if (!memblock_is_region_memory(park_info.start, park_info.len)) {
		pr_warn("cannot reserve park mem: region is not memory!");
		goto out;
	}

	if (memblock_is_region_reserved(park_info.start, park_info.len)) {
		pr_warn("cannot reserve park mem: region overlaps reserved memory!");
		goto out;
	}

	memblock_remove(park_info.start, park_info.len);
	pr_info("cpu park mem reserved: 0x%016lx - 0x%016lx (%ld MB)",
		park_info.start, park_info.start + park_info.len,
		park_info.len >> 20);

	return 0;
out:
	park_info.start = 0;
	park_info.len = 0;
	return -EINVAL;
}
#endif

void __init arm64_memblock_init(void)
{
	const s64 linear_region_size = BIT(vabits_actual - 1);

	/* Handle linux,usable-memory-range property */
	fdt_enforce_memory_region();

	/* Remove memory above our supported physical address size */
	memblock_remove(1ULL << PHYS_MASK_SHIFT, ULLONG_MAX);

	/*
	 * Select a suitable value for the base of physical memory.
	 */
	memstart_addr = round_down(memblock_start_of_DRAM(),
				   ARM64_MEMSTART_ALIGN);

	/*
	 * Remove the memory that we will not be able to cover with the
	 * linear mapping. Take care not to clip the kernel which may be
	 * high in memory.
	 */
	memblock_remove(max_t(u64, memstart_addr + linear_region_size,
			__pa_symbol(_end)), ULLONG_MAX);
	if (memstart_addr + linear_region_size < memblock_end_of_DRAM()) {
		/* ensure that memstart_addr remains sufficiently aligned */
		memstart_addr = round_up(memblock_end_of_DRAM() - linear_region_size,
					 ARM64_MEMSTART_ALIGN);
		memblock_remove(0, memstart_addr);
	}

	/*
	 * If we are running with a 52-bit kernel VA config on a system that
	 * does not support it, we have to place the available physical
	 * memory in the 48-bit addressable part of the linear region, i.e.,
	 * we have to move it upward. Since memstart_addr represents the
	 * physical address of PAGE_OFFSET, we have to *subtract* from it.
	 */
	if (IS_ENABLED(CONFIG_ARM64_VA_BITS_52) && (vabits_actual != 52))
		memstart_addr -= _PAGE_OFFSET(48) - _PAGE_OFFSET(52);

	/*
	 * Apply the memory limit if it was set. Since the kernel may be loaded
	 * high up in memory, add back the kernel region that must be accessible
	 * via the linear mapping.
	 */
	if (memory_limit != PHYS_ADDR_MAX) {
		memblock_mem_limit_remove_map(memory_limit);
		memblock_add(__pa_symbol(_text), (u64)(_end - _text));
	}

	if (IS_ENABLED(CONFIG_BLK_DEV_INITRD) && phys_initrd_size) {
		/*
		 * Add back the memory we just removed if it results in the
		 * initrd to become inaccessible via the linear mapping.
		 * Otherwise, this is a no-op
		 */
		u64 base = phys_initrd_start & PAGE_MASK;
		u64 size = PAGE_ALIGN(phys_initrd_start + phys_initrd_size) - base;

		/*
		 * We can only add back the initrd memory if we don't end up
		 * with more memory than we can address via the linear mapping.
		 * It is up to the bootloader to position the kernel and the
		 * initrd reasonably close to each other (i.e., within 32 GB of
		 * each other) so that all granule/#levels combinations can
		 * always access both.
		 */
		if (WARN(base < memblock_start_of_DRAM() ||
			 base + size > memblock_start_of_DRAM() +
				       linear_region_size,
			"initrd not fully accessible via the linear mapping -- please check your bootloader ...\n")) {
			phys_initrd_size = 0;
		} else {
			memblock_remove(base, size); /* clear MEMBLOCK_ flags */
			memblock_add(base, size);
			memblock_reserve(base, size);
		}
	}

	if (IS_ENABLED(CONFIG_RANDOMIZE_BASE)) {
		extern u16 memstart_offset_seed;
		u64 range = linear_region_size -
			    (memblock_end_of_DRAM() - memblock_start_of_DRAM());

		/*
		 * If the size of the linear region exceeds, by a sufficient
		 * margin, the size of the region that the available physical
		 * memory spans, randomize the linear region as well.
		 */
		if (memstart_offset_seed > 0 && range >= ARM64_MEMSTART_ALIGN) {
			range /= ARM64_MEMSTART_ALIGN;
			memstart_addr -= ARM64_MEMSTART_ALIGN *
					 ((range * memstart_offset_seed) >> 16);
		}
	}

	/*
	 * Register the kernel text, kernel data, initrd, and initial
	 * pagetables with memblock.
	 */
	memblock_reserve(__pa_symbol(_text), _end - _text);
	if (IS_ENABLED(CONFIG_BLK_DEV_INITRD) && phys_initrd_size) {
		/* the generic initrd code expects virtual addresses */
		initrd_start = __phys_to_virt(phys_initrd_start);
		initrd_end = initrd_start + phys_initrd_size;
	}

	early_init_fdt_scan_reserved_mem();

	if (IS_ENABLED(CONFIG_ZONE_DMA)) {
		zone_dma_bits = ARM64_ZONE_DMA_BITS;
		arm64_dma_phys_limit = max_zone_phys(ARM64_ZONE_DMA_BITS);
	}

	if (IS_ENABLED(CONFIG_ZONE_DMA32))
		arm64_dma32_phys_limit = max_zone_phys(32);
	else
		arm64_dma32_phys_limit = PHYS_MASK + 1;

	reserve_crashkernel();

#ifdef CONFIG_QUICK_KEXEC
	reserve_quick_kexec();
#endif

#ifdef CONFIG_ARM64_CPU_PARK
	reserve_park_mem();
#endif

	reserve_elfcorehdr();

	high_memory = __va(memblock_end_of_DRAM() - 1) + 1;

	dma_contiguous_reserve(arm64_dma32_phys_limit);
}

void __init bootmem_init(void)
{
	unsigned long min, max;

	min = PFN_UP(memblock_start_of_DRAM());
	max = PFN_DOWN(memblock_end_of_DRAM());

	early_memtest(min << PAGE_SHIFT, max << PAGE_SHIFT);

	max_pfn = max_low_pfn = max;
	min_low_pfn = min;

	arm64_numa_init();

	/*
	 * must be done after arm64_numa_init() which calls numa_init() to
	 * initialize node_online_map that gets used in hugetlb_cma_reserve()
	 * while allocating required CMA size across online nodes.
	 */
#if defined(CONFIG_HUGETLB_PAGE) && defined(CONFIG_CMA)
	arm64_hugetlb_cma_reserve();
#endif

	dma_pernuma_cma_reserve();

	/*
	 * sparse_init() tries to allocate memory from memblock, so must be
	 * done after the fixed reservations
	 */
	sparse_init();
	zone_sizes_init(min, max);

	memblock_dump_all();
}

#ifndef CONFIG_SPARSEMEM_VMEMMAP
static inline void free_memmap(unsigned long start_pfn, unsigned long end_pfn)
{
	struct page *start_pg, *end_pg;
	unsigned long pg, pgend;

	/*
	 * Convert start_pfn/end_pfn to a struct page pointer.
	 */
	start_pg = pfn_to_page(start_pfn - 1) + 1;
	end_pg = pfn_to_page(end_pfn - 1) + 1;

	/*
	 * Convert to physical addresses, and round start upwards and end
	 * downwards.
	 */
	pg = (unsigned long)PAGE_ALIGN(__pa(start_pg));
	pgend = (unsigned long)__pa(end_pg) & PAGE_MASK;

	/*
	 * If there are free pages between these, free the section of the
	 * memmap array.
	 */
	if (pg < pgend)
		memblock_free(pg, pgend - pg);
}

/*
 * The mem_map array can get very big. Free the unused area of the memory map.
 */
static void __init free_unused_memmap(void)
{
	unsigned long start, end, prev_end = 0;
	int i;

	for_each_mem_pfn_range(i, MAX_NUMNODES, &start, &end, NULL) {
#ifdef CONFIG_SPARSEMEM
		/*
		 * Take care not to free memmap entries that don't exist due
		 * to SPARSEMEM sections which aren't present.
		 */
		start = min(start, ALIGN(prev_end, PAGES_PER_SECTION));
#endif
		/*
		 * If we had a previous bank, and there is a space between the
		 * current bank and the previous, free it.
		 */
		if (prev_end && prev_end < start)
			free_memmap(prev_end, start);

		/*
		 * Align up here since the VM subsystem insists that the
		 * memmap entries are valid from the bank end aligned to
		 * MAX_ORDER_NR_PAGES.
		 */
		prev_end = ALIGN(end, MAX_ORDER_NR_PAGES);
	}

#ifdef CONFIG_SPARSEMEM
	if (!IS_ALIGNED(prev_end, PAGES_PER_SECTION))
		free_memmap(prev_end, ALIGN(prev_end, PAGES_PER_SECTION));
#endif
}
#endif	/* !CONFIG_SPARSEMEM_VMEMMAP */

/*
 * mem_init() marks the free areas in the mem_map and tells us how much memory
 * is free.  This is done after various parts of the system have claimed their
 * memory after the kernel image.
 */
void __init mem_init(void)
{
	if (swiotlb_force == SWIOTLB_FORCE ||
	    max_pfn > PFN_DOWN(arm64_dma_phys_limit ? : arm64_dma32_phys_limit))
		swiotlb_init(1);
	else
		swiotlb_force = SWIOTLB_NO_FORCE;

	set_max_mapnr(max_pfn - PHYS_PFN_OFFSET);

#ifndef CONFIG_SPARSEMEM_VMEMMAP
	free_unused_memmap();
#endif
	/* this will put all unused low memory onto the freelists */
	memblock_free_all();

	mem_init_print_info(NULL);

	/*
	 * Check boundaries twice: Some fundamental inconsistencies can be
	 * detected at build time already.
	 */
#ifdef CONFIG_COMPAT
	BUILD_BUG_ON(TASK_SIZE_32 > DEFAULT_MAP_WINDOW_64);
#endif

	if (PAGE_SIZE >= 16384 && get_num_physpages() <= 128) {
		extern int sysctl_overcommit_memory;
		/*
		 * On a machine this small we won't get anywhere without
		 * overcommit, so turn it on by default.
		 */
		sysctl_overcommit_memory = OVERCOMMIT_ALWAYS;
	}
}

void free_initmem(void)
{
	free_reserved_area(lm_alias(__init_begin),
			   lm_alias(__init_end),
			   POISON_FREE_INITMEM, "unused kernel");
	/*
	 * Unmap the __init region but leave the VM area in place. This
	 * prevents the region from being reused for kernel modules, which
	 * is not supported by kallsyms.
	 */
	unmap_kernel_range((u64)__init_begin, (u64)(__init_end - __init_begin));
}

void dump_mem_limit(void)
{
	if (memory_limit != PHYS_ADDR_MAX) {
		pr_emerg("Memory Limit: %llu MB\n", memory_limit >> 20);
	} else {
		pr_emerg("Memory Limit: none\n");
	}
}
