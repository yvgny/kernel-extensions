/*
 * Copyright (C) 2015 - ARM Ltd
 * Author: Marc Zyngier <marc.zyngier@arm.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/types.h>
#include <linux/jump_label.h>
#include <uapi/linux/psci.h>

#include <kvm/arm_psci.h>

#include <asm/kvm_asm.h>
#include <asm/kvm_emulate.h>
#include <asm/kvm_hyp.h>
#include <asm/fpsimd.h>
#include <asm/debug-monitors.h>

static bool __hyp_text __fpsimd_enabled_nvhe(void)
{
	return !(read_sysreg(cptr_el2) & CPTR_EL2_TFP);
}

static bool __hyp_text __fpsimd_enabled_vhe(void)
{
	return !!(read_sysreg(cpacr_el1) & CPACR_EL1_FPEN);
}

static hyp_alternate_select(__fpsimd_is_enabled,
			    __fpsimd_enabled_nvhe, __fpsimd_enabled_vhe,
			    ARM64_HAS_VIRT_HOST_EXTN);

bool __hyp_text __fpsimd_enabled(void)
{
	return __fpsimd_is_enabled()();
}

static void __hyp_text __activate_traps_vhe(void)
{
	u64 val;

	val = read_sysreg(cpacr_el1);
	val |= CPACR_EL1_TTA;
	val &= ~(CPACR_EL1_FPEN | CPACR_EL1_ZEN);
	write_sysreg(val, cpacr_el1);

	write_sysreg(kvm_get_hyp_vector(), vbar_el1);
}

static void __hyp_text __activate_traps_nvhe(void)
{
	u64 val;

	val = CPTR_EL2_DEFAULT;
	val |= CPTR_EL2_TTA | CPTR_EL2_TFP | CPTR_EL2_TZ;
	write_sysreg(val, cptr_el2);
}

static hyp_alternate_select(__activate_traps_arch,
			    __activate_traps_nvhe, __activate_traps_vhe,
			    ARM64_HAS_VIRT_HOST_EXTN);

static void __hyp_text __activate_traps(struct kvm_vcpu *vcpu)
{
	u64 val;

	/*
	 * We are about to set CPTR_EL2.TFP to trap all floating point
	 * register accesses to EL2, however, the ARM ARM clearly states that
	 * traps are only taken to EL2 if the operation would not otherwise
	 * trap to EL1.  Therefore, always make sure that for 32-bit guests,
	 * we set FPEXC.EN to prevent traps to EL1, when setting the TFP bit.
	 * If FP/ASIMD is not implemented, FPEXC is UNDEFINED and any access to
	 * it will cause an exception.
	 */
	val = vcpu->arch.hcr_el2;

	if (!(val & HCR_RW) && system_supports_fpsimd()) {
		write_sysreg(1 << 30, fpexc32_el2);
		isb();
	}

	if (val & HCR_RW) /* for AArch64 only: */
		val |= HCR_TID3; /* TID3: trap feature register accesses */

	write_sysreg(val, hcr_el2);

	/* Trap on AArch32 cp15 c15 accesses (EL1 or EL0) */
	write_sysreg(1 << 15, hstr_el2);
	/*
	 * Make sure we trap PMU access from EL0 to EL2. Also sanitize
	 * PMSELR_EL0 to make sure it never contains the cycle
	 * counter, which could make a PMXEVCNTR_EL0 access UNDEF at
	 * EL1 instead of being trapped to EL2.
	 */
	write_sysreg(0, pmselr_el0);
	write_sysreg(ARMV8_PMU_USERENR_MASK, pmuserenr_el0);
	write_sysreg(vcpu->arch.mdcr_el2, mdcr_el2);
	__activate_traps_arch()();
}

static void __hyp_text __deactivate_traps_vhe(void)
{
	extern char vectors[];	/* kernel exception vectors */
	u64 mdcr_el2 = read_sysreg(mdcr_el2);

	mdcr_el2 &= MDCR_EL2_HPMN_MASK |
		    MDCR_EL2_E2PB_MASK << MDCR_EL2_E2PB_SHIFT |
		    MDCR_EL2_TPMS;

	write_sysreg(mdcr_el2, mdcr_el2);
	write_sysreg(HCR_HOST_VHE_FLAGS, hcr_el2);
	write_sysreg(CPACR_EL1_DEFAULT, cpacr_el1);
	write_sysreg(vectors, vbar_el1);
}

