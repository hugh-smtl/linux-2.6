/*
 * Xen SMP support
 *
 * This file implements the Xen versions of smp_ops.  SMP under Xen is
 * very straightforward.  Bringing a CPU up is simply a matter of
 * loading its initial context and setting it running.
 *
 * IPIs are handled through the Xen event mechanism.
 *
 * Because virtual CPUs can be scheduled onto any real CPU, there's no
 * useful topology information for the kernel to make use of.  As a
 * result, all CPUs are treated as if they're single-core and
 * single-threaded.
 *
 * This does not handle HOTPLUG_CPU yet.
 */
#include <linux/sched.h>
#include <linux/err.h>
#include <linux/smp.h>

#include <asm/paravirt.h>
#include <asm/desc.h>
#include <asm/pgtable.h>
#include <asm/cpu.h>

#include <xen/interface/xen.h>
#include <xen/interface/vcpu.h>

#include <asm/xen/interface.h>
#include <asm/xen/hypercall.h>

#include <xen/page.h>
#include <xen/events.h>

#include "xen-ops.h"
#include "mmu.h"

cpumask_t xen_cpu_initialized_map;

static DEFINE_PER_CPU(int, resched_irq);
static DEFINE_PER_CPU(int, callfunc_irq);
static DEFINE_PER_CPU(int, callfuncsingle_irq);
static DEFINE_PER_CPU(int, debug_irq) = -1;

static irqreturn_t xen_call_function_interrupt(int irq, void *dev_id);
static irqreturn_t xen_call_function_single_interrupt(int irq, void *dev_id);

/*
 * Reschedule call back. Nothing to do,
 * all the work is done automatically when
 * we return from the interrupt.
 */
static irqreturn_t xen_reschedule_interrupt(int irq, void *dev_id)
{
#ifdef CONFIG_X86_32
	__get_cpu_var(irq_stat).irq_resched_count++;
#else
	add_pda(irq_resched_count, 1);
#endif

	return IRQ_HANDLED;
}

static __cpuinit void cpu_bringup_and_idle(void)
{
	int cpu = smp_processor_id();

	cpu_init();
	xen_enable_sysenter();

	preempt_disable();
	per_cpu(cpu_state, cpu) = CPU_ONLINE;

	xen_setup_cpu_clockevents();

	/* We can take interrupts now: we're officially "up". */
	local_irq_enable();

	wmb();			/* make sure everything is out */
	cpu_idle();
}

static int xen_smp_intr_init(unsigned int cpu)
{
	int rc;
	const char *resched_name, *callfunc_name, *debug_name;

	resched_name = kasprintf(GFP_KERNEL, "resched%d", cpu);
	rc = bind_ipi_to_irqhandler(XEN_RESCHEDULE_VECTOR,
				    cpu,
				    xen_reschedule_interrupt,
				    IRQF_DISABLED|IRQF_PERCPU|IRQF_NOBALANCING,
				    resched_name,
				    NULL);
	if (rc < 0)
		goto fail;
	per_cpu(resched_irq, cpu) = rc;

	callfunc_name = kasprintf(GFP_KERNEL, "callfunc%d", cpu);
	rc = bind_ipi_to_irqhandler(XEN_CALL_FUNCTION_VECTOR,
				    cpu,
				    xen_call_function_interrupt,
				    IRQF_DISABLED|IRQF_PERCPU|IRQF_NOBALANCING,
				    callfunc_name,
				    NULL);
	if (rc < 0)
		goto fail;
	per_cpu(callfunc_irq, cpu) = rc;

	debug_name = kasprintf(GFP_KERNEL, "debug%d", cpu);
	rc = bind_virq_to_irqhandler(VIRQ_DEBUG, cpu, xen_debug_interrupt,
				     IRQF_DISABLED | IRQF_PERCPU | IRQF_NOBALANCING,
				     debug_name, NULL);
	if (rc < 0)
		goto fail;
	per_cpu(debug_irq, cpu) = rc;

	callfunc_name = kasprintf(GFP_KERNEL, "callfuncsingle%d", cpu);
	rc = bind_ipi_to_irqhandler(XEN_CALL_FUNCTION_SINGLE_VECTOR,
				    cpu,
				    xen_call_function_single_interrupt,
				    IRQF_DISABLED|IRQF_PERCPU|IRQF_NOBALANCING,
				    callfunc_name,
				    NULL);
	if (rc < 0)
		goto fail;
	per_cpu(callfuncsingle_irq, cpu) = rc;

	return 0;

 fail:
	if (per_cpu(resched_irq, cpu) >= 0)
		unbind_from_irqhandler(per_cpu(resched_irq, cpu), NULL);
	if (per_cpu(callfunc_irq, cpu) >= 0)
		unbind_from_irqhandler(per_cpu(callfunc_irq, cpu), NULL);
	if (per_cpu(debug_irq, cpu) >= 0)
		unbind_from_irqhandler(per_cpu(debug_irq, cpu), NULL);
	if (per_cpu(callfuncsingle_irq, cpu) >= 0)
		unbind_from_irqhandler(per_cpu(callfuncsingle_irq, cpu), NULL);

	return rc;
}

