/*
 * Copyright (C) 2000 Paolo Mantegazza <mantegazza@aero.polimi.it>
 *               2002  Robert Schwebel  <robert@schwebel.de>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/stringify.h>
#include <asm/io.h>

#include <asm/rtai.h>
#include <rtai_sched.h>
#include <rtai_fifos.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Latency measurement tool for RTAI");
MODULE_AUTHOR("Paolo Mantegazza <mantegazza@aero.polimi.it>, Robert Schwebel <robert@schwebel.de>");


/* command line parameters */

#if defined(CONFIG_UCLINUX) || defined(CONFIG_ARM) || defined(CONFIG_COLDFIRE)
#define DEFAULT_PERIOD 1000000
#else
#define DEFAULT_PERIOD 100000
#endif

int period = DEFAULT_PERIOD;
RTAI_MODULE_PARM(period, int);
MODULE_PARM_DESC(period, "period in ns (default: " __stringify(DEFAULT_PERIOD) ")");

static int loops;
int avrgtime = 1;
RTAI_MODULE_PARM(avrgtime, int);
MODULE_PARM_DESC(avrgtime, "Averages are calculated for <avrgtime (s)> runs (default: 1)");

int use_fpu = 0;
RTAI_MODULE_PARM(use_fpu, int);
MODULE_PARM_DESC(use_fpu, "do we want to use the FPU? (default: 0)");

#define DEBUG_FIFO 1
#define TIMER_TO_CPU 3		// < 0  || > 1 to maintain a symmetric processed timer.
#define RUNNABLE_ON_CPUS 3	// 1: on cpu 0 only, 2: on cpu 1 only, 3: on any;
#define RUN_ON_CPUS (num_online_cpus() > 1 ? RUNNABLE_ON_CPUS : 1)

#define ECHOSPEED 1

/*
 *	Global Variables
 */

RT_TASK thread;
RTIME expected;
int period_counts;
struct sample {
	long long min;
	long long max;
	int index, ovrn;
} samp;
double dotres;

static int cpu_used[RTAI_NR_CPUS];

#define MAXDIM 10

#ifdef CONFIG_RTAI_FPU_SUPPORT
static double a[MAXDIM], b[MAXDIM];

static double dot(double *a, double *b, int n)
{
	int k; double s;
	for(k = n - 1, s = 0.0; k >= 0; k--) {
		s = s + a[k]*b[k];
	}
	return s;
}
#endif

/* Periodic realtime thread */
 
void fun(long thread)
{

	int diff = 0;
	int i;
	int average;
	int min_diff = 0;
	int max_diff = 0;
	int warmedup;

	rtf_put(DEBUG_FIFO, &period, sizeof(period));
	rtf_put(DEBUG_FIFO, &avrgtime, sizeof(avrgtime));

#ifdef CONFIG_RTAI_FPU_SUPPORT
	if (use_fpu) {
		for(i = 0; i < MAXDIM; i++) {
			a[i] = b[i] = 3.141592;
		}
	}
#endif

	warmedup = samp.ovrn = 0;
	while (1) {

		min_diff =  1000000000;
		max_diff = -1000000000;

		average = 0;

		for (i = 0; i < loops; i++) {
			cpu_used[rtai_cpuid()]++;
			expected += period_counts;

			if (!rt_task_wait_period()) {
				diff = (int) count2nano(rt_get_time() - expected);
			} else {
				samp.ovrn++;
				diff = 0;
			}

			if (diff < min_diff) { min_diff = diff; }
			if (diff > max_diff) { max_diff = diff; }
			average += diff;
#ifdef CONFIG_RTAI_FPU_SUPPORT
			if (use_fpu) {
				dotres = dot(a, b, MAXDIM);
			}
#endif
		}
		if (warmedup) {
			samp.min = min_diff;
			samp.max = max_diff;
			samp.index = average / loops;
			rtf_put(DEBUG_FIFO, &samp, sizeof (samp));
		}
		warmedup = 1;
	}
	rt_printk("\nDOT PRODUCT RESULT = %lu\n", (unsigned long)dotres);
}


/* Initialisation. */

static int __latency_init(void)
{
	/* XXX check option ranges here */

	rtf_create(DEBUG_FIFO, 16000);	/* create a fifo length: 16000 bytes */
	rt_linux_use_fpu(use_fpu);	/* declare if we use the FPU         */

	rt_task_init(			/* create our measuring task         */
			    &thread,	/* poiter to our RT_TASK             */
			    fun,	/* implementation of the task        */
			    0,		/* we could transfer data -> task    */
			    5000,	/* stack size                        */
			    0,		/* priority                          */
			    use_fpu,	/* do we use the FPU?                */
			    0		/* signal? XXX                       */
	);

	rt_set_runnable_on_cpus(	/* select on which CPUs the task is  */
		&thread,		/* allowed to run                    */
		RUN_ON_CPUS
	);

	period_counts = nano2count(period);

	loops = ((1000000000*avrgtime)/period)/ECHOSPEED;

	/* Calculate the start time for the task. */
	/* We set this to "now plus 10 periods"   */
	expected = rt_get_time() + 10 * period_counts;
	rt_task_make_periodic(&thread, expected, period_counts);
	return 0;
}


/* Cleanup */

static void
__latency_exit(void)
{
	int cpuid;

	/* Now delete our task and remove the FIFO. */
	rt_task_delete(&thread);
	rtf_destroy(DEBUG_FIFO);

	/* Output some statistics about CPU usage */
	printk("\n\nCPU USE SUMMARY\n");
	for (cpuid = 0; cpuid < RTAI_NR_CPUS; cpuid++) {
		printk("# %d -> %d\n", cpuid, cpu_used[cpuid]);
	}
	printk("END OF CPU USE SUMMARY\n\n");

}

module_init(__latency_init);
module_exit(__latency_exit);