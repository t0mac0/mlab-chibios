#include <stdio.h>
#include <string.h>

#include <ch.h>
#include <hal.h>
#include <test.h>
#include <shell.h>
#include <evtimer.h>
#include <chprintf.h>

#include "ff.h"

int connected = 0;
int tried = 0;


/*===========================================================================*/
/* Card insertion monitor.                                                   */
/*===========================================================================*/

#define SDC_POLLING_INTERVAL            10
#define SDC_POLLING_DELAY               10

static VirtualTimer tmr; /* Card monitor timer. */
static unsigned cnt;     /* Debounce counter. */
static EventSource inserted_event, removed_event; /* Card event sources. */

/* Insertion monitor function. */
bool_t sdc_lld_is_card_inserted(SDCDriver *sdcp) {
	(void)sdcp;
	return !palReadPad(GPIOB, GPIOB_SD_DETECT);
}

/* Protection detection. */
bool_t sdc_lld_is_write_protected(SDCDriver *sdcp) {
	
	(void)sdcp;
	return !palReadPad(GPIOB, GPIOB_SD_PROTECT);
}

/* Insertion monitor timer callback function. */
static void tmrfunc(void *p) {
	SDCDriver *sdcp = p;
	
	if (cnt > 0) {
		if (sdcIsCardInserted(sdcp)) {
			if (--cnt == 0) {
				chEvtBroadcastI(&inserted_event);
			}
		}
		else
			cnt = SDC_POLLING_INTERVAL;
	}
	else {
		if (!sdcIsCardInserted(sdcp)) {
			cnt = SDC_POLLING_INTERVAL;
			chEvtBroadcastI(&removed_event);
		}
	}
	chVTSetI(&tmr, MS2ST(SDC_POLLING_DELAY), tmrfunc, sdcp);
}

/* Polling monitor start. */
static void tmr_init(SDCDriver *sdcp) {
	
	chEvtInit(&inserted_event);
	chEvtInit(&removed_event);
	chSysLock();
	cnt = SDC_POLLING_INTERVAL;
	chVTSetI(&tmr, MS2ST(SDC_POLLING_DELAY), tmrfunc, sdcp);
	chSysUnlock();
}


/*===========================================================================*/
/* FatFs related.                                                            */
/*===========================================================================*/

/* FS object. */
FATFS SDC_FS;

/* FS mounted and ready.*/
static bool_t fs_ready = FALSE;

/* Generic large buffer.*/
uint8_t fbuff[1024];

static FRESULT scan_files(BaseChannel *chp, char *path) {
	FRESULT res;
	FILINFO fno;
	DIR dir;
	int i;
	char *fn;
	
#if _USE_LFN
	fno.lfname = 0;
	fno.lfsize = 0;
#endif
	res = f_opendir(&dir, path);
	if (res == FR_OK) {
		i = strlen(path);
		for (;;) {
			res = f_readdir(&dir, &fno);
			if (res != FR_OK || fno.fname[0] == 0)
				break;
			if (fno.fname[0] == '.')
				continue;
			fn = fno.fname;
			if (fno.fattrib & AM_DIR) {
				path[i++] = '/';
				strcpy(&path[i], fn);
				res = scan_files(chp, path);
				if (res != FR_OK)
					break;
				path[--i] = 0;
			}
			else {
				chprintf(chp, "%s/%s\r\n", path, fn);
			}
		}
	}
	return res;
}

/*===========================================================================*/
/* Command line related.                                                     */
/*===========================================================================*/

#define SHELL_WA_SIZE   THD_WA_SIZE(2048)
#define TEST_WA_SIZE    THD_WA_SIZE(256)

static void cmd_mem(BaseChannel *chp, int argc, char *argv[])
{
	size_t n, size;
	int i;
	
	(void)argv;
	if (argc > 0) {
		chprintf(chp, "Usage: mem\r\n");
		return;
	}
	n = chHeapStatus(NULL, &size);
	chprintf(chp, "core free memory : %u bytes\r\n", chCoreStatus());
	chprintf(chp, "heap fragments   : %u\r\n", n);
	chprintf(chp, "heap free total  : %u bytes\r\n", size);
}

static void cmd_threads(BaseChannel *chp, int argc, char *argv[])
{
	static const char *states[] = {THD_STATE_NAMES};
	Thread *tp;
	
	(void)argv;
	if (argc > 0) {
		chprintf(chp, "Usage: threads\r\n");
		return;
	}
	chprintf(chp, "    addr    stack prio refs     state time\r\n");
	tp = chRegFirstThread();
	do {
		chprintf(chp, "%.8lx %.8lx %4lu %4lu %9s %lu\r\n",
				 (uint32_t)tp, (uint32_t)tp->p_ctx.r13,
				 (uint32_t)tp->p_prio, (uint32_t)(tp->p_refs - 1),
				 states[tp->p_state], (uint32_t)tp->p_time);
		tp = chRegNextThread(tp);
	} while (tp != NULL);
}

static void cmd_test(BaseChannel *chp, int argc, char *argv[])
{
	Thread *tp;
	
	(void)argv;
	if (argc > 0) {
		chprintf(chp, "Usage: test\r\n");
		return;
	}
	tp = chThdCreateFromHeap(NULL, TEST_WA_SIZE, chThdGetPriority(),
							 TestThread, chp);
	chprintf(chp, "thread created\r\n");
	if (tp == NULL) {
		chprintf(chp, "out of memory\r\n");
		return;
	}
	chThdWait(tp);
}

