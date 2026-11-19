/* PCIe2 only. Register operations follow the DWC/QMP hardware contract;
 * neither Wi-Fi's fixed PCIe0 addresses nor Linux driver bodies are reused. */
#include "umi_modem_phy.h"
#ifndef MD_REFGEN_LOCK
#define MD_REFGEN_LOCK() do {} while (0)
#define MD_REFGEN_UNLOCK() do {} while (0)
#endif
static int md_rcg(uint32_t cmd,uint32_t cfg)
{
	MD_TRY(md_update(MD_GCC+cmd+4,0x371f,cfg));
	MD_TRY(md_update(MD_GCC+cmd,0,1));
	return md_wait(MD_GCC+cmd,1,0,10000);
}
static int md_branch(uint32_t off)
{
	MD_TRY(md_update(MD_GCC+off,0,1));
	return md_wait(MD_GCC+off,MD_BIT(31),0,10000);
}
static int md_refgen(void)
{
	/* PCIe0/1/2 share GCC's PHY refgen RCG. All SM8250 PCIe consumers use
	 * 100 MHz. Re-apply the common setting while holding the shared lock so
	 * either controller may be brought up first; an inherited 19.2 MHz vote
	 * is a stale boot vote, not a modem/Wi-Fi incompatibility. */
	MD_REFGEN_LOCK();
	int error=md_rcg(0x6f014,0x10b);
	MD_REFGEN_UNLOCK();
	return error;
}
static int md_atu(unsigned n,unsigned type,uint32_t base,uint32_t size,uint32_t target)
{
	uint32_t a=MD_ATU+n*0x200;
	md_write(a+4,0); md_write(a,type);
	md_address(a+8,base); md_write(a+16,base+size-1);
	md_address(a+20,target); md_write(a+4,MD_BIT(31));
	return md_wait(a+4,MD_BIT(31),MD_BIT(31),1000);
}
static void md_link_snapshot(const char *step,int error)
{
	(void)step; (void)error;
	MD_LOG("link step=%s err=%d PHY_CTRL=%#x SYS_CTRL=%#x LTSSM=%#x ELBI_CTRL=%#x ELBI_STTS=%#x PCS_STATUS=%#x PHY_START=%#x PIPE_RCG=%#x PIPE_CBCR=%#x PIPE_VOTE=%#x DBI_LINK=%#x DBI_DBG=%#x\n",
	    step,error,md_read(MD_PARF+0x40),md_read(MD_PARF),
	    md_read(MD_PARF+0x1b0),md_read(MD_DBI+0xf24),md_read(MD_DBI+0xf28),
	    md_read(MD_PHY+0xa14),md_read(MD_PHY+0xa44),md_read(MD_GCC+0x603c),
	    md_read(MD_GCC+0x602c),md_read(MD_GCC+0x52010),
	    md_read(MD_DBI+0x80),md_read(MD_DBI+0x72c));
}
#define MD_LINK_TRY(name,expr) do { int md_link_err=(expr); md_link_snapshot(name,md_link_err); if(md_link_err)return md_link_err; } while (0)
static int md_pcie(void)
{
	md_stage="clock-ownership";
	md_stage="power";
	MD_TRY(md_output(85,false,1)); md_gpio_owned=true;
	MD_LOG("power GPIO85 PERST=0\n");
	MD_TRY(md_output(57,false,7u<<6));
	MD_LOG("power GPIO57 AP_ERRFATAL=0\n");
	MD_TRY(md_output(56,false,7u<<6));
	MD_LOG("power GPIO56 AP_STATUS=0\n");
	MD_TRY(md_update(MD_GPIO+1*0x1000,0x3ff,3u<<6));
	MD_LOG("power GPIO1 MDM_ERRFATAL=input\n");
	MD_TRY(md_update(MD_GPIO+3*0x1000,0x3ff,3u<<6));
	MD_LOG("power GPIO3 MDM_STATUS=input\n");
	MD_TRY(md_update(MD_GPIO+86*0x1000,0x3ff,7));
	MD_LOG("power GPIO86 CLKREQ=input\n");
	MD_TRY(md_update(MD_GPIO+87*0x1000,0x3ff,3));
	MD_LOG("power GPIO87 WAKE=input\n");
	MD_LOG("power GPIOs ready PERST=0 AP_STATUS=0 AP_ERRFATAL=0\n");
	MD_TRY(umi_rpmh_modem_power());
	/* The actual UMI qcom,ext-sdx55m node has no
	 * qcom,ap2mdm-soft-reset-gpio or modem-power regulator. Its only
	 * AP-side power handshake is AP_STATUS; never write the AP PM8150 PON. */
	MD_LOG("power no PMIC PON/soft-reset GPIO in UMI DT; AP_STATUS handshake only\n");
	MD_TRY(md_output(56,true,7u<<6));
	MD_LOG("power AP_STATUS=1; MDM_STATUS is diagnostic only\n");
	MD_LOG("power MDM_STATUS=%u MDM_ERRFATAL=%u\n",
	    md_read(MD_GPIO+3*0x1000+4)&1,md_read(MD_GPIO+0x1000+4)&1);
	md_stage="clocks";
	MD_TRY(md_update(MD_GCC+0x6004,7,0));
	MD_TRY(md_wait(MD_GCC+0x6004,MD_BIT(31),MD_BIT(31),10000));
	MD_TRY(md_rcg(0x6038,1)); /* PCIe2 AUX: XO / 1 */
	MD_TRY(md_refgen()); /* shared REFGEN: GPLL0 / 6 = 100 MHz */
	MD_TRY(md_branch(0x8c00c));
	MD_TRY(md_branch(0x6f034));
	MD_TRY(md_branch(0x6f004));
	MD_TRY(md_branch(0x9000c));
	MD_TRY(md_branch(0x8d058));
	MD_TRY(md_update(MD_GCC+0x6014,0x6002,0));
	MD_TRY(md_update(MD_GCC+0x601c,0x6000,0));
	MD_TRY(md_update(MD_GCC+0x6024,2,0));
	MD_TRY(md_update(MD_GCC+0x602c,0x6000,0));
	MD_TRY(md_update(MD_GCC+0x52010,0,0x7c00));
	static const uint32_t gates[]={0x6010,0x6014,0x601c,0x6024,0x6028};
	for (unsigned i=0;i<5;i++) MD_TRY(md_wait(MD_GCC+gates[i],MD_BIT(31),0,10000));
	MD_TRY(md_update(MD_GCC+0x6000,0,1));
	MD_TRY(md_update(MD_GCC+0x1f01c,0,1)); delay(1000);
	MD_TRY(md_update(MD_GCC+0x6000,1,0));
	MD_TRY(md_update(MD_GCC+0x1f01c,1,0)); delay(1000);
	MD_LOG("clocks ready GDSC=on AUX=19200000 REFGEN=100000000\n");
	/* No controller/PHY reads occur above the clock and domain checks. */
	md_stage="PHY";
	md_write(MD_PARF+0x1000,4);
	MD_TRY(md_update(MD_PARF+0x40,1,0));
	md_address(MD_PARF+0x350,MD_DBI);
	md_address(MD_PARF+0x634,MD_ATU);
	MD_TRY(md_update(MD_PARF,MD_BIT(29),0));
	MD_TRY(md_update(MD_PARF+0x174,0,MD_BIT(4)));
	MD_TRY(md_update(MD_PARF+0x20,0,MD_BIT(5))); /* hold L0 during polling */
	MD_TRY(md_update(MD_PARF+0x1a8,0,MD_BIT(31)));
	md_write(MD_PARF+0x22c,0);
	MD_TRY(md_update(MD_PHY+0xa40,0,3));
	for (unsigned i=0;i<sizeof(md_phy_values)/sizeof(md_phy_values[0]);i++)
		md_write(MD_PHY+md_phy_values[i].offset,md_phy_values[i].value);
	MD_TRY(md_update(MD_GCC+0x52010,0,MD_BIT(15)));
	MD_TRY(md_update(MD_PHY+0xa00,1,0));
	MD_TRY(md_update(MD_PHY+0xa44,0,3)); delay(1000);
	MD_TRY(md_wait(MD_PHY+0xa14,MD_BIT(6),0,100000));
	MD_LOG("PHY ready QMP-v4 Gen3x2\n");
	md_stage="link";
	/* Mirror qcom_pcie_init_2_7_0(): RC mode, DBI/ATU windows, and the
	 * controller's MHI clock bypass before touching DBI link controls. */
	MD_LINK_TRY("parf-device-type",(md_write(MD_PARF+0x1000,4),0));
	MD_LINK_TRY("parf-phy-powerdown",md_update(MD_PARF+0x40,1,0));
	MD_LINK_TRY("parf-dbi-base",(md_address(MD_PARF+0x350,MD_DBI),0));
	MD_LINK_TRY("parf-atu-base",(md_address(MD_PARF+0x634,MD_ATU),0));
	MD_LINK_TRY("parf-slave-window",(md_write(MD_PARF+0x358,0),md_write(MD_PARF+0x35c,0x80000000u),0));
	MD_LINK_TRY("parf-sys-ctrl",md_update(MD_PARF,MD_BIT(29),0));
	MD_LINK_TRY("parf-mhi-bypass",md_update(MD_PARF+0x174,0,MD_BIT(4)));
	MD_LINK_TRY("parf-axi-halt",md_update(MD_PARF+0x1a8,0,MD_BIT(31)));
	/* DWC root port: x2 lane mode/count, Gen3 target, ASPM disabled. */
	MD_LINK_TRY("dbi-lane-count",md_update(MD_DBI+0x710,0x3f0000,3u<<16));
	MD_LINK_TRY("dbi-link-width",md_update(MD_DBI+0x80c,0x1f00,2u<<8));
	MD_LINK_TRY("dbi-link-speed",md_update(MD_DBI+0xa0,15,3));
	uint32_t ctl=md_read(MD_DBI+0x80);
	if (ctl==UINT32_MAX) { md_link_snapshot("dbi-link-read",EIO); return EIO; }
	md_write(MD_DBI+0x80,(ctl&0xffff)&~3u);
	md_write(MD_DBI+0xb40,20);
	md_link_snapshot("dbi-pre-perst",0);
	delay(100000); /* power/refclk stable before releasing PERST# */
	MD_LINK_TRY("perst-deassert",md_output(85,true,1)); delay(1000);
	/* gcc_pcie_2_pipe_clk is the branch fed by the PHY's registered
	 * pcie_2_pipe_clk output. Keep it voted on after PHY PHYSTATUS clears,
	 * matching qmp_pcie_power_on()'s pipe-clock enable. */
	MD_LINK_TRY("pipe-clock-enable",md_update(MD_GCC+0x52010,0,MD_BIT(15)));
	MD_LINK_TRY("ltssm-enable",md_update(MD_PARF+0x1b0,0,MD_BIT(8)));
	/* Linux qcom_pcie_start_link enables PARF_LTSSM after the 100 ms
	 * PERST hold. Keep the same order, but sample LTSSM for 3 seconds so a
	 * PBL endpoint has time to train before we call it a link failure. */
	int link_error = ETIMEDOUT;
	for (unsigned us = 0; us < 3000000; us += 10000) {
		uint32_t ltssm = md_read(MD_PARF+0x1b0);
		uint32_t elbi = md_read(MD_DBI+0xf28);
		uint32_t dbg = md_read(MD_DBI+0x72c);
		uint32_t status = md_read(MD_DBI+0x80);
		uint32_t phy_status=md_read(MD_PHY+0xa14),pipe_cbcr=md_read(MD_GCC+0x602c),
		    pipe_vote=md_read(MD_GCC+0x52010),sys_ctrl=md_read(MD_PARF);
		if (ltssm == UINT32_MAX || elbi == UINT32_MAX || dbg == UINT32_MAX ||
		    status == UINT32_MAX || phy_status == UINT32_MAX || pipe_cbcr == UINT32_MAX ||
		    pipe_vote == UINT32_MAX || sys_ctrl == UINT32_MAX) {
			MD_LOG("link wait us=%u EIO LTSSM=%#x ELBI_STTS=%#x PCS_STATUS=%#x PHY_START=%#x PIPE_RCG=%#x PIPE_CBCR=%#x PIPE_VOTE=%#x SYS_CTRL=%#x DBI_DBG=%#x DBI_STATUS=%#x\n",
			    us,ltssm,elbi,phy_status,md_read(MD_PHY+0xa44),md_read(MD_GCC+0x603c),
			    pipe_cbcr,pipe_vote,sys_ctrl,dbg,status);
			link_error = EIO;
			break;
		}
		if ((dbg & MD_BIT(4)) && !(dbg & MD_BIT(29)) && (status & MD_BIT(29))) {
			link_error = 0;
			break;
		}
		if ((us % 100000) == 0)
			MD_LOG("link wait us=%u LTSSM=%#x ELBI_STTS=%#x PCS_STATUS=%#x PHY_START=%#x PIPE_RCG=%#x PIPE_CBCR=%#x PIPE_VOTE=%#x SYS_CTRL=%#x DBI_DBG=%#x DBI_STATUS=%#x\n",
			    us,ltssm & 0x3fu,elbi,phy_status,md_read(MD_PHY+0xa44),md_read(MD_GCC+0x603c),
			    pipe_cbcr,pipe_vote,sys_ctrl,dbg,status);
		delay(10000);
	}
	MD_TRY(link_error);
	MD_LOG("link up status=%#x\n",md_read(MD_DBI+0x80));
	delay(100000); /* PCIe configuration recovery delay */
	md_stage="enumerate";
	md_write(MD_DBI+0x18,0x00010100);
	md_write(MD_DBI+0x20,0x64306430);
	MD_TRY(md_atu(0,4,MD_CONF,0x1000,0x01000000));
	uint32_t id=md_read(MD_CONF);
	MD_LOG("PCI ID=%#x expected=0x030617cb\n",id);
	if (id!=0x030617cb) return ENODEV;
	md_link=true;
	uint32_t command=md_read(MD_CONF+4),bar=md_read(MD_CONF+0x10);
	if (command==UINT32_MAX || bar==UINT32_MAX || (bar&1) || ((bar&6)!=0 && (bar&6)!=4)) return EIO;
	md_write(MD_CONF+4,((command&0xffff)&~7u)|MD_BIT(10));
	if (md_read(MD_CONF+4)&4) return EIO;
	md_write(MD_CONF+0x10,UINT32_MAX);
	uint32_t mask=md_read(MD_CONF+0x10)&~15u;
	md_bar_size=(~mask)+1;
	md_write(MD_CONF+0x10,MD_BAR|(bar&15));
	if ((bar&6)==4) md_write(MD_CONF+0x14,0);
	if (md_bar_size<0x1000 || md_bar_size>0x100000 || (md_bar_size&(md_bar_size-1))) return ERANGE;
	MD_TRY(md_atu(1,0,MD_BAR,md_bar_size,MD_BAR));
	md_write(MD_DBI+4,2);
	md_write(MD_CONF+4,((command&0xffff)&~7u)|2|MD_BIT(10));
	if ((md_read(MD_CONF+4)&7)!=2 || (md_read(MD_CONF+0x10)&~15u)!=MD_BAR) return EIO;
	MD_LOG("enumerated BAR0=%#x size=%#x DMA=off\n",MD_BAR,md_bar_size);
	return 0;
}
