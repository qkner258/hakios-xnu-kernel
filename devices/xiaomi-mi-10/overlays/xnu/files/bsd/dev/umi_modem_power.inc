/* Included by umi_rpmh.c so every vote uses the existing shared client lock.
 * These are the same minimum voltages as Wi-Fi's PCIe rails. No release,
 * voltage reduction, SLEEP/WAKE reprogramming, or guessed 3.3 V supply.
 */
int umi_rpmh_modem_power(void);
int
umi_rpmh_modem_power(void)
{
	static const char *const names[] = { "ldoa5", "ldoa9" };
	static const unsigned mv[] = { 880, 1200 };
	uint32_t address[2];
	lck_mtx_lock(&rh_client_lock);
	if (__atomic_exchange_n(&rh_lock,1,__ATOMIC_ACQUIRE)) {
		lck_mtx_unlock(&rh_client_lock);return EBUSY;
	}
	rh_client = "modem";
	int error = rh_poisoned ? EIO : rh_map();
	for (unsigned i = 0; !error && i < 2; i++)
		error = rh_lookup(names[i], &address[i]);
	for (unsigned i = 0; !error && i < 2; i++)
		error = rh_send(address[i], mv[i], false);
	rh_client = NULL;
	__atomic_store_n(&rh_lock,0,__ATOMIC_RELEASE);
	lck_mtx_unlock(&rh_client_lock);
	printf("UMI MODEM: rails ldoa5=880 ldoa9=1200 errno=%d\n", error);
	return error;
}
