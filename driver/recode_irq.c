/* TODO Debug */
// #include <asm/irq_regs.h>
#include <asm/apic.h>

#include "dependencies.h"
#include "recode.h"

DEFINE_PER_CPU(unsigned long, pcpu_pmi_counter) = 0;
DEFINE_PER_CPU(u64, pcpu_reset_period);

unsigned __read_mostly max_pmi_before_ctx = (1ULL << 13); // 8192


int pmi_recode(void)
{
	u64 global;
	unsigned handled = 0, loops = 0;
	unsigned long flags = 0;
	/* TODO Debug */
	// struct pt_regs *regs;

	atomic_inc(&active_pmis);
	/* Not sure we need this */
	local_irq_save(flags);

	/* Inc every time this handler fires */
	this_cpu_inc(pcpu_pmi_counter);

	/** TODO Create a tuning method
	 *
	 * We should borrow the tuning method from perf to avoid to make the
	 * system experience throttling issues.
	 * At the moment, this mechanims is hardcoded according to some values
	 * computed during experimental phase.
	 */

	if (this_cpu_read(pcpu_pmi_counter) > max_pmi_before_ctx) {
		u64 nrs = PMC_TRIM(this_cpu_read(pcpu_reset_period) << 4);
		pr_warn("[%u] Too many PMIs before CTX\n", smp_processor_id());

		/* Try to atomically adjust the reset period */
		if (__sync_bool_compare_and_swap(&reset_period, 
			this_cpu_read(pcpu_reset_period), nrs)) {
			
			pr_warn("[%u] Reset_period updated to: %llx\n", 
				smp_processor_id(), nrs);

			/* Give the System a grace time */
			this_cpu_sub(pcpu_pmi_counter, 
				this_cpu_read(pcpu_pmi_counter) >> 1);
		} else {
			pr_warn("[%u] Concurrent update, aborted\n",
				smp_processor_id());
		}
	}

	/* Safe Guard ? */
	if (recode_state == OFF)
		goto end;

	/* Read the PMCs state */
	rdmsrl(MSR_CORE_PERF_GLOBAL_STATUS, global);

	/* Nothing to do here */
	if (!global) {
		pr_info("[%u] Got PMI %llx - NO GLOBAL - FIXED&u %llx\n", 
		smp_processor_id(), global, fixed_pmc_pmi,
		READ_FIXED_PMC(fixed_pmc_pmi));
		goto no_pmi;
	}

	/* This IRQ is not originated from PMC overflow */
	if(!(global & (PERF_GLOBAL_CTRL_FIXED0_MASK << fixed_pmc_pmi))) {
		pr_warn("Something triggered pmc_detection_interrupt line\n\
			MSR_CORE_PERF_GLOBAL_STATUS: %llx\n", global);
		goto no_pmi;
	}
	
	/* 
	 * The current implementation of this function does not
	 * provide a sliding window for a discrete samples collection.
	 * If a PMI arises, it means that there is a pmc multiplexing
	 * request. 	 
	 */

again:
	if (++loops > 100) {
		wrmsrl(MSR_CORE_PERF_GLOBAL_CTRL, 0ULL);
		pr_warn("[%u] LOOP STUCK - STOP PMCs\n", smp_processor_id());
		goto end;
	}

	/* TODO Debug */
	// regs = __this_cpu_read(irq_regs);
	// pr_warn("[%u] CS: %lx\n", smp_processor_id(), regs->cs);

	pmc_evaluate_activity(current, is_pid_tracked(current->tgid), false);

	handled++;

	/* Backup last used reset_period */
	this_cpu_write(pcpu_reset_period, reset_period);
	WRITE_FIXED_PMC(fixed_pmc_pmi, this_cpu_read(pcpu_reset_period));
no_pmi:

	apic_write(APIC_LVTPC, RECODE_PMI);

	if (global) {
		wrmsrl(MSR_CORE_PERF_GLOBAL_OVF_CTRL, global);
	} else { 
		/* See arc/x86/intel/events/core.c:intel_pmu_handle_irq_v4 */
		rdmsrl(MSR_CORE_PERF_GLOBAL_STATUS, global);
		pr_warn("PMI end - GLOBAL: %llx\n", global);
		
		goto again;
	}

end:
	apic_eoi();
	
	/* Not sure we need this */
	local_irq_restore(flags);

	atomic_dec(&active_pmis);

        return handled;
}// handle_ibs_event