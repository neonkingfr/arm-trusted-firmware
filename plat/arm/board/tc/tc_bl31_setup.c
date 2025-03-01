/*
 * Copyright (c) 2020-2024, Arm Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <assert.h>

#include <libfdt.h>
#include <tc_plat.h>

#include <arch_helpers.h>
#include <common/bl_common.h>
#include <common/debug.h>
#include <drivers/arm/css/css_mhu_doorbell.h>
#include <drivers/arm/css/scmi.h>
#include <drivers/arm/sbsa.h>
#include <lib/fconf/fconf.h>
#include <lib/fconf/fconf_dyn_cfg_getter.h>
#include <plat/arm/common/plat_arm.h>
#include <plat/common/platform.h>

#ifdef PLATFORM_TEST_TFM_TESTSUITE
#include <psa/crypto_platform.h>
#include <psa/crypto_types.h>
#include <psa/crypto_values.h>
#endif /* PLATFORM_TEST_TFM_TESTSUITE */

#ifdef PLATFORM_TEST_TFM_TESTSUITE
/*
 * We pretend using an external RNG (through MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG
 * mbedTLS config option) so we need to provide an implementation of
 * mbedtls_psa_external_get_random(). Provide a fake one, since we do not
 * actually use any of external RNG and this function is only needed during
 * the execution of TF-M testsuite during exporting the public part of the
 * delegated attestation key.
 */
psa_status_t mbedtls_psa_external_get_random(
			mbedtls_psa_external_random_context_t *context,
			uint8_t *output, size_t output_size,
			size_t *output_length)
{
	for (size_t i = 0U; i < output_size; i++) {
		output[i] = (uint8_t)(read_cntpct_el0() & 0xFFU);
	}

	*output_length = output_size;

	return PSA_SUCCESS;
}
#endif /* PLATFORM_TEST_TFM_TESTSUITE */

static scmi_channel_plat_info_t tc_scmi_plat_info[] = {
	{
		.scmi_mbx_mem = CSS_SCMI_PAYLOAD_BASE,
		.db_reg_addr = PLAT_CSS_MHU_BASE + SENDER_REG_SET(0),
		.db_preserve_mask = 0xfffffffe,
		.db_modify_mask = 0x1,
		.ring_doorbell = &mhuv2_ring_doorbell,
	}
};

void bl31_platform_setup(void)
{
	tc_bl31_common_platform_setup();
}

scmi_channel_plat_info_t *plat_css_get_scmi_info(unsigned int channel_id)
{

	return &tc_scmi_plat_info[channel_id];

}

void bl31_early_platform_setup2(u_register_t arg0, u_register_t arg1,
				u_register_t arg2, u_register_t arg3)
{
	/*
	 * Pass the hw_config to BL33 in R0. You'll notice that
	 * arm_bl31_early_platform_setup does something similar but only behind
	 * ARM_LINUX_KERNEL_AS_BL33 and we want to pass the DTB even to a
	 * bootloader. Lucky for us, it copies the ep_info BL2 gave us to BL33
	 * unconditionally in the generic case so hijack that.
	 * TODO: this goes away with firmware handoff when it will be proper
	 */

	bl_params_node_t *bl_params = ((bl_params_t *)arg0)->head;

	while (bl_params != NULL) {
		if (bl_params->image_id == BL33_IMAGE_ID) {
			bl_params->ep_info->args.arg0 = arg2;
			break;
		}
		bl_params = bl_params->next_params_info;
	}

	arm_bl31_early_platform_setup((void *)arg0, arg1, arg2, (void *)arg3);

	/* Fill the properties struct with the info from the config dtb */
	fconf_populate("FW_CONFIG", arg1);
}

#ifdef PLATFORM_TESTS
static __dead2 void tc_run_platform_tests(void)
{
	int tests_failed;

	printf("\nStarting platform tests...\n");

#ifdef PLATFORM_TEST_NV_COUNTERS
	tests_failed = nv_counter_test();
#elif PLATFORM_TEST_ROTPK
	tests_failed = rotpk_test();
#elif PLATFORM_TEST_TFM_TESTSUITE
	tests_failed = run_platform_tests();
#endif

	printf("Platform tests %s.\n",
	       (tests_failed != 0) ? "failed" : "succeeded");

	/* Suspend booting, no matter the tests outcome. */
	printf("Suspend booting...\n");
	plat_error_handler(-1);
}
#endif

void tc_bl31_common_platform_setup(void)
{
	arm_bl31_platform_setup();

#ifdef PLATFORM_TESTS
	tc_run_platform_tests();
#endif
}

const plat_psci_ops_t *plat_arm_psci_override_pm_ops(plat_psci_ops_t *ops)
{
	return css_scmi_override_pm_ops(ops);
}

void __init bl31_plat_arch_setup(void)
{
	arm_bl31_plat_arch_setup();

	/* HW_CONFIG was also loaded by BL2 */
	const struct dyn_cfg_dtb_info_t *hw_config_info;

	hw_config_info = FCONF_GET_PROPERTY(dyn_cfg, dtb, HW_CONFIG_ID);
	assert(hw_config_info != NULL);

	fconf_populate("HW_CONFIG", hw_config_info->config_addr);
}

#if defined(SPD_spmd) && (SPMC_AT_EL3 == 0)
void tc_bl31_plat_runtime_setup(void)
{
	/* Start secure watchdog timer. */
	plat_arm_secure_wdt_start();

	arm_bl31_plat_runtime_setup();
}

void bl31_plat_runtime_setup(void)
{
	tc_bl31_plat_runtime_setup();
}

/*
 * Platform handler for Group0 secure interrupt.
 */
int plat_spmd_handle_group0_interrupt(uint32_t intid)
{
	/* Trusted Watchdog timer is the only source of Group0 interrupt now. */
	if (intid == SBSA_SECURE_WDOG_INTID) {
		/* Refresh the timer. */
		plat_arm_secure_wdt_refresh();

		return 0;
	}

	return -1;
}
#endif /*defined(SPD_spmd) && (SPMC_AT_EL3 == 0)*/