void __init xen_fill_possible_map(void)
{
	int i, rc;

	for (i = 0; i < NR_CPUS; i++) {
		rc = HYPERVISOR_vcpu_op(VCPUOP_is_up, i, NULL);
		if (rc >= 0)
			cpu_set(i, cpu_possible_map);
	}
}

void __init xen_smp_prepare_boot_cpu(void)
{
	int cpu;

	BUG_ON(smp_processor_id() != 0);
	native_smp_prepare_boot_cpu();

	/* We've switched to the "real" per-cpu gdt, so make sure the
	   old memory can be recycled */
	make_lowmem_page_readwrite(&per_cpu__gdt_page);

	for_each_possible_cpu(cpu) {
		cpus_clear(per_cpu(cpu_sibling_map, cpu));
		/*
		 * cpu_core_map lives in a per cpu area that is cleared
		 * when the per cpu array is allocated.
		 *
		 * cpus_clear(per_cpu(cpu_core_map, cpu));
		 */
	}

	xen_setup_vcpu_info_placement();
}

void __init xen_smp_prepare_cpus(unsigned int max_cpus)
{
	unsigned cpu;

	for_each_possible_cpu(cpu) {
		cpus_clear(per_cpu(cpu_sibling_map, cpu));
		/*
		 * cpu_core_ map will be zeroed when the per
		 * cpu area is allocated.
		 *
		 * cpus_clear(per_cpu(cpu_core_map, cpu));
		 */
	}

	smp_store_cpu_info(0);
	set_cpu_sibling_map(0);

	if (xen_smp_intr_init(0))
		BUG();

	xen_cpu_initialized_map = cpumask_of_cpu(0);

	/* Restrict the possible_map according to max_cpus. */
	while ((num_possible_cpus() > 1) && (num_possible_cpus() > max_cpus)) {
		for (cpu = NR_CPUS - 1; !cpu_possible(cpu); cpu--)
			continue;
		cpu_clear(cpu, cpu_possible_map);
	}

	for_each_possible_cpu (cpu) {
		struct task_struct *idle;

		if (cpu == 0)
			continue;

		idle = fork_idle(cpu);
		if (IS_ERR(idle))
			panic("failed fork for CPU %d", cpu);

		cpu_set(cpu, cpu_present_map);
	}

	//init_xenbus_allowed_cpumask();
}

static __cpuinit int
cpu_initialize_context(unsigned int cpu, struct task_struct *idle)
{
	struct vcpu_guest_context *ctxt;
	struct gdt_page *gdt = &per_cpu(gdt_page, cpu);

	if (cpu_test_and_set(cpu, xen_cpu_initialized_map))
		return 0;

	ctxt = kzalloc(sizeof(*ctxt), GFP_KERNEL);
	if (ctxt == NULL)
		return -ENOMEM;

	ctxt->flags = VGCF_IN_KERNEL;
	ctxt->user_regs.ds = __USER_DS;
	ctxt->user_regs.es = __USER_DS;
	ctxt->user_regs.fs = __KERNEL_PERCPU;
	ctxt->user_regs.gs = 0;
	ctxt->user_regs.ss = __KERNEL_DS;
	ctxt->user_regs.eip = (unsigned long)cpu_bringup_and_idle;
	ctxt->user_regs.eflags = 0x1000; /* IOPL_RING1 */

	memset(&ctxt->fpu_ctxt, 0, sizeof(ctxt->fpu_ctxt));

	xen_copy_trap_info(ctxt->trap_ctxt);

	ctxt->ldt_ents = 0;

	BUG_ON((unsigned long)gdt->gdt & ~PAGE_MASK);
	make_lowmem_page_readonly(gdt->gdt);

	ctxt->gdt_frames[0] = virt_to_mfn(gdt->gdt);
	ctxt->gdt_ents      = ARRAY_SIZE(gdt->gdt);

	ctxt->user_regs.cs = __KERNEL_CS;
	ctxt->user_regs.esp = idle->thread.sp0 - sizeof(struct pt_regs);

	ctxt->kernel_ss = __KERNEL_DS;
	ctxt->kernel_sp = idle->thread.sp0;

	ctxt->event_callback_cs     = __KERNEL_CS;
	ctxt->event_callback_eip    = (unsigned long)xen_hypervisor_callback;
	ctxt->failsafe_callback_cs  = __KERNEL_CS;
	ctxt->failsafe_callback_eip = (unsigned long)xen_failsafe_callback;

	per_cpu(xen_cr3, cpu) = __pa(swapper_pg_dir);
	ctxt->ctrlreg[3] = xen_pfn_to_cr3(virt_to_mfn(swapper_pg_dir));

	if (HYPERVISOR_vcpu_op(VCPUOP_initialise, cpu, ctxt))
		BUG();

	kfree(ctxt);
	return 0;
}

