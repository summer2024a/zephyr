/*
 * Copyright 2020 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 */

/**
 * @file
 * @brief codes required for AArch64 multicore and Zephyr smp support
 */

#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/kernel_structs.h>
#include <kernel_arch_interface.h>
#include <ipi.h>
#include <zephyr/init.h>
#include <zephyr/arch/arm64/mm.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/drivers/interrupt_controller/gic.h>
#include <zephyr/drivers/pm_cpu_ops.h>
#include <zephyr/arch/arch_interface.h>
#include <zephyr/platform/hooks.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/irq.h>
#include "boot.h"

#if defined(CONFIG_SOC_MESON_S4_SMP_DEBUG)
#include <stdio.h>
#endif

#define INV_MPID	UINT64_MAX

#define SGI_SCHED_IPI	0
#define SGI_MMCFG_IPI	1
#define SGI_FPU_IPI	2

struct boot_params {
	uint64_t mpid;
	char *sp;
	uint8_t voting[DT_CHILD_NUM_STATUS_OKAY(DT_PATH(cpus))];
	arch_cpustart_t fn;
	void *arg;
	int cpu_num;
};

/* Offsets used in reset.S */
BUILD_ASSERT(offsetof(struct boot_params, mpid) == BOOT_PARAM_MPID_OFFSET);
BUILD_ASSERT(offsetof(struct boot_params, sp) == BOOT_PARAM_SP_OFFSET);
BUILD_ASSERT(offsetof(struct boot_params, voting) == BOOT_PARAM_VOTING_OFFSET);

volatile struct boot_params __aligned(L1_CACHE_BYTES) arm64_cpu_boot_params = {
	.mpid = -1,
};

const uint64_t cpu_node_list[] = {
	DT_FOREACH_CHILD_STATUS_OKAY_SEP(DT_PATH(cpus), DT_REG_ADDR, (,))
};

BUILD_ASSERT(ARRAY_SIZE(cpu_node_list) == DT_CHILD_NUM_STATUS_OKAY(DT_PATH(cpus)));

/* cpu_map saves the maping of core id and mpid */
static uint64_t cpu_map[CONFIG_MP_MAX_NUM_CPUS] = {
	[0 ... (CONFIG_MP_MAX_NUM_CPUS - 1)] = INV_MPID
};

#ifdef CONFIG_SOC_MESON_S4_BOOT_TRACE
extern void meson_s4_boot_marker(char tag);
#define S4_SMP_TRACE(tag) meson_s4_boot_marker(tag)
#else
#define S4_SMP_TRACE(tag) do { } while (0)
#endif

#if defined(CONFIG_SOC_MESON_S4_SMP_DEBUG)
#define S4_SMP_LOG(...) printk(__VA_ARGS__)
#else
#define S4_SMP_LOG(...) do { } while (0)
#endif

#if defined(CONFIG_SOC_AMLOGIC_MESON_S4)
extern void meson_s4_enable_dcache_el1(void);

static void meson_s4_smp_publish_boot_params(void)
{
	barrier_dsync_fence_full();
	sys_cache_data_flush_range((void *)&arm64_cpu_boot_params,
				   sizeof(arm64_cpu_boot_params));
	barrier_dsync_fence_full();
}

static bool meson_s4_smp_secondary_ready(void)
{
	arch_cpustart_t fn;

	sys_cache_data_invd_range((void *)&arm64_cpu_boot_params,
				  sizeof(arm64_cpu_boot_params));
	barrier_dsync_fence_full();
	fn = arm64_cpu_boot_params.fn;

	return fn == NULL;
}

static void meson_s4_smp_wait_secondary(void)
{
	unsigned int polls;

	for (polls = 0; !meson_s4_smp_secondary_ready(); polls++) {
		if ((polls & 0xffU) == 0U) {
			__asm__ volatile("sev" ::: "memory");
		}
		wfe();
	}
}

static int meson_s4_pm_cpu_on_retry(uint64_t cpu_mpid)
{
	int rc = -EINVAL;
	int attempt;

	for (attempt = 0; attempt < 3; attempt++) {
		if (attempt > 0) {
			k_busy_wait(100000);
		}

		rc = pm_cpu_on(cpu_mpid, (uint64_t)&__start);
		if (rc == 0) {
			break;
		}

		printk("smp: pm_cpu_on mpid=%#llx failed (%d), retry %d/3\n",
		       (unsigned long long)cpu_mpid, rc, attempt + 1);
	}

	return rc;
}
#endif /* CONFIG_SOC_AMLOGIC_MESON_S4 */