static void __hyp_text __deactivate_traps_nvhe(void)
{
	u64 mdcr_el2 = read_sysreg(mdcr_el2);

	mdcr_el2 &= MDCR_EL2_HPMN_MASK;
	mdcr_el2 |= MDCR_EL2_E2PB_MASK << MDCR_EL2_E2PB_SHIFT;

	write_sysreg(mdcr_el2, mdcr_el2);
	write_sysreg(HCR_RW, hcr_el2);
	write_sysreg(CPTR_EL2_DEFAULT, cptr_el2);
}

static hyp_alternate_select(__deactivate_traps_arch,
			    __deactivate_traps_nvhe, __deactivate_traps_vhe,
			    ARM64_HAS_VIRT_HOST_EXTN);

static void __hyp_text __deactivate_traps(struct kvm_vcpu *vcpu)
{
	/*
	 * If we pended a virtual abort, preserve it until it gets
	 * cleared. See D1.14.3 (Virtual Interrupts) for details, but
	 * the crucial bit is "On taking a vSError interrupt,
	 * HCR_EL2.VSE is cleared to 0."
	 */
	if (vcpu->arch.hcr_el2 & HCR_VSE)
		vcpu->arch.hcr_el2 = read_sysreg(hcr_el2);

	__deactivate_traps_arch()();
	write_sysreg(0, hstr_el2);
	write_sysreg(0, pmuserenr_el0);
}

static void __hyp_text __activate_vm(struct kvm_vcpu *vcpu)
{
	struct kvm *kvm = kern_hyp_va(vcpu->kvm);
	write_sysreg(kvm->arch.vttbr, vttbr_el2);
}

static void __hyp_text __deactivate_vm(struct kvm_vcpu *vcpu)
{
	write_sysreg(0, vttbr_el2);
}

static void __hyp_text __vgic_save_state(struct kvm_vcpu *vcpu)
{
	if (static_branch_unlikely(&kvm_vgic_global_state.gicv3_cpuif))
		__vgic_v3_save_state(vcpu);
	else
		__vgic_v2_save_state(vcpu);

	write_sysreg(read_sysreg(hcr_el2) & ~HCR_INT_OVERRIDE, hcr_el2);
}

static void __hyp_text __vgic_restore_state(struct kvm_vcpu *vcpu)
{
	u64 val;

	val = read_sysreg(hcr_el2);
	val |= 	HCR_INT_OVERRIDE;
	val |= vcpu->arch.irq_lines;
	write_sysreg(val, hcr_el2);

	if (static_branch_unlikely(&kvm_vgic_global_state.gicv3_cpuif))
		__vgic_v3_restore_state(vcpu);
	else
		__vgic_v2_restore_state(vcpu);
}

static bool __hyp_text __true_value(void)
{
	return true;
}

static bool __hyp_text __false_value(void)
{
	return false;
}

static hyp_alternate_select(__check_arm_834220,
			    __false_value, __true_value,
			    ARM64_WORKAROUND_834220);

static bool __hyp_text __translate_far_to_hpfar(u64 far, u64 *hpfar)
{
	u64 par, tmp;

	/*
	 * Resolve the IPA the hard way using the guest VA.
	 *
	 * Stage-1 translation already validated the memory access
	 * rights. As such, we can use the EL1 translation regime, and
	 * don't have to distinguish between EL0 and EL1 access.
	 *
	 * We do need to save/restore PAR_EL1 though, as we haven't
	 * saved the guest context yet, and we may return early...
	 */
	par = read_sysreg(par_el1);
	asm volatile("at s1e1r, %0" : : "r" (far));
	isb();

	tmp = read_sysreg(par_el1);
	write_sysreg(par, par_el1);

	if (unlikely(tmp & 1))
		return false; /* Translation failed, back to guest */

	/* Convert PAR to HPFAR format */
	*hpfar = ((tmp >> 12) & ((1UL << 36) - 1)) << 4;
	return true;
}

