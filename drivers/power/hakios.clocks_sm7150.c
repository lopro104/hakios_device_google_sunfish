/*
 * The SM7150's clocks, for the battery: which ones run (clk_dump=1) and switching chosen ones off (clk_off=NAMES), to find
 * what the boot loaders leave on that Android switches off at the end of its boot (Linux's clk_disable_unused).
 *
 * clk_dump=1           at clk_s seconds (default 100; under about 60 s it does not reach the run log): every branch clock
 *                      whose CLK_OFF (bit 31 of its CBCR) is clear, and every PLL that is locked, one line each, then a count.
 * clk_off=a,b,c*       at clk_off_s seconds (default clk_s + 5): clears the enable bit of each named branch (a trailing *
 *                      matches a prefix), then says whether each one stopped. A voted clock (enable bit in GCC 0x52000..)
 *                      only loses our vote: it stays on while another master votes for it. Refused: PLLs, and the clocks of
 *                      UFS, the QUPs, USB, the GPU, the CPU subsystem and the NoCs and DDR.
 *
 * A clock controller's registers are read only when its bus clock in GCC runs: reading one without it can stop the bus.
 * The table comes from msm-4.14's drivers/clk/qcom/*-sdmmagpie.c, by tools/clk-table.py.
 */
#include <kern/clock.h>
#include <kern/startup.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void delay(int usec);

struct clk_row {
	const char *name;
	uint32_t base, halt, enable;
	uint8_t bit, pll;
};

