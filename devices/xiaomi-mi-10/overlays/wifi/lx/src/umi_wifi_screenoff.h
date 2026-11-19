/* Included by lx_80211.c. Runs on the driver's workqueue, serialized with
 * interface teardown by wiphy->mtx and cancel_delayed_work_sync(). */
static void
umi_wifi_pm_work(struct work_struct *work)
{
	struct lx_radio *radio = container_of(work, struct lx_radio, pm_work.work);
	mutex_lock(&radio->wiphy->mtx);
	if (radio->pm_running && radio->vif != NULL && radio->joined && !radio->joining) {
		/* LX uses unsigned options: 0 disabled, 1 awake, 2 screen-off. */
		unsigned policy = lx_os_option("umi_screenoff_wifi_policy");
		bool ps = policy == 2 && radio->scan != 1;
		if (radio->vif->cfg.ps != ps) {
			radio->hw.conf.dynamic_ps_timeout = 100; /* ms; firmware rejects 0 */
			radio->vif->cfg.ps = ps;
			radio->ops->bss_info_changed(&radio->hw, radio->vif,
			    &radio->vif->bss_conf, BSS_CHANGED_PS);
			/* mac80211 callback is void: this is a request, not firmware proof. */
			lx_tell("umi-pm: Wi-Fi station PS=%u requested dtim=%u (check ath11k errors)\n",
			    ps, radio->vif->bss_conf.dtim_period);
		}
	}
	if (radio->pm_running) queue_delayed_work(radio->work, &radio->pm_work, msecs_to_jiffies(1000));
	mutex_unlock(&radio->wiphy->mtx);
}

static void
umi_wifi_pm_start(struct lx_radio *radio)
{
	if (!lx_os_option("umi_screenoff_wifi_policy") || radio->pm_running) return;
	/* Use the AP's actual DTIM period, never assume one beacon. */
	const uint8_t *tim = element(radio->target.elements, radio->target.elements_length, 5);
	if (tim == NULL || tim[1] < 4 || tim[3] == 0) {
		lx_tell("umi-pm: Wi-Fi PS unavailable: no valid beacon TIM/DTIM\n");
		return;
	}
	mutex_lock(&radio->wiphy->mtx);
	radio->vif->bss_conf.dtim_period = tim[3];
	radio->ops->bss_info_changed(&radio->hw, radio->vif,
	    &radio->vif->bss_conf, BSS_CHANGED_BEACON_INFO);
	INIT_DELAYED_WORK(&radio->pm_work, umi_wifi_pm_work);
	radio->pm_running = true;
	queue_delayed_work(radio->work, &radio->pm_work, msecs_to_jiffies(1000));
	mutex_unlock(&radio->wiphy->mtx);
}

static void
umi_wifi_pm_stop(struct lx_radio *radio)
{
	mutex_lock(&radio->wiphy->mtx);
	bool running = radio->pm_running;
	radio->pm_running = false;
	mutex_unlock(&radio->wiphy->mtx);
	if (!running) return;
	cancel_delayed_work_sync(&radio->pm_work);
	mutex_lock(&radio->wiphy->mtx);
	if (radio->vif != NULL && radio->vif->cfg.ps) {
		radio->vif->cfg.ps = false;
		radio->ops->bss_info_changed(&radio->hw, radio->vif,
		    &radio->vif->bss_conf, BSS_CHANGED_PS);
	}
	mutex_unlock(&radio->wiphy->mtx);
}