static bool __hyp_text __populate_fault_info(struct kvm_vcpu *vcpu)
{
	u64 esr = read_sysreg_el2(esr);
	u8 ec = ESR_ELx_EC(esr);
	u64 hpfar, far;

	vcpu->arch.fault.esr_el2 = esr;

	if (ec != ESR_ELx_EC_DABT_LOW && ec != ESR_ELx_EC_IABT_LOW)
		return true;

	far = read_sysreg_el2(far);

	/*
	 * The HPFAR can be invalid if the stage 2 fault did not
	 * happen during a stage 1 page table walk (the ESR_EL2.S1PTW
	 * bit is clear) and one of the two following cases are true:
	 *   1. The fault was due to a permission fault
	 *   2. The processor carries errata 834220
	 *
	 * Therefore, for all non S1PTW faults where we either have a
	 * permission fault or the errata workaround is enabled, we
	 * resolve the IPA using the AT instruction.
	 */
	if (!(esr & ESR_ELx_S1PTW) &&
	    (__check_arm_834220()() || (esr & ESR_ELx_FSC_TYPE) == FSC_PERM)) {
		if (!__translate_far_to_hpfar(far, &hpfar))
			return false;
	} else {
		hpfar = read_sysreg(hpfar_el2);
	}

	vcpu->arch.fault.far_el2 = far;
	vcpu->arch.fault.hpfar_el2 = hpfar;
	return true;
}

/* Skip an instruction which has been emulated. Returns true if
 * execution can continue or false if we need to exit hyp mode because
 * single-step was in effect.
 */
static bool __hyp_text __skip_instr(struct kvm_vcpu *vcpu)
{
	*vcpu_pc(vcpu) = read_sysreg_el2(elr);

	if (vcpu_mode_is_32bit(vcpu)) {
		vcpu->arch.ctxt.gp_regs.regs.pstate = read_sysreg_el2(spsr);
		kvm_skip_instr32(vcpu, kvm_vcpu_trap_il_is32bit(vcpu));
		write_sysreg_el2(vcpu->arch.ctxt.gp_regs.regs.pstate, spsr);
	} else {
		*vcpu_pc(vcpu) += 4;
	}

	write_sysreg_el2(*vcpu_pc(vcpu), elr);

	if (vcpu->guest_debug & KVM_GUESTDBG_SINGLESTEP) {
		vcpu->arch.fault.esr_el2 =
			(ESR_ELx_EC_SOFTSTP_LOW << ESR_ELx_EC_SHIFT) | 0x22;
		return false;
	} else {
		return true;
	}
}

int __hyp_text __kvm_vcpu_run(struct kvm_vcpu *vcpu)
{
	struct kvm_cpu_context *host_ctxt;
	struct kvm_cpu_context *guest_ctxt;
	bool fp_enabled;
	u64 exit_code;

	vcpu = kern_hyp_va(vcpu);
	write_sysreg(vcpu, tpidr_el2);

	host_ctxt = kern_hyp_va(vcpu->arch.host_cpu_context);
	guest_ctxt = &vcpu->arch.ctxt;

	__sysreg_save_host_state(host_ctxt);
	__debug_cond_save_host_state(vcpu);

	__activate_traps(vcpu);
	__activate_vm(vcpu);

	__vgic_restore_state(vcpu);
	__timer_enable_traps(vcpu);

	/*
	 * We must restore the 32-bit state before the sysregs, thanks
	 * to erratum #852523 (Cortex-A57) or #853709 (Cortex-A72).
	 */
	__sysreg32_restore_state(vcpu);
	__sysreg_restore_guest_state(guest_ctxt);
	__debug_restore_state(vcpu, kern_hyp_va(vcpu->arch.debug_ptr), guest_ctxt);

	/* Jump in the fire! */
again:
	exit_code = __guest_enter(vcpu, host_ctxt);
	/* And we're baaack! */

	/*
	 * We're using the raw exception code in order to only process
	 * the trap if no SError is pending. We will come back to the
	 * same PC once the SError has been injected, and replay the
	 * trapping instruction.
	 */
	if (exit_code == ARM_EXCEPTION_TRAP && !__populate_fault_info(vcpu))
		goto again;

	if (static_branch_unlikely(&vgic_v2_cpuif_trap) &&
	    exit_code == ARM_EXCEPTION_TRAP) {
		bool valid;

		valid = kvm_vcpu_trap_get_class(vcpu) == ESR_ELx_EC_DABT_LOW &&
			kvm_vcpu_trap_get_fault_type(vcpu) == FSC_FAULT &&
			kvm_vcpu_dabt_isvalid(vcpu) &&
			!kvm_vcpu_dabt_isextabt(vcpu) &&
			!kvm_vcpu_dabt_iss1tw(vcpu);

		if (valid) {
			int ret = __vgic_v2_perform_cpuif_access(vcpu);

			if (ret == 1) {
				if (__skip_instr(vcpu))
					goto again;
				else
					exit_code = ARM_EXCEPTION_TRAP;
			}

			if (ret == -1) {
				/* Promote an illegal access to an
				 * SError. If we would be returning
				 * due to single-step clear the SS
				 * bit so handle_exit knows what to
				 * do after dealing with the error.
				 */
				if (!__skip_instr(vcpu))
					*vcpu_cpsr(vcpu) &= ~DBG_SPSR_SS;
				exit_code = ARM_EXCEPTION_EL1_SERROR;
			}

			/* 0 falls through to be handler out of EL2 */
		}
	}

	if (static_branch_unlikely(&vgic_v3_cpuif_trap) &&
	    exit_code == ARM_EXCEPTION_TRAP &&
	    (kvm_vcpu_trap_get_class(vcpu) == ESR_ELx_EC_SYS64 ||
	     kvm_vcpu_trap_get_class(vcpu) == ESR_ELx_EC_CP15_32)) {
		int ret = __vgic_v3_perform_cpuif_access(vcpu);

		if (ret == 1) {
			if (__skip_instr(vcpu))
				goto again;
			else
				exit_code = ARM_EXCEPTION_TRAP;
		}

		/* 0 falls through to be handled out of EL2 */
	}

	if (cpus_have_const_cap(ARM64_HARDEN_BP_POST_GUEST_EXIT)) {
		u32 midr = read_cpuid_id();

		/* Apply BTAC predictors mitigation to all Falkor chips */
		if ((midr & MIDR_CPU_MODEL_MASK) == MIDR_QCOM_FALKOR_V1)
			__qcom_hyp_sanitize_btac_predictors();
	}

	fp_enabled = __fpsimd_enabled();

	__sysreg_save_guest_state(guest_ctxt);
	__sysreg32_save_state(vcpu);
	__timer_disable_traps(vcpu);
	__vgic_save_state(vcpu);

	__deactivate_traps(vcpu);
	__deactivate_vm(vcpu);

	__sysreg_restore_host_state(host_ctxt);

	if (fp_enabled) {
		__fpsimd_save_state(&guest_ctxt->gp_regs.fp_regs);
		__fpsimd_restore_state(&host_ctxt->gp_regs.fp_regs);
	}

	__debug_save_state(vcpu, kern_hyp_va(vcpu->arch.debug_ptr), guest_ctxt);
	/*
	 * This must come after restoring the host sysregs, since a non-VHE
	 * system may enable SPE here and make use of the TTBRs.
	 */
	__debug_cond_restore_host_state(vcpu);

	return exit_code;
}