/* Made by tools/clk-table.py from msm-4.14's drivers/clk/qcom/*-sdmmagpie.c: do not edit. */
static const struct clk_row clk_rows[] = {
	{ "gpll0", 0x100000, 0x0, 0x52000, 0, 1 },
	{ "gpll6", 0x100000, 0x13000, 0x52000, 6, 1 },
	{ "gpll7", 0x100000, 0x27000, 0x52000, 7, 1 },
	{ "gcc_aggre_noc_pcie_tbu_clk", 0x100000, 0x2800c, 0x2800c, 0, 0 },
	{ "gcc_aggre_ufs_phy_axi_clk", 0x100000, 0x82024, 0x82024, 0, 0 },
	{ "gcc_aggre_ufs_phy_axi_hw_ctl_clk", 0x100000, 0x82024, 0x82024, 1, 0 },
	{ "gcc_aggre_usb3_prim_axi_clk", 0x100000, 0x8201c, 0x8201c, 0, 0 },
	{ "gcc_apc_vs_clk", 0x100000, 0x7a050, 0x7a050, 0, 0 },
	{ "gcc_boot_rom_ahb_clk", 0x100000, 0x38004, 0x52004, 10, 0 },
	{ "gcc_camera_ahb_clk", 0x100000, 0xb008, 0xb008, 0, 0 },
	{ "gcc_camera_hf_axi_clk", 0x100000, 0xb020, 0xb020, 0, 0 },
	{ "gcc_camera_sf_axi_clk", 0x100000, 0xb06c, 0xb06c, 0, 0 },
	{ "gcc_camera_xo_clk", 0x100000, 0xb02c, 0xb02c, 0, 0 },
	{ "gcc_ce1_ahb_clk", 0x100000, 0x4100c, 0x52004, 3, 0 },
	{ "gcc_ce1_axi_clk", 0x100000, 0x41008, 0x52004, 4, 0 },
	{ "gcc_ce1_clk", 0x100000, 0x41004, 0x52004, 5, 0 },
	{ "gcc_cfg_noc_usb3_prim_axi_clk", 0x100000, 0x502c, 0x502c, 0, 0 },
	{ "gcc_cpuss_ahb_clk", 0x100000, 0x48000, 0x52004, 21, 0 },
	{ "gcc_cpuss_rbcpr_clk", 0x100000, 0x48008, 0x48008, 0, 0 },
	{ "gcc_ddrss_gpu_axi_clk", 0x100000, 0x4452c, 0x4452c, 0, 0 },
	{ "gcc_disp_ahb_clk", 0x100000, 0xb00c, 0xb00c, 0, 0 },
	{ "gcc_disp_hf_axi_clk", 0x100000, 0xb024, 0xb024, 0, 0 },
	{ "gcc_disp_sf_axi_clk", 0x100000, 0xb070, 0xb070, 0, 0 },
	{ "gcc_disp_xo_clk", 0x100000, 0xb030, 0xb030, 0, 0 },
	{ "gcc_gp1_clk", 0x100000, 0x64000, 0x64000, 0, 0 },
	{ "gcc_gp2_clk", 0x100000, 0x65000, 0x65000, 0, 0 },
	{ "gcc_gp3_clk", 0x100000, 0x66000, 0x66000, 0, 0 },
	{ "gcc_gpu_cfg_ahb_clk", 0x100000, 0x71004, 0x71004, 0, 0 },
	{ "gcc_gpu_memnoc_gfx_clk", 0x100000, 0x7100c, 0x7100c, 0, 0 },
	{ "gcc_gpu_snoc_dvm_gfx_clk", 0x100000, 0x71018, 0x71018, 0, 0 },
	{ "gcc_gpu_vs_clk", 0x100000, 0x7a04c, 0x7a04c, 0, 0 },
	{ "gcc_npu_axi_clk", 0x100000, 0x4d008, 0x4d008, 0, 0 },
	{ "gcc_npu_cfg_ahb_clk", 0x100000, 0x4d004, 0x4d004, 0, 0 },
	{ "gcc_pcie_0_aux_clk", 0x100000, 0x6b01c, 0x5200c, 3, 0 },
	{ "gcc_pcie_0_cfg_ahb_clk", 0x100000, 0x6b018, 0x5200c, 2, 0 },
	{ "gcc_pcie_0_clkref_clk", 0x100000, 0x8c008, 0x8c008, 0, 0 },
	{ "gcc_pcie_0_mstr_axi_clk", 0x100000, 0x6b014, 0x5200c, 1, 0 },
	{ "gcc_pcie_0_pipe_clk", 0x100000, 0x6b020, 0x5200c, 4, 0 },
	{ "gcc_pcie_0_slv_axi_clk", 0x100000, 0x6b010, 0x5200c, 0, 0 },
	{ "gcc_pcie_0_slv_q2a_axi_clk", 0x100000, 0x6b00c, 0x5200c, 5, 0 },
	{ "gcc_pcie_phy_aux_clk", 0x100000, 0x6f004, 0x6f004, 0, 0 },
	{ "gcc_pcie_phy_refgen_clk", 0x100000, 0x6f02c, 0x6f02c, 0, 0 },
	{ "gcc_pdm2_clk", 0x100000, 0x3300c, 0x3300c, 0, 0 },
	{ "gcc_pdm_ahb_clk", 0x100000, 0x33004, 0x33004, 0, 0 },
	{ "gcc_pdm_xo4_clk", 0x100000, 0x33008, 0x33008, 0, 0 },
	{ "gcc_prng_ahb_clk", 0x100000, 0x34004, 0x52004, 13, 0 },
	{ "gcc_qupv3_wrap0_core_2x_clk", 0x100000, 0x17014, 0x5200c, 9, 0 },
	{ "gcc_qupv3_wrap0_core_clk", 0x100000, 0x1700c, 0x5200c, 8, 0 },
	{ "gcc_qupv3_wrap0_s0_clk", 0x100000, 0x17030, 0x5200c, 10, 0 },
	{ "gcc_qupv3_wrap0_s1_clk", 0x100000, 0x17160, 0x5200c, 11, 0 },
	{ "gcc_qupv3_wrap0_s2_clk", 0x100000, 0x17290, 0x5200c, 12, 0 },
	{ "gcc_qupv3_wrap0_s3_clk", 0x100000, 0x173c0, 0x5200c, 13, 0 },
	{ "gcc_qupv3_wrap0_s4_clk", 0x100000, 0x174f0, 0x5200c, 14, 0 },
	{ "gcc_qupv3_wrap0_s5_clk", 0x100000, 0x17620, 0x5200c, 15, 0 },
	{ "gcc_qupv3_wrap0_s6_clk", 0x100000, 0x17750, 0x5200c, 16, 0 },
	{ "gcc_qupv3_wrap0_s7_clk", 0x100000, 0x17880, 0x5200c, 17, 0 },
	{ "gcc_qupv3_wrap1_core_2x_clk", 0x100000, 0x18004, 0x5200c, 18, 0 },
	{ "gcc_qupv3_wrap1_core_clk", 0x100000, 0x18008, 0x5200c, 19, 0 },
	{ "gcc_qupv3_wrap1_s0_clk", 0x100000, 0x18014, 0x5200c, 22, 0 },
	{ "gcc_qupv3_wrap1_s1_clk", 0x100000, 0x18144, 0x5200c, 23, 0 },
	{ "gcc_qupv3_wrap1_s2_clk", 0x100000, 0x18274, 0x5200c, 24, 0 },
	{ "gcc_qupv3_wrap1_s3_clk", 0x100000, 0x183a4, 0x5200c, 25, 0 },
	{ "gcc_qupv3_wrap1_s4_clk", 0x100000, 0x184d4, 0x5200c, 26, 0 },
	{ "gcc_qupv3_wrap1_s5_clk", 0x100000, 0x18604, 0x5200c, 27, 0 },
	{ "gcc_qupv3_wrap1_s6_clk", 0x100000, 0x18734, 0x5200c, 28, 0 },
	{ "gcc_qupv3_wrap1_s7_clk", 0x100000, 0x18864, 0x5200c, 29, 0 },
	{ "gcc_qupv3_wrap_0_m_ahb_clk", 0x100000, 0x17004, 0x5200c, 6, 0 },
	{ "gcc_qupv3_wrap_0_s_ahb_clk", 0x100000, 0x17008, 0x5200c, 7, 0 },
	{ "gcc_qupv3_wrap_1_m_ahb_clk", 0x100000, 0x1800c, 0x5200c, 20, 0 },
	{ "gcc_qupv3_wrap_1_s_ahb_clk", 0x100000, 0x18010, 0x5200c, 21, 0 },
	{ "gcc_sdcc1_ahb_clk", 0x100000, 0x12008, 0x12008, 0, 0 },
	{ "gcc_sdcc1_apps_clk", 0x100000, 0x1200c, 0x1200c, 0, 0 },
	{ "gcc_sdcc1_ice_core_clk", 0x100000, 0x12040, 0x12040, 0, 0 },
	{ "gcc_sdcc2_ahb_clk", 0x100000, 0x14008, 0x14008, 0, 0 },
	{ "gcc_sdcc2_apps_clk", 0x100000, 0x14004, 0x14004, 0, 0 },
	{ "gcc_sdcc4_ahb_clk", 0x100000, 0x16008, 0x16008, 0, 0 },
	{ "gcc_sdcc4_apps_clk", 0x100000, 0x16004, 0x16004, 0, 0 },
	{ "gcc_sys_noc_cpuss_ahb_clk", 0x100000, 0x4144, 0x52004, 0, 0 },
	{ "gcc_tsif_ahb_clk", 0x100000, 0x36004, 0x36004, 0, 0 },
	{ "gcc_tsif_inactivity_timers_clk", 0x100000, 0x3600c, 0x3600c, 0, 0 },
	{ "gcc_tsif_ref_clk", 0x100000, 0x36008, 0x36008, 0, 0 },
	{ "gcc_ufs_mem_clkref_clk", 0x100000, 0x8c000, 0x8c000, 0, 0 },
	{ "gcc_ufs_phy_ahb_clk", 0x100000, 0x77014, 0x77014, 0, 0 },
	{ "gcc_ufs_phy_axi_clk", 0x100000, 0x77038, 0x77038, 0, 0 },
	{ "gcc_ufs_phy_axi_hw_ctl_clk", 0x100000, 0x77038, 0x77038, 1, 0 },
	{ "gcc_ufs_phy_ice_core_clk", 0x100000, 0x77090, 0x77090, 0, 0 },
	{ "gcc_ufs_phy_ice_core_hw_ctl_clk", 0x100000, 0x77090, 0x77090, 1, 0 },
	{ "gcc_ufs_phy_phy_aux_clk", 0x100000, 0x77094, 0x77094, 0, 0 },
	{ "gcc_ufs_phy_phy_aux_hw_ctl_clk", 0x100000, 0x77094, 0x77094, 1, 0 },
	{ "gcc_ufs_phy_rx_symbol_0_clk", 0x100000, 0x7701c, 0x7701c, 0, 0 },
	{ "gcc_ufs_phy_tx_symbol_0_clk", 0x100000, 0x77018, 0x77018, 0, 0 },
	{ "gcc_ufs_phy_unipro_core_clk", 0x100000, 0x7708c, 0x7708c, 0, 0 },
	{ "gcc_ufs_phy_unipro_core_hw_ctl_clk", 0x100000, 0x7708c, 0x7708c, 1, 0 },
	{ "gcc_usb30_prim_master_clk", 0x100000, 0xf010, 0xf010, 0, 0 },
	{ "gcc_usb30_prim_mock_utmi_clk", 0x100000, 0xf018, 0xf018, 0, 0 },
	{ "gcc_usb30_prim_sleep_clk", 0x100000, 0xf014, 0xf014, 0, 0 },
	{ "gcc_usb3_prim_clkref_clk", 0x100000, 0x8c010, 0x8c010, 0, 0 },
	{ "gcc_usb3_prim_phy_aux_clk", 0x100000, 0xf050, 0xf050, 0, 0 },
	{ "gcc_usb3_prim_phy_com_aux_clk", 0x100000, 0xf054, 0xf054, 0, 0 },
	{ "gcc_usb_phy_cfg_ahb2phy_clk", 0x100000, 0x6a004, 0x6a004, 0, 0 },
	{ "gcc_vdda_vs_clk", 0x100000, 0x7a00c, 0x7a00c, 0, 0 },
	{ "gcc_vddcx_vs_clk", 0x100000, 0x7a004, 0x7a004, 0, 0 },
	{ "gcc_vddmx_vs_clk", 0x100000, 0x7a008, 0x7a008, 0, 0 },
	{ "gcc_video_ahb_clk", 0x100000, 0xb004, 0xb004, 0, 0 },
	{ "gcc_video_axi_clk", 0x100000, 0xb01c, 0xb01c, 0, 0 },
	{ "gcc_video_xo_clk", 0x100000, 0xb028, 0xb028, 0, 0 },
	{ "gcc_vs_ctrl_ahb_clk", 0x100000, 0x7a014, 0x7a014, 0, 0 },
	{ "gcc_vs_ctrl_clk", 0x100000, 0x7a010, 0x7a010, 0, 0 },
	{ "gcc_cpuss_gnoc_clk", 0x100000, 0x48004, 0x52004, 22, 0 },
	{ "disp_cc_pll0", 0xaf00000, 0x0, 0x0, 0, 1 },
	{ "disp_cc_mdss_ahb_clk", 0xaf00000, 0x2080, 0x2080, 0, 0 },
	{ "disp_cc_mdss_byte0_clk", 0xaf00000, 0x2028, 0x2028, 0, 0 },
	{ "disp_cc_mdss_byte0_intf_clk", 0xaf00000, 0x202c, 0x202c, 0, 0 },
	{ "disp_cc_mdss_byte1_clk", 0xaf00000, 0x2030, 0x2030, 0, 0 },
	{ "disp_cc_mdss_byte1_intf_clk", 0xaf00000, 0x2034, 0x2034, 0, 0 },
	{ "disp_cc_mdss_dp_aux_clk", 0xaf00000, 0x2054, 0x2054, 0, 0 },
	{ "disp_cc_mdss_dp_crypto_clk", 0xaf00000, 0x2048, 0x2048, 0, 0 },
	{ "disp_cc_mdss_dp_link_clk", 0xaf00000, 0x2040, 0x2040, 0, 0 },
	{ "disp_cc_mdss_dp_link_intf_clk", 0xaf00000, 0x2044, 0x2044, 0, 0 },
	{ "disp_cc_mdss_dp_pixel1_clk", 0xaf00000, 0x2050, 0x2050, 0, 0 },
	{ "disp_cc_mdss_dp_pixel_clk", 0xaf00000, 0x204c, 0x204c, 0, 0 },
	{ "disp_cc_mdss_esc0_clk", 0xaf00000, 0x2038, 0x2038, 0, 0 },
	{ "disp_cc_mdss_esc1_clk", 0xaf00000, 0x203c, 0x203c, 0, 0 },
	{ "disp_cc_mdss_mdp_clk", 0xaf00000, 0x200c, 0x200c, 0, 0 },
	{ "disp_cc_mdss_mdp_lut_clk", 0xaf00000, 0x201c, 0x201c, 0, 0 },
	{ "disp_cc_mdss_non_gdsc_ahb_clk", 0xaf00000, 0x4004, 0x4004, 0, 0 },
	{ "disp_cc_mdss_pclk0_clk", 0xaf00000, 0x2004, 0x2004, 0, 0 },
	{ "disp_cc_mdss_pclk1_clk", 0xaf00000, 0x2008, 0x2008, 0, 0 },
	{ "disp_cc_mdss_rot_clk", 0xaf00000, 0x2014, 0x2014, 0, 0 },
	{ "disp_cc_mdss_rscc_ahb_clk", 0xaf00000, 0x400c, 0x400c, 0, 0 },
	{ "disp_cc_mdss_rscc_vsync_clk", 0xaf00000, 0x4008, 0x4008, 0, 0 },
	{ "disp_cc_mdss_vsync_clk", 0xaf00000, 0x2024, 0x2024, 0, 0 },
	{ "disp_cc_sleep_clk", 0xaf00000, 0x6078, 0x6078, 0, 0 },
	{ "disp_cc_xo_clk", 0xaf00000, 0x605c, 0x605c, 0, 0 },
	{ "gpu_cc_pll0", 0x5090000, 0x0, 0x0, 0, 1 },
	{ "gpu_cc_pll1", 0x5090000, 0x100, 0x100, 0, 1 },
	{ "gpu_cc_acd_ahb_clk", 0x5090000, 0x1168, 0x1168, 0, 0 },
	{ "gpu_cc_acd_cxo_clk", 0x5090000, 0x1164, 0x1164, 0, 0 },
	{ "gpu_cc_ahb_clk", 0x5090000, 0x1078, 0x1078, 0, 0 },
	{ "gpu_cc_crc_ahb_clk", 0x5090000, 0x107c, 0x107c, 0, 0 },
	{ "gpu_cc_cx_apb_clk", 0x5090000, 0x1088, 0x1088, 0, 0 },
	{ "gpu_cc_cx_gfx3d_clk", 0x5090000, 0x10a4, 0x10a4, 0, 0 },
	{ "gpu_cc_cx_gfx3d_slv_clk", 0x5090000, 0x10a8, 0x10a8, 0, 0 },
	{ "gpu_cc_cx_gmu_clk", 0x5090000, 0x1098, 0x1098, 0, 0 },
	{ "gpu_cc_cx_snoc_dvm_clk", 0x5090000, 0x108c, 0x108c, 0, 0 },
	{ "gpu_cc_cxo_aon_clk", 0x5090000, 0x1004, 0x1004, 0, 0 },
	{ "gpu_cc_cxo_clk", 0x5090000, 0x109c, 0x109c, 0, 0 },
	{ "gpu_cc_gx_cxo_clk", 0x5090000, 0x1060, 0x1060, 0, 0 },
	{ "gpu_cc_gx_gfx3d_clk", 0x5090000, 0x1054, 0x1054, 0, 0 },
	{ "gpu_cc_gx_gmu_clk", 0x5090000, 0x1064, 0x1064, 0, 0 },
	{ "gpu_cc_gx_vsense_clk", 0x5090000, 0x1058, 0x1058, 0, 0 },
	{ "video_pll0", 0xab00000, 0x42c, 0x42c, 0, 1 },
	{ "video_cc_iris_ahb_clk", 0xab00000, 0x8f4, 0x8f4, 0, 0 },
	{ "video_cc_mvs0_axi_clk", 0xab00000, 0x9ec, 0x9ec, 0, 0 },
	{ "video_cc_mvs0_core_clk", 0xab00000, 0x890, 0x890, 0, 0 },
	{ "video_cc_mvs1_axi_clk", 0xab00000, 0xa0c, 0xa0c, 0, 0 },
	{ "video_cc_mvs1_core_clk", 0xab00000, 0x8d0, 0x8d0, 0, 0 },
	{ "video_cc_mvsc_core_clk", 0xab00000, 0x850, 0x850, 0, 0 },
	{ "video_cc_mvsc_ctl_axi_clk", 0xab00000, 0x9cc, 0x9cc, 0, 0 },
	{ "video_cc_venus_ahb_clk", 0xab00000, 0xa6c, 0xa6c, 0, 0 },
	{ "video_cc_xo_clk", 0xab00000, 0x984, 0x984, 0, 0 },
	{ "cam_cc_pll0", 0xad00000, 0x0, 0x0, 0, 1 },
	{ "cam_cc_pll1", 0xad00000, 0x1000, 0x1000, 0, 1 },
	{ "cam_cc_pll2", 0xad00000, 0x2000, 0x2000, 0, 1 },
	{ "cam_cc_pll3", 0xad00000, 0x3000, 0x3000, 0, 1 },
	{ "cam_cc_pll4", 0xad00000, 0x4000, 0x4000, 0, 1 },
	{ "cam_cc_bps_ahb_clk", 0xad00000, 0x7070, 0x7070, 0, 0 },
	{ "cam_cc_bps_areg_clk", 0xad00000, 0x7054, 0x7054, 0, 0 },
	{ "cam_cc_bps_axi_clk", 0xad00000, 0x7038, 0x7038, 0, 0 },
	{ "cam_cc_bps_clk", 0xad00000, 0x7028, 0x7028, 0, 0 },
	{ "cam_cc_camnoc_axi_clk", 0xad00000, 0xc148, 0xc148, 0, 0 },
	{ "cam_cc_camnoc_dcd_xo_clk", 0xad00000, 0xc150, 0xc150, 0, 0 },
	{ "cam_cc_cci_0_clk", 0xad00000, 0xc0dc, 0xc0dc, 0, 0 },
	{ "cam_cc_cci_1_clk", 0xad00000, 0xc0f8, 0xc0f8, 0, 0 },
	{ "cam_cc_core_ahb_clk", 0xad00000, 0xc184, 0xc184, 0, 0 },
	{ "cam_cc_cpas_ahb_clk", 0xad00000, 0xc124, 0xc124, 0, 0 },
	{ "cam_cc_csi0phytimer_clk", 0xad00000, 0x601c, 0x601c, 0, 0 },
	{ "cam_cc_csi1phytimer_clk", 0xad00000, 0x6040, 0x6040, 0, 0 },
	{ "cam_cc_csi2phytimer_clk", 0xad00000, 0x6064, 0x6064, 0, 0 },
	{ "cam_cc_csi3phytimer_clk", 0xad00000, 0x6088, 0x6088, 0, 0 },
	{ "cam_cc_csiphy0_clk", 0xad00000, 0x6020, 0x6020, 0, 0 },
	{ "cam_cc_csiphy1_clk", 0xad00000, 0x6044, 0x6044, 0, 0 },
	{ "cam_cc_csiphy2_clk", 0xad00000, 0x6068, 0x6068, 0, 0 },
	{ "cam_cc_csiphy3_clk", 0xad00000, 0x608c, 0x608c, 0, 0 },
	{ "cam_cc_fd_core_clk", 0xad00000, 0xc0b4, 0xc0b4, 0, 0 },
	{ "cam_cc_fd_core_uar_clk", 0xad00000, 0xc0bc, 0xc0bc, 0, 0 },
	{ "cam_cc_gdsc_clk", 0xad00000, 0xc1a0, 0xc1a0, 0, 0 },
	{ "cam_cc_icp_ahb_clk", 0xad00000, 0xc094, 0xc094, 0, 0 },
	{ "cam_cc_icp_clk", 0xad00000, 0xc08c, 0xc08c, 0, 0 },
	{ "cam_cc_ife_0_axi_clk", 0xad00000, 0xa080, 0xa080, 0, 0 },
	{ "cam_cc_ife_0_clk", 0xad00000, 0xa028, 0xa028, 0, 0 },
	{ "cam_cc_ife_0_cphy_rx_clk", 0xad00000, 0xa07c, 0xa07c, 0, 0 },
	{ "cam_cc_ife_0_csid_clk", 0xad00000, 0xa054, 0xa054, 0, 0 },
	{ "cam_cc_ife_0_dsp_clk", 0xad00000, 0xa038, 0xa038, 0, 0 },
	{ "cam_cc_ife_1_axi_clk", 0xad00000, 0xb058, 0xb058, 0, 0 },
	{ "cam_cc_ife_1_clk", 0xad00000, 0xb028, 0xb028, 0, 0 },
	{ "cam_cc_ife_1_cphy_rx_clk", 0xad00000, 0xb054, 0xb054, 0, 0 },
	{ "cam_cc_ife_1_csid_clk", 0xad00000, 0xb04c, 0xb04c, 0, 0 },
	{ "cam_cc_ife_1_dsp_clk", 0xad00000, 0xb030, 0xb030, 0, 0 },
	{ "cam_cc_ife_lite_clk", 0xad00000, 0xc01c, 0xc01c, 0, 0 },
	{ "cam_cc_ife_lite_cphy_rx_clk", 0xad00000, 0xc040, 0xc040, 0, 0 },
	{ "cam_cc_ife_lite_csid_clk", 0xad00000, 0xc038, 0xc038, 0, 0 },
	{ "cam_cc_ipe_0_ahb_clk", 0xad00000, 0x8040, 0x8040, 0, 0 },
	{ "cam_cc_ipe_0_areg_clk", 0xad00000, 0x803c, 0x803c, 0, 0 },
	{ "cam_cc_ipe_0_axi_clk", 0xad00000, 0x8038, 0x8038, 0, 0 },
	{ "cam_cc_ipe_0_clk", 0xad00000, 0x8028, 0x8028, 0, 0 },
	{ "cam_cc_ipe_1_ahb_clk", 0xad00000, 0x9028, 0x9028, 0, 0 },
	{ "cam_cc_ipe_1_areg_clk", 0xad00000, 0x9024, 0x9024, 0, 0 },
	{ "cam_cc_ipe_1_axi_clk", 0xad00000, 0x9020, 0x9020, 0, 0 },
	{ "cam_cc_ipe_1_clk", 0xad00000, 0x9010, 0x9010, 0, 0 },
	{ "cam_cc_jpeg_clk", 0xad00000, 0xc060, 0xc060, 0, 0 },
	{ "cam_cc_lrme_clk", 0xad00000, 0xc118, 0xc118, 0, 0 },
	{ "cam_cc_mclk0_clk", 0xad00000, 0x501c, 0x501c, 0, 0 },
	{ "cam_cc_mclk1_clk", 0xad00000, 0x503c, 0x503c, 0, 0 },
	{ "cam_cc_mclk2_clk", 0xad00000, 0x505c, 0x505c, 0, 0 },
	{ "cam_cc_mclk3_clk", 0xad00000, 0x507c, 0x507c, 0, 0 },
	{ "cam_cc_sleep_clk", 0xad00000, 0xc1bc, 0xc1bc, 0, 0 },
	{ "npu_cc_pll0", 0x9910000, 0x0, 0x0, 0, 1 },
	{ "npu_cc_pll1", 0x9910000, 0x400, 0x400, 0, 1 },
	{ "npu_cc_armwic_core_clk", 0x9910000, 0x1058, 0x1058, 0, 0 },
	{ "npu_cc_bto_core_clk", 0x9910000, 0x1090, 0x1090, 0, 0 },
	{ "npu_cc_bwmon_clk", 0x9910000, 0x1088, 0x1088, 0, 0 },
	{ "npu_cc_cal_dp_cdc_clk", 0x9910000, 0x1068, 0x1068, 0, 0 },
	{ "npu_cc_cal_dp_clk", 0x9910000, 0x101c, 0x101c, 0, 0 },
	{ "npu_cc_comp_noc_axi_clk", 0x9910000, 0x106c, 0x106c, 0, 0 },
	{ "npu_cc_conf_noc_ahb_clk", 0x9910000, 0x1074, 0x1074, 0, 0 },
	{ "npu_cc_npu_core_apb_clk", 0x9910000, 0x1080, 0x1080, 0, 0 },
	{ "npu_cc_npu_core_atb_clk", 0x9910000, 0x1078, 0x1078, 0, 0 },
	{ "npu_cc_npu_core_clk", 0x9910000, 0x1048, 0x1048, 0, 0 },
	{ "npu_cc_npu_core_cti_clk", 0x9910000, 0x107c, 0x107c, 0, 0 },
	{ "npu_cc_npu_cpc_clk", 0x9910000, 0x1050, 0x1050, 0, 0 },
	{ "npu_cc_npu_cpc_timer_clk", 0x9910000, 0x105c, 0x105c, 0, 0 },
	{ "npu_cc_perf_cnt_clk", 0x9910000, 0x108c, 0x108c, 0, 0 },
	{ "npu_cc_qtimer_core_clk", 0x9910000, 0x1060, 0x1060, 0, 0 },
	{ "npu_cc_sleep_clk", 0x9910000, 0x1064, 0x1064, 0, 0 },
	{ "npu_cc_xo_clk", 0x9910000, 0x3020, 0x3020, 0, 0 },
};

