/*
 * Copyright 2020, Data61, CSIRO (ABN 41 687 119 230)
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <autoconf.h>
#include <elfloader/gen_config.h>
#include <devices_gen.h>
#include <drivers/smp.h>

#include <printf.h>
#include <cpuid.h>
#include <abort.h>

#include <elfloader.h>
#include <armv/smp.h>
#include <armv/machine.h>
#include <mode/structures.h>

#if CONFIG_MAX_NUM_NODES > 1
static volatile int non_boot_lock = 0;
extern volatile int core_up[CONFIG_MAX_NUM_NODES];

void arm_disable_dcaches(void);

extern struct image_info kernel_info[CONFIG_MAX_NUM_NODES];
extern struct image_info user_info[CONFIG_MAX_NUM_NODES];
extern void const *dtb[CONFIG_MAX_NUM_NODES];
extern size_t dtb_size[CONFIG_MAX_NUM_NODES];


WEAK void non_boot_init(void) {}

/* Entry point for all CPUs other than the initial. */
void non_boot_main(word_t id)
{
#ifndef CONFIG_ARCH_AARCH64
    arm_disable_dcaches();
#endif

    /* Initialise any platform-specific per-core state */
    non_boot_init();

#ifndef CONFIG_ARM_HYPERVISOR_SUPPORT
    if (is_hyp_mode()) {
        extern void leave_hyp(void);
        leave_hyp();
    }
#endif
    word_t mpidr = read_cpuid_mpidr();
    printf("Booting cpu id = 0x%x, index=%d\n", mpidr, id);


    unsigned int num_apps = 0;
    int ret = load_images(&kernel_info[id], &user_info[id], 1, &num_apps,
                          NULL, &dtb[id], &dtb_size[id], id);
    if (0 != ret) {
        printf("ERROR: image loading failed\n");
        abort();
    }

    /* Setup MMU. */
    if (is_hyp_mode()) {
#ifdef CONFIG_ARCH_AARCH64
        extern void disable_caches_hyp();
        disable_caches_hyp();
#endif
        init_hyp_boot_vspace(&kernel_info[id], id);
    } else {
        /* If we are not in HYP mode, we enable the SV MMU and paging
         * just in case the kernel does not support hyp mode. */
        init_boot_vspace(&kernel_info[id], id);
    }

    /* Enable the MMU, and enter the kernel. */
    if (is_hyp_mode()) {
#ifdef CONFIG_ARCH_AARCH64
        arm_enable_hyp_mmu((word_t)_boot_pgd_down[id]);
#else
        pd_node_id = id;
        arm_enable_hyp_mmu();
#endif
    } else {
#ifdef CONFIG_ARCH_AARCH64
        arm_enable_mmu((word_t)_boot_pgd_up[id], (word_t)_boot_pgd_down[id]);
#else
        pd_node_id = id;
        arm_enable_mmu();
#endif
    }
    printf("jump to kernel %lx %lx\n", kernel_info[id].virt_entry, user_info[id].phys_region_start);

    // Signal that we've initialized this core.
    dsb();
    core_up[id] = id;
    dsb();

    /* Jump to the kernel. */
    ((init_arm_kernel_t)kernel_info[id].virt_entry)(user_info[id].phys_region_start,
                                                user_info[id].phys_region_end, user_info[id].phys_virt_offset,
                                                user_info[id].virt_entry, (paddr_t)dtb[id], dtb_size[id]);

    printf("AP Kernel returned back to the elf-loader.\n");
    abort();
}

/* TODO: convert imx7 to driver model and remove WEAK */
WEAK void init_cpus(void)
{
    /*
     * first, figure out which CPU we're booting on.
     */
    int booting_cpu_index = -1;
    word_t mpidr = read_cpuid_mpidr();
    int i;

    for (i = 0; elfloader_cpus[i].compat != NULL; i++) {
        if (elfloader_cpus[i].cpu_id == mpidr) {
            booting_cpu_index = i;
            break;
        }
    }

    if (booting_cpu_index == -1) {
        printf("Could not find cpu entry for boot cpu (mpidr=0x%x)\n", mpidr);
        abort();
    }

    printf("Booting cpu id = 0x%x, index=%d\n", mpidr, booting_cpu_index);
    /*
     * We want to boot CPUs in the same cluster before we boot CPUs in another cluster.
     * This is important on systems like TX2, where the system boots on the A57 cluster,
     * even though the Denver cluster is the "first" cluster according to the mpidr registers.
     *
     * There are a couple of assumptions made here:
     *  1. The elfloader_cpus array is ordered based on the cpuid field (guaranteed by hardware_gen).
     *  2. The CPU we boot on is the first CPU in a cluster (not necessarily the first cluster).
     */
    int start_index = booting_cpu_index;

    int num_cpus = 1;
    for (i = start_index + 1; num_cpus < CONFIG_MAX_NUM_NODES && i != start_index; i++) {
        if (i == booting_cpu_index) {
            continue;
        }
        if (elfloader_cpus[i].compat == NULL) {
            /* move back to start of the array if we started somewhere in the middle */
            i = -1;
            continue;
        }
        int ret = plat_cpu_on(&elfloader_cpus[i], core_entry, &core_stacks[num_cpus][0]);
        if (ret != 0) {
            printf("Failed to boot cpu 0x%x: %d\n", elfloader_cpus[i].cpu_id, ret);
            abort();
        }

        while (!is_core_up(num_cpus));
        printf("Core %d is up with logic id %d\n", elfloader_cpus[i].cpu_id, num_cpus);
        num_cpus++;
    }

#ifdef CONFIG_ARCH_AARCH64
    /* main CPU has thread id == 0 */
    MSR("tpidr_el1", 0);
#endif
}

void smp_boot(void)
{
#ifndef CONFIG_ARCH_AARCH64
    arm_disable_dcaches();
#endif
    init_cpus();

}
#endif /* CONFIG_MAX_NUM_NODES */