static const char __hyp_panic_string[] = "HYP panic:\nPS:%08llx PC:%016llx ESR:%08llx\nFAR:%016llx HPFAR:%016llx PAR:%016llx\nVCPU:%p\n";

static void __hyp_text __hyp_call_panic_nvhe(u64 spsr, u64 elr, u64 par)
{
	unsigned long str_va;

	/*
	 * Force the panic string to be loaded from the literal pool,
	 * making sure it is a kernel address and not a PC-relative
	 * reference.
	 */
	asm volatile("ldr %0, =__hyp_panic_string" : "=r" (str_va));

	__hyp_do_panic(str_va,
		       spsr,  elr,
		       read_sysreg(esr_el2),   read_sysreg_el2(far),
		       read_sysreg(hpfar_el2), par,
		       (void *)read_sysreg(tpidr_el2));
}

static void __hyp_text __hyp_call_panic_vhe(u64 spsr, u64 elr, u64 par)
{
	panic(__hyp_panic_string,
	      spsr,  elr,
	      read_sysreg_el2(esr),   read_sysreg_el2(far),
	      read_sysreg(hpfar_el2), par,
	      (void *)read_sysreg(tpidr_el2));
}

static hyp_alternate_select(__hyp_call_panic,
			    __hyp_call_panic_nvhe, __hyp_call_panic_vhe,
			    ARM64_HAS_VIRT_HOST_EXTN);

void __hyp_text __noreturn __hyp_panic(void)
{
	u64 spsr = read_sysreg_el2(spsr);
	u64 elr = read_sysreg_el2(elr);
	u64 par = read_sysreg(par_el1);

	if (read_sysreg(vttbr_el2)) {
		struct kvm_vcpu *vcpu;
		struct kvm_cpu_context *host_ctxt;

		vcpu = (struct kvm_vcpu *)read_sysreg(tpidr_el2);
		host_ctxt = kern_hyp_va(vcpu->arch.host_cpu_context);
		__timer_disable_traps(vcpu);
		__deactivate_traps(vcpu);
		__deactivate_vm(vcpu);
		__sysreg_restore_host_state(host_ctxt);
	}

	/* Call panic for real */
	__hyp_call_panic()(spsr, elr, par);

	unreachable();
}