static const struct { uint32_t base, size, ahb; const char *name; } controllers[] = {
	{ 0x100000, 0x1f0000, 0, "gcc" }, { 0xaf00000, 0x20000, 0xb00c, "dispcc" }, { 0x5090000, 0x9000, 0x71004, "gpucc" },
	{ 0xab00000, 0x10000, 0xb004, "videocc" }, { 0xad00000, 0x10000, 0xb008, "camcc" }, { 0x9910000, 0x10000, 0x4d004, "npucc" },
};
#define CONTROLLERS (sizeof(controllers) / sizeof(controllers[0]))
#define ROWS (sizeof(clk_rows) / sizeof(clk_rows[0]))

static vm_offset_t pages_pa[192], pages_va[192];
static unsigned pages_used;
static char off_list[512];
static uint32_t clk_s = 100, clk_off_s;
static bool dump_on;
static thread_call_t clk_call;

/* One 16 KB page at a time, each mapped once: a 2 MB mapping of GCC fails (kmem_alloc), and a mapping per read leaks. */
static volatile uint32_t *
reg(unsigned c, uint32_t off)
{
	vm_offset_t pa = controllers[c].base + off, page = pa & ~(vm_offset_t)0x3fff;
	static volatile uint32_t dummy = 0xffffffffu;

	for (unsigned i = 0; i < pages_used; i++) {
		if (pages_pa[i] == page) {
			return (volatile uint32_t *)(pages_va[i] + (pa & 0x3fff));
		}
	}
	if (pages_used == sizeof(pages_pa) / sizeof(pages_pa[0])) {
		return &dummy;
	}
	pages_va[pages_used] = ml_io_map(page, 0x4000);
	if (pages_va[pages_used] == 0) {
		return &dummy;
	}
	pages_pa[pages_used] = page;
	return (volatile uint32_t *)(pages_va[pages_used++] + (pa & 0x3fff));
}