static void cmd_count(BaseChannel *chp, int argc, char *argv[])
{
	(void)argc;
	(void)argv;
	while(1)
	{
		chprintf(chp, "detect %d\r\n", palReadPad(GPIOB, 5));
		chprintf(chp, "protect %d\r\n", palReadPad(GPIOB, 4));
		chThdSleepMilliseconds(500);
	}
}

static void cmd_tree(BaseChannel *chp, int argc, char *argv[]) {
	FRESULT err;
	uint32_t clusters;
	FATFS *fsp;
	
	(void)argv;
	if (argc > 0) {
		chprintf(chp, "Usage: tree\r\n");
		return;
	}
	if (!fs_ready) {
		chprintf(chp, "File System not mounted\r\n");
		return;
	}
	err = f_getfree("/", &clusters, &fsp);
	if (err != FR_OK) {
		chprintf(chp, "FS: f_getfree() failed\r\n");
		return;
	}
	chprintf(chp,
			 "FS: %lu free clusters, %lu sectors per cluster, %lu bytes free\r\n",
			 clusters, (uint32_t)SDC_FS.csize,
			 clusters * (uint32_t)SDC_FS.csize * (uint32_t)SDC_BLOCK_SIZE);
	fbuff[0] = 0;
	scan_files(chp, (char *)fbuff);
}

static void cmd_card(BaseChannel *chp, int argc, char *argv[]) {
	chprintf(chp, "DETECT %d, PROTECT %d\r\n",
			 palReadPad(GPIOB, GPIOB_SD_DETECT),
			 palReadPad(GPIOB, GPIOB_SD_PROTECT));

	chprintf(chp, "sdcIsCardInserted %d\r\n", sdcIsCardInserted(&SDCD1));
	chprintf(chp, "tried %d, connected %d\r\n", tried, connected);

}

static const ShellCommand commands[] = {
	{"mem", cmd_mem},
	{"threads", cmd_threads},
	{"test", cmd_test},
	{"count", cmd_count},
	{"tree", cmd_tree},
	{"card", cmd_card},
	{NULL, NULL}
};

static const ShellConfig shell_cfg1 = {
	(BaseChannel *)&SD2,
	commands
};


/*===========================================================================*/
/* Main and generic code.                                                    */
/*===========================================================================*/

/* SD card insertion event.  */
static void InsertHandler(eventid_t id) {
	FRESULT err;
	
	tried = 1;
	
	(void)id;
	/*
	 * On insertion SDC initialization and FS mount.
	 */
	if (sdcConnect(&SDCD1))
	{
		return;
	}
	
	err = f_mount(0, &SDC_FS);
	if (err != FR_OK) {
		sdcDisconnect(&SDCD1);
		return;
	}
	fs_ready = TRUE;
}

/* SD card removal event. */
static void RemoveHandler(eventid_t id) {
	
	(void)id;
	if (sdcGetDriverState(&SDCD1) == SDC_ACTIVE)
		sdcDisconnect(&SDCD1);
	fs_ready = FALSE;
}

/* Working area for the LED flashing thread. */
static WORKING_AREA(staticWA, 1280);

/* LED flashing thread. */
static msg_t staticThread(void *arg)
{
	(void)arg;
	while(1)
	{
		palTogglePad(GPIOB, GPIOB_LED1);
		chThdSleepMilliseconds(200);
	}
	return 0;
}

/*
 * main ()
 */

int main(void)
{
	static const evhandler_t evhndl[] = {
		InsertHandler,
		RemoveHandler
	};
	Thread *shelltp = NULL;
	struct EventListener el0, el1;

	/*
	 * System initializations.
	 * - HAL initialization, this also initializes the configured device drivers
	 *   and performs the board-specific initializations.
	 * - Kernel initialization, the main() function becomes a thread and the
	 *   RTOS is active.
	 */

	printf("MLAB ChibiOS SDcard demo \r\n");

	halInit();
	chSysInit();

	palSetPadMode(GPIOB, 7, PAL_MODE_OUTPUT_PUSHPULL);
	palSetPadMode(GPIOB, 5, 4);
	palSetPadMode(GPIOB, 4, 4);
	
	sdStart(&SD2, NULL);
	sdcStart(&SDCD1, NULL);

	/* Start the shell */
	shellInit();

	/* Activates the card insertion monitor. */
	tmr_init(&SDCD1);

	
	(void)chThdCreateStatic(staticWA, sizeof(staticWA), HIGHPRIO, staticThread, NULL);
	
	chEvtRegister(&inserted_event, &el0, 0);
	chEvtRegister(&removed_event, &el1, 1);
	while(1)
	{
		if (!shelltp)
		{
			shelltp = shellCreate(&shell_cfg1, SHELL_WA_SIZE, NORMALPRIO);
		}
		else if (chThdTerminated(shelltp))
		{
			chThdRelease(shelltp);    /* Recovers memory of the previous shell.   */
			shelltp = NULL;           /* Triggers spawning of a new shell.        */
		}
		chEvtDispatch(evhndl, chEvtWaitOne(ALL_EVENTS));
	}
	
	
}