int __cpuinit xen_cpu_up(unsigned int cpu)
{
	struct task_struct *idle = idle_task(cpu);
	int rc;

#if 0
	rc = cpu_up_check(cpu);
	if (rc)
		return rc;
#endif

	init_gdt(cpu);
	per_cpu(current_task, cpu) = idle;
	irq_ctx_init(cpu);
	xen_setup_timer(cpu);

	/* make sure interrupts start blocked */
	per_cpu(xen_vcpu, cpu)->evtchn_upcall_mask = 1;

	rc = cpu_initialize_context(cpu, idle);
	if (rc)
		return rc;

	if (num_online_cpus() == 1)
		alternatives_smp_switch(1);

	rc = xen_smp_intr_init(cpu);
	if (rc)
		return rc;

	smp_store_cpu_info(cpu);
	set_cpu_sibling_map(cpu);
	/* This must be done before setting cpu_online_map */
	wmb();

	cpu_set(cpu, cpu_online_map);

	rc = HYPERVISOR_vcpu_op(VCPUOP_up, cpu, NULL);
	BUG_ON(rc);

	return 0;
}

void xen_smp_cpus_done(unsigned int max_cpus)
{
}

static void stop_self(void *v)
{
	int cpu = smp_processor_id();

	/* make sure we're not pinning something down */
	load_cr3(swapper_pg_dir);
	/* should set up a minimal gdt */

	HYPERVISOR_vcpu_op(VCPUOP_down, cpu, NULL);
	BUG();
}

void xen_smp_send_stop(void)
{
	smp_call_function(stop_self, NULL, 0);
}

void xen_smp_send_reschedule(int cpu)
{
	xen_send_IPI_one(cpu, XEN_RESCHEDULE_VECTOR);
}

static void xen_send_IPI_mask(cpumask_t mask, enum ipi_vector vector)
{
	unsigned cpu;

	cpus_and(mask, mask, cpu_online_map);

	for_each_cpu_mask(cpu, mask)
		xen_send_IPI_one(cpu, vector);
}

void xen_smp_send_call_function_ipi(cpumask_t mask)
{
	int cpu;

	xen_send_IPI_mask(mask, XEN_CALL_FUNCTION_VECTOR);

	/* Make sure other vcpus get a chance to run if they need to. */
	for_each_cpu_mask(cpu, mask) {
		if (xen_vcpu_stolen(cpu)) {
			HYPERVISOR_sched_op(SCHEDOP_yield, 0);
			break;
		}
	}
}

void xen_smp_send_call_function_single_ipi(int cpu)
{
	xen_send_IPI_mask(cpumask_of_cpu(cpu), XEN_CALL_FUNCTION_SINGLE_VECTOR);
}

static irqreturn_t xen_call_function_interrupt(int irq, void *dev_id)
{
	irq_enter();
	generic_smp_call_function_interrupt();
	__get_cpu_var(irq_stat).irq_call_count++;
	irq_exit();

	return IRQ_HANDLED;
}

static irqreturn_t xen_call_function_single_interrupt(int irq, void *dev_id)
{
	irq_enter();
	generic_smp_call_function_single_interrupt();
	__get_cpu_var(irq_stat).irq_call_count++;
	irq_exit();

	return IRQ_HANDLED;
}
