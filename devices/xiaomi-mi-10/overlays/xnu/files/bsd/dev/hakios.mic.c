/* CONFIG_HAKIOS_MIC builds this entry; CONFIG_HAKIOS_UMI selects this overlay. */
#if HAKIOS_UMI
#include "umi_mic.c"
#else
#error "UMI mic overlay requires CONFIG_HAKIOS_UMI=y"
#endif