extern void z_arm64_mm_init(bool is_primary_core);

/* Called from Zephyr initialization */
void arch_cpu_start(int cpu_num, k_thread_stack_t *stack, int sz,
		    arch_cpustart_t fn, void *arg)
{
	int cpu_count;
	static int i;
	uint64_t cpu_mpid = 0;
	uint64_t primary_core_mpid;

#if defined(CONFIG_SOC_MESON_S4_SMP_DEBUG)
	printf("s4: arch_cpu_start enter cpu_num=%d curr_cpu=%u\n",
	       cpu_num, arch_curr_cpu()->id);
#endif

	/* Now it is on primary core */
	__ASSERT(arch_curr_cpu()->id == 0, "");
#if defined(CONFIG_SOC_MESON_S4_SMP_DEBUG)
	printf("s4: arch_cpu_start after assert\n");
#endif
	primary_core_mpid = MPIDR_TO_CORE(GET_MPIDR());
#if defined(CONFIG_SOC_MESON_S4_SMP_DEBUG)
	printf("s4: arch_cpu_start primary=%#llx loop_i=%d\n",
	       (unsigned long long)primary_core_mpid, i);
#endif

#if defined(CONFIG_SOC_AMLOGIC_MESON_S4)
	unsigned int irq_key = arch_irq_lock();

	if (cpu_num == 1) {
		k_busy_wait(50000);
	}
#endif

	cpu_count = ARRAY_SIZE(cpu_node_list);

#ifdef CONFIG_ARM64_FALLBACK_ON_RESERVED_CORES
	__ASSERT(cpu_count >= CONFIG_MP_MAX_NUM_CPUS,
		"The count of CPU Core nodes in dts is not greater or equal to CONFIG_MP_MAX_NUM_CPUS\n");
#else
	__ASSERT(cpu_count == CONFIG_MP_MAX_NUM_CPUS,
		"The count of CPU Cores nodes in dts is not equal to CONFIG_MP_MAX_NUM_CPUS\n");
#endif

#if defined(CONFIG_SOC_MESON_S4_SMP_DEBUG)
	printf("s4: arch_cpu_start set boot_params\n");
#endif

	arm64_cpu_boot_params.sp = K_KERNEL_STACK_BUFFER(stack) + sz;
	arm64_cpu_boot_params.fn = fn;
	arm64_cpu_boot_params.arg = arg;
	arm64_cpu_boot_params.cpu_num = cpu_num;

#if defined(CONFIG_SOC_MESON_S4_SMP_DEBUG)
	printf("s4: arch_cpu_start boot_params ok fn=%p\n",
	       (void *)arm64_cpu_boot_params.fn);
#endif

	S4_SMP_TRACE('Q');

#if defined(CONFIG_SOC_MESON_S4_SMP_DEBUG)
	printf("s4: arch_cpu_start loop i=%d count=%d\n", i, cpu_count);
#endif

	for (; i < cpu_count; i++) {
#if defined(CONFIG_SOC_MESON_S4_SMP_DEBUG)
		printf("s4: arch_cpu_start loop i=%d node=%llu\n",
		       i, (unsigned long long)cpu_node_list[i]);
#endif
		if (cpu_node_list[i] == primary_core_mpid) {
			continue;
		}

		cpu_mpid = cpu_node_list[i];

		barrier_dsync_fence_full();

		/* store mpid last as this is our synchronization point */
		arm64_cpu_boot_params.mpid = cpu_mpid;

#if defined(CONFIG_SOC_AMLOGIC_MESON_S4)
		meson_s4_smp_publish_boot_params();
#else
		sys_cache_data_flush_range((void *)&arm64_cpu_boot_params,
					   sizeof(arm64_cpu_boot_params));
#endif

		S4_SMP_LOG("smp: start cpu%d mpid=%#llx primary=%#llx\n",
			    cpu_num, (unsigned long long)cpu_mpid,
			    (unsigned long long)primary_core_mpid);
#if defined(CONFIG_SOC_MESON_S4_SMP_DEBUG)
		printf("s4: arch_cpu_start pm_cpu_on mpid=%#llx ep=%#llx\n",
		       (unsigned long long)cpu_mpid, (unsigned long long)&__start);
#endif
		S4_SMP_TRACE('+');

#if defined(CONFIG_SOC_AMLOGIC_MESON_S4)
		if (meson_s4_pm_cpu_on_retry(cpu_mpid)) {
#else
		if (pm_cpu_on(cpu_mpid, (uint64_t)&__start)) {
#endif
			printk("Failed to boot secondary CPU core %d (MPID:%#llx)\n",
			       cpu_num, cpu_mpid);
#ifdef CONFIG_ARM64_FALLBACK_ON_RESERVED_CORES
			printk("Falling back on reserved cores\n");
			continue;
#else
			k_panic();
#endif
		}

		S4_SMP_TRACE('=');
		break;
	}
	if (i++ == cpu_count) {
		printk("Can't find CPU Core %d from dts and failed to boot it\n", cpu_num);
		k_panic();
	}

	/* Wait secondary cores up, see arch_secondary_cpu_init */
#if defined(CONFIG_SOC_AMLOGIC_MESON_S4)
	meson_s4_smp_wait_secondary();
#else
	while (arm64_cpu_boot_params.fn) {
#ifdef CONFIG_CACHE_MANAGEMENT
		sys_cache_data_invd_range((void *)&arm64_cpu_boot_params.fn,
					  sizeof(arm64_cpu_boot_params.fn));
#endif
		wfe();
	}
#endif

	S4_SMP_TRACE('9');

	cpu_map[cpu_num] = cpu_mpid;

	printk("Secondary CPU core %d (MPID:%#llx) is up\n", cpu_num, cpu_mpid);

#if defined(CONFIG_SOC_AMLOGIC_MESON_S4)
	if (cpu_num < (CONFIG_MP_MAX_NUM_CPUS - 1)) {
		k_busy_wait(50000);
	}
	arch_irq_unlock(irq_key);
#endif
}

/* the C entry of secondary cores */
void arch_secondary_cpu_init(int cpu_num)
{
	cpu_num = arm64_cpu_boot_params.cpu_num;
	arch_cpustart_t fn;
	void *arg;

	S4_SMP_TRACE('>');

#if defined(CONFIG_SOC_MESON_S4_SMP_DEBUG)
	printf("s4: secondary enter mpid=%#llx boot_mpid=%#llx cpu_num=%d\n",
	       (unsigned long long)MPIDR_TO_CORE(GET_MPIDR()),
	       (unsigned long long)arm64_cpu_boot_params.mpid, cpu_num);
#endif

	S4_SMP_LOG("smp: secondary mpid=%#llx boot_mpid=%#llx cpu_num=%d\n",
		   (unsigned long long)MPIDR_TO_CORE(GET_MPIDR()),
		   (unsigned long long)arm64_cpu_boot_params.mpid, cpu_num);

	__ASSERT(arm64_cpu_boot_params.mpid == MPIDR_TO_CORE(GET_MPIDR()), "");

	/* Initialize tpidrro_el0 with our struct _cpu instance address */
	write_tpidrro_el0((uintptr_t)&_kernel.cpus[cpu_num]);

	z_arm64_mm_init(false);
	S4_SMP_TRACE('{');

#if defined(CONFIG_SOC_AMLOGIC_MESON_S4)
	/*
	 * Enable dcache before GIC/fn sync: primary polls fn with dcache on;
	 * secondary must not store NULL while its dcache is still off.
	 */
	meson_s4_enable_dcache_el1();
#endif

#ifdef CONFIG_ARM64_SAFE_EXCEPTION_STACK
	z_arm64_safe_exception_stack_init();
#endif

#ifdef CONFIG_SMP
	arm_gic_secondary_init();
	S4_SMP_TRACE('}');

	irq_enable(SGI_SCHED_IPI);
#ifdef CONFIG_USERSPACE
	irq_enable(SGI_MMCFG_IPI);
#endif
#ifdef CONFIG_FPU_SHARING
	irq_enable(SGI_FPU_IPI);
#endif
#endif

	soc_per_core_init_hook();
	S4_SMP_TRACE('~');

	fn = arm64_cpu_boot_params.fn;
	arg = arm64_cpu_boot_params.arg;
	barrier_dsync_fence_full();

	/*
	 * Secondary core clears .fn to announce its presence.
	 * Primary core is polling for this. We no longer own
	 * arm64_cpu_boot_params afterwards.
	 */
	arm64_cpu_boot_params.fn = NULL;
#if defined(CONFIG_SOC_AMLOGIC_MESON_S4)
	meson_s4_smp_publish_boot_params();
#else
	barrier_dsync_fence_full();
#ifdef CONFIG_CACHE_MANAGEMENT
	sys_cache_data_flush_range((void *)&arm64_cpu_boot_params.fn,
				   sizeof(arm64_cpu_boot_params.fn));
#endif
#endif
	sev();

	S4_SMP_TRACE('!');

#if defined(CONFIG_SOC_MESON_S4_SMP_DEBUG)
	printf("s4: secondary fn cleared, entering smp_init_top\n");
#endif

	fn(arg);
}

#ifdef CONFIG_SMP

static void send_ipi(unsigned int ipi, uint32_t cpu_bitmap)
{
	uint64_t mpidr = MPIDR_TO_CORE(GET_MPIDR());

	/*
	 * Send SGI to all cores except itself
	 */
	unsigned int num_cpus = arch_num_cpus();

	for (int i = 0; i < num_cpus; i++) {
		if ((cpu_bitmap & BIT(i)) == 0) {
			continue;
		}

		uint64_t target_mpidr = cpu_map[i];
		uint8_t aff0;

		if (mpidr == target_mpidr || target_mpidr == INV_MPID) {
			continue;
		}

		aff0 = MPIDR_AFFLVL(target_mpidr, 0);
		gic_raise_sgi(ipi, target_mpidr, 1 << aff0);
	}
}

void sched_ipi_handler(const void *unused)
{
	ARG_UNUSED(unused);

	z_sched_ipi();
}

void arch_sched_broadcast_ipi(void)
{
	send_ipi(SGI_SCHED_IPI, IPI_ALL_CPUS_MASK);
}

void arch_sched_directed_ipi(uint32_t cpu_bitmap)
{
	send_ipi(SGI_SCHED_IPI, cpu_bitmap);
}

#ifdef CONFIG_USERSPACE
void mem_cfg_ipi_handler(const void *unused)
{
	ARG_UNUSED(unused);
	unsigned int key = arch_irq_lock();

	/*
	 * Make sure a domain switch by another CPU is effective on this CPU.
	 * This is a no-op if the page table is already the right one.
	 * Lock irq to prevent the interrupt during mem region switch.
	 */
	z_arm64_swap_mem_domains(_current);
	arch_irq_unlock(key);
}

void z_arm64_mem_cfg_ipi(void)
{
	send_ipi(SGI_MMCFG_IPI, IPI_ALL_CPUS_MASK);
}
#endif

#ifdef CONFIG_FPU_SHARING
void flush_fpu_ipi_handler(const void *unused)
{
	ARG_UNUSED(unused);

	disable_irq();
	arch_flush_local_fpu();
	/* no need to re-enable IRQs here */
}

void arch_flush_fpu_ipi(unsigned int cpu)
{
	const uint64_t mpidr = cpu_map[cpu];
	uint8_t aff0;

	if (mpidr == INV_MPID) {
		return;
	}

	aff0 = MPIDR_AFFLVL(mpidr, 0);
	gic_raise_sgi(SGI_FPU_IPI, mpidr, 1 << aff0);
}

/*
 * Make sure there is no pending FPU flush request for this CPU while
 * waiting for a contended spinlock to become available. This prevents
 * a deadlock when the lock we need is already taken by another CPU
 * that also wants its FPU content to be reinstated while such content
 * is still live in this CPU's FPU.
 */
void arch_spin_relax(void)
{
	if (arm_gic_irq_is_pending(SGI_FPU_IPI)) {
		arm_gic_irq_clear_pending(SGI_FPU_IPI);
		/*
		 * We may not be in IRQ context here hence cannot use
		 * arch_flush_local_fpu() directly.
		 */
		arch_float_disable(_current_cpu->arch.fpu_owner);
	}
}
#endif

int arch_smp_init(void)
{
	cpu_map[0] = MPIDR_TO_CORE(GET_MPIDR());

	/*
	 * SGI0 is use for sched ipi, this might be changed to use Kconfig
	 * option
	 */
	IRQ_CONNECT(SGI_SCHED_IPI, IRQ_DEFAULT_PRIORITY, sched_ipi_handler, NULL, 0);
	irq_enable(SGI_SCHED_IPI);

#ifdef CONFIG_USERSPACE
	IRQ_CONNECT(SGI_MMCFG_IPI, IRQ_DEFAULT_PRIORITY,
			mem_cfg_ipi_handler, NULL, 0);
	irq_enable(SGI_MMCFG_IPI);
#endif
#ifdef CONFIG_FPU_SHARING
	IRQ_CONNECT(SGI_FPU_IPI, IRQ_DEFAULT_PRIORITY, flush_fpu_ipi_handler, NULL, 0);
	irq_enable(SGI_FPU_IPI);
#endif

	return 0;
}

#endif