static unsigned
controller_of(uint32_t base)
{
	for (unsigned c = 0; c < CONTROLLERS; c++) {
		if (controllers[c].base == base) {
			return c;
		}
	}
	return 0;
}

/* Its bus clock runs (and it is mapped): its registers can be read. */
static bool
ready(unsigned c)
{
	/* enabled (bit 0) or hardware-gated (bit 1, HWCG: CLK_OFF reads set while idle, and an access ungates it) */
	return controllers[c].ahb == 0 || (*reg(0, controllers[c].ahb) & 0x3u) != 0;
}

static bool
running(const struct clk_row *r, uint32_t *value)
{
	unsigned c = controller_of(r->base);

	*value = *reg(c, r->halt);
	return r->pll ? (*value & 0x80000000u) != 0 : !(*value & 0x80000000u);
}

static void
dump(const char *when)
{
	unsigned on = 0;
	bool said[CONTROLLERS] = { false };

	for (unsigned i = 0; i < ROWS; i++) {
		const struct clk_row *r = &clk_rows[i];
		unsigned c = controller_of(r->base);
		uint32_t v;

		if (i > 0 && clk_rows[i - 1].base == r->base && clk_rows[i - 1].halt == r->halt && !r->pll) {
			continue;       /* the same CBCR as the row before (a _hw_ctl twin) */
		}
		if (!ready(c)) {
			if (!said[c]) {
				printf("clk_dump: %s: %s's bus clock is off: not read\n", when, controllers[c].name);
				said[c] = true;
			}
			continue;
		}
		if (running(r, &v)) {
			on++;
			printf("clk_dump: %s: %-44s %#010x%s\n", when, r->name, v,
			    r->enable != r->halt ? ((*reg(c, r->enable) >> r->bit) & 1 ? " voted" : " (not our vote)") : "");
		}
	}
	printf("clk_dump: %s: %u running\n", when, on);
}

static bool
refused(const char *name)
{
	static const char *const keep[] = { "ufs", "qup", "usb", "gpu", "cpuss", "ddrss", "noc", "memnoc" };

	for (unsigned k = 0; k < sizeof(keep) / sizeof(keep[0]); k++) {
		if (strnstr(name, keep[k], strlen(name)) != NULL) {
			return true;
		}
	}
	return false;
}

static bool
named(const char *name, const char *list)
{
	const char *p = list;

	while (*p != '\0') {
		const char *end = p;
		size_t n;

		while (*end != '\0' && *end != ',') {
			end++;
		}
		n = (size_t)(end - p);
		if (n > 0 && p[n - 1] == '*' ? strncmp(name, p, n - 1) == 0 : (strlen(name) == n && strncmp(name, p, n) == 0)) {
			return true;
		}
		p = *end == ',' ? end + 1 : end;
	}
	return false;
}

static void
switch_off(void)
{
	unsigned done = 0;

	for (unsigned i = 0; i < ROWS; i++) {
		const struct clk_row *r = &clk_rows[i];
		unsigned c = controller_of(r->base);
		uint32_t before, after;

		if (!named(r->name, off_list)) {
			continue;
		}
		if (r->pll || refused(r->name)) {
			printf("clk_off: %s: refused\n", r->name);
			continue;
		}
		if (!ready(c)) {
			printf("clk_off: %s: %s's bus clock is off: not touched\n", r->name, controllers[c].name);
			continue;
		}
		if (!running(r, &before)) {
			continue;
		}
		*reg(c, r->enable) &= ~(1u << r->bit);
		__asm__ volatile ("dsb sy" ::: "memory");
		delay(50);
		printf("clk_off: %s: %#x -> %s\n", r->name, before, running(r, &after) ? "STILL RUNNING" : "off");
		done++;
	}
	printf("clk_off: %u switched\n", done);
}

static void
clk_run(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	if (dump_on) {
		dump("before");
	}
	if (off_list[0] != '\0') {
		uint64_t now = mach_absolute_time(), at;

		nanoseconds_to_absolutetime((uint64_t)(clk_off_s - clk_s) * NSEC_PER_SEC, &at);
		clock_delay_until(now + at);
		switch_off();
		if (dump_on) {
			delay(2000000);
			dump("after");
		}
	}
}

__startup_func
static void
clocks_init(void)
{
	uint32_t on = 0;
	uint64_t deadline;

	(void)PE_parse_boot_argn("clk_dump", &on, sizeof(on));
	dump_on = on != 0;
	(void)PE_parse_boot_arg_str("clk_off", off_list, sizeof(off_list));
	if (!dump_on && off_list[0] == '\0') {
		return;
	}
	(void)PE_parse_boot_argn("clk_s", &clk_s, sizeof(clk_s));
	clk_off_s = clk_s + 5;
	(void)PE_parse_boot_argn("clk_off_s", &clk_off_s, sizeof(clk_off_s));
	if (clk_off_s < clk_s) {
		clk_off_s = clk_s;
	}
	clk_call = thread_call_allocate(clk_run, NULL);
	clock_interval_to_deadline(clk_s, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(clk_call, deadline);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, clocks_init);
