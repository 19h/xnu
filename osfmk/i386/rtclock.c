/*
 * Copyright (c) 2000-2005 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
/*
 * @OSF_COPYRIGHT@
 */

/*
 *	File:		i386/rtclock.c
 *	Purpose:	Routines for handling the machine dependent
 *			real-time clock. Historically, this clock is
 *			generated by the Intel 8254 Programmable Interval
 *			Timer, but local apic timers are now used for
 *			this purpose with the master time reference being
 *			the cpu clock counted by the timestamp MSR.
 */

#include <platforms.h>
#include <mach_kdb.h>

#include <mach/mach_types.h>

#include <kern/cpu_data.h>
#include <kern/cpu_number.h>
#include <kern/clock.h>
#include <kern/host_notify.h>
#include <kern/macro_help.h>
#include <kern/misc_protos.h>
#include <kern/spl.h>
#include <kern/assert.h>
#include <mach/vm_prot.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>		/* for kernel_map */
#include <i386/ipl.h>
#include <i386/pit.h>
#include <i386/pio.h>
#include <i386/misc_protos.h>
#include <i386/proc_reg.h>
#include <i386/machine_cpu.h>
#include <i386/mp.h>
#include <i386/cpuid.h>
#include <i386/cpu_data.h>
#include <i386/cpu_threads.h>
#include <i386/perfmon.h>
#include <i386/machine_routines.h>
#include <i386/AT386/bbclock_entries.h>
#include <pexpert/pexpert.h>
#include <machine/limits.h>
#include <machine/commpage.h>
#include <sys/kdebug.h>

#define MAX(a,b) (((a)>(b))?(a):(b))
#define MIN(a,b) (((a)>(b))?(b):(a))

#define NSEC_PER_HZ			(NSEC_PER_SEC / 100) /* nsec per tick */

#define UI_CPUFREQ_ROUNDING_FACTOR	10000000

int		sysclk_config(void);

int		sysclk_init(void);

kern_return_t	sysclk_gettime(
	mach_timespec_t			*cur_time);

kern_return_t	sysclk_getattr(
	clock_flavor_t			flavor,
	clock_attr_t			attr,
	mach_msg_type_number_t	*count);

void		sysclk_setalarm(
	mach_timespec_t			*alarm_time);

/*
 * Lists of clock routines.
 */
struct clock_ops  sysclk_ops = {
	sysclk_config,			sysclk_init,
	sysclk_gettime,			0,
	sysclk_getattr,			0,
	sysclk_setalarm,
};

int		calend_config(void);

int		calend_init(void);

kern_return_t	calend_gettime(
	mach_timespec_t			*cur_time);

kern_return_t	calend_getattr(
	clock_flavor_t			flavor,
	clock_attr_t			attr,
	mach_msg_type_number_t	*count);

struct clock_ops calend_ops = {
	calend_config,			calend_init,
	calend_gettime,			0,
	calend_getattr,			0,
	0,
};

/* local data declarations */

static clock_timer_func_t	rtclock_timer_expire;

static timer_call_data_t	rtclock_alarm_timer;

static void	rtclock_alarm_expire(
			timer_call_param_t	p0,
			timer_call_param_t	p1);

struct	{
	mach_timespec_t			calend_offset;
	boolean_t			calend_is_set;

	int64_t				calend_adjtotal;
	int32_t				calend_adjdelta;

	uint32_t			boottime;

        mach_timebase_info_data_t	timebase_const;

	decl_simple_lock_data(,lock)	/* real-time clock device lock */
} rtclock;

boolean_t		rtc_initialized = FALSE;
clock_res_t		rtc_intr_nsec = NSEC_PER_HZ;	/* interrupt res */
uint64_t		rtc_cycle_count;	/* clocks in 1/20th second */
uint64_t		rtc_cyc_per_sec;	/* processor cycles per sec */
uint32_t		rtc_boot_frequency;	/* provided by 1st speed-step */
uint32_t		rtc_quant_scale;	/* clock to nanos multiplier */
uint32_t		rtc_quant_shift;	/* clock to nanos right shift */
uint64_t		rtc_decrementer_min;

static	mach_timebase_info_data_t	rtc_lapic_scale; /* nsec to lapic count */

/*
 *	Macros to lock/unlock real-time clock data.
 */
#define RTC_INTRS_OFF(s)		\
	(s) = splclock()

#define RTC_INTRS_ON(s)			\
	splx(s)

#define RTC_LOCK(s)			\
MACRO_BEGIN				\
	RTC_INTRS_OFF(s);		\
	simple_lock(&rtclock.lock);	\
MACRO_END

#define RTC_UNLOCK(s)			\
MACRO_BEGIN				\
	simple_unlock(&rtclock.lock);	\
	RTC_INTRS_ON(s);		\
MACRO_END

/*
 * i8254 control.  ** MONUMENT **
 *
 * The i8254 is a traditional PC device with some arbitrary characteristics.
 * Basically, it is a register that counts at a fixed rate and can be
 * programmed to generate an interrupt every N counts.  The count rate is
 * clknum counts per sec (see pit.h), historically 1193167=14.318MHz/12
 * but the more accurate value is 1193182=14.31818MHz/12. [14.31818 MHz being
 * the master crystal oscillator reference frequency since the very first PC.]
 * Various constants are computed based on this value, and we calculate
 * them at init time for execution efficiency.  To obtain sufficient
 * accuracy, some of the calculation are most easily done in floating
 * point and then converted to int.
 *
 */

/*
 * Forward decl.
 */

static uint64_t	rtc_set_cyc_per_sec(uint64_t cycles);
uint64_t	rtc_nanotime_read(void);

/*
 * create_mul_quant_GHZ
 *   create a constant used to multiply the TSC by to convert to nanoseconds.
 *   This is a 32 bit number and the TSC *MUST* have a frequency higher than
 *   1000Mhz for this routine to work.
 *
 * The theory here is that we know how many TSCs-per-sec the processor runs at.
 * Normally to convert this to nanoseconds you would multiply the current
 * timestamp by 1000000000 (a billion) then divide by TSCs-per-sec.
 * Unfortunatly the TSC is 64 bits which would leave us with 96 bit intermediate
 * results from the multiply that must be divided by.
 * Usually thats
 *   uint96 = tsc * numer
 *   nanos = uint96 / denom
 * Instead, we create this quant constant and it becomes the numerator,
 * the denominator can then be 0x100000000 which makes our division as simple as
 * forgetting the lower 32 bits of the result. We can also pass this number to
 * user space as the numer and pass 0xFFFFFFFF (RTC_FAST_DENOM) as the denom to
 * convert raw counts * to nanos. The difference is so small as to be
 * undetectable by anything.
 *
 * Unfortunatly we can not do this for sub GHZ processors. In this case, all
 * we do is pass the CPU speed in raw as the denom and we pass in 1000000000
 * as the numerator. No short cuts allowed
 */
#define RTC_FAST_DENOM	0xFFFFFFFF
inline static uint32_t
create_mul_quant_GHZ(int shift, uint32_t quant)
{
	return (uint32_t)((((uint64_t)NSEC_PER_SEC/20) << shift) / quant);
}
/*
 * This routine takes a value of raw TSC ticks and applies the passed mul_quant
 * generated by create_mul_quant() This is our internal routine for creating
 * nanoseconds.
 * Since we don't really have uint96_t this routine basically does this....
 *   uint96_t intermediate = (*value) * scale
 *   return (intermediate >> 32)
 */
inline static uint64_t
fast_get_nano_from_abs(uint64_t value, int scale)
{
    asm (" 	movl	%%edx,%%esi	\n\t"
         "      mull	%%ecx		\n\t"
         "      movl	%%edx,%%edi	\n\t"
         "      movl	%%esi,%%eax	\n\t"
         "      mull	%%ecx		\n\t"
         "      xorl	%%ecx,%%ecx	\n\t"	
         "      addl	%%edi,%%eax	\n\t"	
         "      adcl	%%ecx,%%edx	    "
		: "+A" (value)
		: "c" (scale)
		: "%esi", "%edi");
    return value;
}

/*
 * This routine basically does this...
 * ts.tv_sec = nanos / 1000000000;	create seconds
 * ts.tv_nsec = nanos % 1000000000;	create remainder nanos
 */
inline static mach_timespec_t 
nanos_to_timespec(uint64_t nanos)
{
	union {
		mach_timespec_t ts;
		uint64_t u64;
	} ret;
        ret.u64 = nanos;
        asm volatile("divl %1" : "+A" (ret.u64) : "r" (NSEC_PER_SEC));
        return ret.ts;
}

/*
 * The following two routines perform the 96 bit arithmetic we need to
 * convert generic absolute<->nanoseconds
 * The multiply routine takes a uint64_t and a uint32_t and returns the result
 * in a uint32_t[3] array.
 * The divide routine takes this uint32_t[3] array and divides it by a uint32_t
 * returning a uint64_t
 */
inline static void
longmul(uint64_t	*abstime, uint32_t multiplicand, uint32_t *result)
{
    asm volatile(
        " pushl	%%ebx			\n\t"	
        " movl	%%eax,%%ebx		\n\t"
        " movl	(%%eax),%%eax		\n\t"
        " mull	%%ecx			\n\t"
        " xchg	%%eax,%%ebx		\n\t"
        " pushl	%%edx			\n\t"
        " movl	4(%%eax),%%eax		\n\t"
        " mull	%%ecx			\n\t"
        " movl	%2,%%ecx		\n\t"
        " movl	%%ebx,(%%ecx)		\n\t"
        " popl	%%ebx			\n\t"
        " addl	%%ebx,%%eax		\n\t"
        " popl	%%ebx			\n\t"
        " movl	%%eax,4(%%ecx)		\n\t"
        " adcl	$0,%%edx		\n\t"
        " movl	%%edx,8(%%ecx)	// and save it"
        : : "a"(abstime), "c"(multiplicand), "m"(result));
    
}

inline static uint64_t
longdiv(uint32_t *numer, uint32_t denom)
{
    uint64_t	result;
    asm volatile(
        " pushl	%%ebx			\n\t"
        " movl	%%eax,%%ebx		\n\t"
        " movl	8(%%eax),%%edx		\n\t"
        " movl	4(%%eax),%%eax		\n\t"
        " divl	%%ecx			\n\t"
        " xchg	%%ebx,%%eax		\n\t"
        " movl	(%%eax),%%eax		\n\t"
        " divl	%%ecx			\n\t"
        " xchg	%%ebx,%%edx		\n\t"
        " popl	%%ebx			\n\t"
        : "=A"(result) : "a"(numer),"c"(denom));
    return result;
}

/*
 * Enable or disable timer 2.
 * Port 0x61 controls timer 2:
 *   bit 0 gates the clock,
 *   bit 1 gates output to speaker.
 */
inline static void
enable_PIT2(void)
{
    asm volatile(
        " inb   $0x61,%%al      \n\t"
        " and   $0xFC,%%al       \n\t"
        " or    $1,%%al         \n\t"
        " outb  %%al,$0x61      \n\t"
        : : : "%al" );
}

inline static void
disable_PIT2(void)
{
    asm volatile(
        " inb   $0x61,%%al      \n\t"
        " and   $0xFC,%%al      \n\t"
        " outb  %%al,$0x61      \n\t"
        : : : "%al" );
}

inline static void
set_PIT2(int value)
{
/*
 * First, tell the clock we are going to write 16 bits to the counter
 *   and enable one-shot mode (command 0xB8 to port 0x43)
 * Then write the two bytes into the PIT2 clock register (port 0x42).
 * Loop until the value is "realized" in the clock,
 * this happens on the next tick.
 */
    asm volatile(
        " movb  $0xB8,%%al      \n\t"
        " outb	%%al,$0x43	\n\t"
        " movb	%%dl,%%al	\n\t"
        " outb	%%al,$0x42	\n\t"
        " movb	%%dh,%%al	\n\t"
        " outb	%%al,$0x42	\n"
"1:	  inb	$0x42,%%al	\n\t" 
        " inb	$0x42,%%al	\n\t"
        " cmp	%%al,%%dh	\n\t"
        " jne	1b"
        : : "d"(value) : "%al");
}

inline static uint64_t
get_PIT2(unsigned int *value)
{
    register uint64_t	result;
/*
 * This routine first latches the time (command 0x80 to port 0x43),
 * then gets the time stamp so we know how long the read will take later.
 * Read (from port 0x42) and return the current value of the timer.
 */
    asm volatile(
        " xorl	%%ecx,%%ecx	\n\t"
        " movb	$0x80,%%al	\n\t"
        " outb	%%al,$0x43	\n\t"
        " rdtsc			\n\t"
        " pushl	%%eax		\n\t"
        " inb	$0x42,%%al	\n\t"
        " movb	%%al,%%cl	\n\t"
        " inb	$0x42,%%al	\n\t"
        " movb	%%al,%%ch	\n\t"
        " popl	%%eax	"
        : "=A"(result), "=c"(*value));
    return result;
}

/*
 * timeRDTSC()
 * This routine sets up PIT counter 2 to count down 1/20 of a second.
 * It pauses until the value is latched in the counter
 * and then reads the time stamp counter to return to the caller.
 */
static uint64_t
timeRDTSC(void)
{
    int		attempts = 0;
    uint64_t	latchTime;
    uint64_t	saveTime,intermediate;
    unsigned int timerValue, lastValue;
    boolean_t   int_enabled;
    /*
     * Table of correction factors to account for
     *   - timer counter quantization errors, and
     *   - undercounts 0..5
     */
#define	SAMPLE_CLKS_EXACT	(((double) CLKNUM) / 20.0)
#define	SAMPLE_CLKS_INT		((int) CLKNUM / 20)
#define SAMPLE_NSECS		(2000000000LL)
#define SAMPLE_MULTIPLIER	(((double)SAMPLE_NSECS)*SAMPLE_CLKS_EXACT)
#define ROUND64(x)		((uint64_t)((x) + 0.5))
    uint64_t	scale[6] = {
	ROUND64(SAMPLE_MULTIPLIER/(double)(SAMPLE_CLKS_INT-0)), 
	ROUND64(SAMPLE_MULTIPLIER/(double)(SAMPLE_CLKS_INT-1)), 
	ROUND64(SAMPLE_MULTIPLIER/(double)(SAMPLE_CLKS_INT-2)), 
	ROUND64(SAMPLE_MULTIPLIER/(double)(SAMPLE_CLKS_INT-3)), 
	ROUND64(SAMPLE_MULTIPLIER/(double)(SAMPLE_CLKS_INT-4)), 
	ROUND64(SAMPLE_MULTIPLIER/(double)(SAMPLE_CLKS_INT-5))
    };
                            
    int_enabled = ml_set_interrupts_enabled(FALSE);
    
restart:
    if (attempts >= 2)
	panic("timeRDTSC() calibation failed with %d attempts\n", attempts);
    attempts++;
    enable_PIT2();      // turn on PIT2
    set_PIT2(0);	// reset timer 2 to be zero
    latchTime = rdtsc64();	// get the time stamp to time 
    latchTime = get_PIT2(&timerValue) - latchTime; // time how long this takes
    set_PIT2(SAMPLE_CLKS_INT);	// set up the timer for (almost) 1/20th a second
    saveTime = rdtsc64();	// now time how long a 20th a second is...
    get_PIT2(&lastValue);
    get_PIT2(&lastValue);	// read twice, first value may be unreliable
    do {
        intermediate = get_PIT2(&timerValue);
        if (timerValue > lastValue) {
	    printf("Hey we are going backwards! %u -> %u, restarting timing\n",
			timerValue,lastValue);
	    set_PIT2(0);
	    disable_PIT2();
	    goto restart;
	}
        lastValue = timerValue;
    } while (timerValue > 5);
    kprintf("timerValue   %d\n",timerValue);
    kprintf("intermediate 0x%016llx\n",intermediate);
    kprintf("saveTime     0x%016llx\n",saveTime);
    
    intermediate -= saveTime;		// raw count for about 1/20 second
    intermediate *= scale[timerValue];	// rescale measured time spent
    intermediate /= SAMPLE_NSECS;	// so its exactly 1/20 a second
    intermediate += latchTime;		// add on our save fudge
    
    set_PIT2(0);			// reset timer 2 to be zero
    disable_PIT2();     		// turn off PIT 2

    ml_set_interrupts_enabled(int_enabled);
    return intermediate;
}

static uint64_t
tsc_to_nanoseconds(uint64_t abstime)
{
        uint32_t	numer;
        uint32_t	denom;
        uint32_t	intermediate[3];
        
        numer = rtclock.timebase_const.numer;
        denom = rtclock.timebase_const.denom;
        if (denom == RTC_FAST_DENOM) {
            abstime = fast_get_nano_from_abs(abstime, numer);
        } else {
            longmul(&abstime, numer, intermediate);
            abstime = longdiv(intermediate, denom);
        }
        return abstime;
}

inline static mach_timespec_t 
tsc_to_timespec(void)
{
        uint64_t	currNanos;
        currNanos = rtc_nanotime_read();
        return nanos_to_timespec(currNanos);
}

#define	DECREMENTER_MAX		UINT_MAX
static uint32_t
deadline_to_decrementer(
	uint64_t	deadline,
	uint64_t	now)
{
	uint64_t	delta;

	if (deadline <= now)
		return rtc_decrementer_min;
	else {
		delta = deadline - now;
		return MIN(MAX(rtc_decrementer_min,delta),DECREMENTER_MAX); 
	}
}

static inline uint64_t
lapic_time_countdown(uint32_t initial_count)
{
	boolean_t		state;
	uint64_t		start_time;
	uint64_t		stop_time;
	lapic_timer_count_t	count;

	state = ml_set_interrupts_enabled(FALSE);
	lapic_set_timer(FALSE, one_shot, divide_by_1, initial_count);
	start_time = rdtsc64();
	do {
		lapic_get_timer(NULL, NULL, NULL, &count);
	} while (count > 0);
	stop_time = rdtsc64();
	ml_set_interrupts_enabled(state);

	return tsc_to_nanoseconds(stop_time - start_time);
}

static void
rtc_lapic_timer_calibrate(void)
{
	uint32_t	nsecs;
	uint64_t	countdown;

	if (!(cpuid_features() & CPUID_FEATURE_APIC))
		return;

	/*
	 * Set the local apic timer counting down to zero without an interrupt.
	 * Use the timestamp to calculate how long this takes.
	 */ 
	nsecs = (uint32_t) lapic_time_countdown(rtc_intr_nsec);

	/*
	 * Compute a countdown ratio for a given time in nanoseconds.
	 * That is, countdown = time * numer / denom.
	 */
	countdown = (uint64_t)rtc_intr_nsec * (uint64_t)rtc_intr_nsec / nsecs;

	nsecs = (uint32_t) lapic_time_countdown((uint32_t) countdown);

	rtc_lapic_scale.numer = countdown;
	rtc_lapic_scale.denom = nsecs;

	kprintf("rtc_lapic_timer_calibrate() scale: %d/%d\n",
		(uint32_t) countdown, nsecs);
}

static void
rtc_lapic_set_timer(
	uint32_t	interval)
{
	uint64_t	count;

	assert(rtc_lapic_scale.denom);

	count = interval * (uint64_t) rtc_lapic_scale.numer;
	count /= rtc_lapic_scale.denom;

	lapic_set_timer(TRUE, one_shot, divide_by_1, (uint32_t) count);
}

static void
rtc_lapic_start_ticking(void)
{
	uint64_t	abstime;
	uint64_t	first_tick;
	uint64_t	decr;

	abstime = mach_absolute_time();
	first_tick = abstime + NSEC_PER_HZ;
	current_cpu_datap()->cpu_rtc_tick_deadline = first_tick;
	decr = deadline_to_decrementer(first_tick, abstime);
	rtc_lapic_set_timer(decr);
}

/*
 * Configure the real-time clock device. Return success (1)
 * or failure (0).
 */

int
sysclk_config(void)
{

	mp_disable_preemption();
	if (cpu_number() != master_cpu) {
		mp_enable_preemption();
		return(1);
	}
	mp_enable_preemption();

	timer_call_setup(&rtclock_alarm_timer, rtclock_alarm_expire, NULL);

	simple_lock_init(&rtclock.lock, 0);

	return (1);
}


/*
 * Nanotime/mach_absolutime_time
 * -----------------------------
 * The timestamp counter (tsc) - which counts cpu clock cycles and can be read
 * efficient by the kernel and in userspace - is the reference for all timing.
 * However, the cpu clock rate is not only platform-dependent but can change
 * (speed-step) dynamically. Hence tsc is converted into nanoseconds which is
 * identical to mach_absolute_time. The conversion to tsc to nanoseconds is
 * encapsulated by nanotime.
 *
 * The kernel maintains nanotime information recording:
 * 	- the current ratio of tsc to nanoseconds
 *	  with this ratio expressed as a 32-bit scale and shift
 *	  (power of 2 divider);
 *	- the tsc (step_tsc) and nanotime (step_ns) at which the current
 *	  ratio (clock speed) began.
 * So a tsc value can be converted to nanotime by:
 *
 *	nanotime = (((tsc - step_tsc)*scale) >> shift) + step_ns
 *
 * In general, (tsc - step_tsc) is a 64-bit quantity with the scaling
 * involving a 96-bit intermediate value. However, by saving the converted 
 * values at each tick (or at any intervening speed-step) - base_tsc and
 * base_ns - we can perform conversions relative to these and be assured that
 * (tsc - tick_tsc) is 32-bits. Hence:
 *
 * 	fast_nanotime = (((tsc - base_tsc)*scale) >> shift) + base_ns  
 *
 * The tuple {base_tsc, base_ns, scale, shift} is exported in the commpage 
 * for the userspace nanotime routine to read. A duplicate check_tsc is
 * appended so that the consistency of the read can be verified. Note that
 * this scheme is essential for MP systems in which the commpage is updated
 * by the master cpu but may be read concurrently by other cpus.
 * 
 */
static inline void
rtc_nanotime_set_commpage(rtc_nanotime_t *rntp)
{
	commpage_nanotime_t	cp_nanotime;

	/* Only the master cpu updates the commpage */
	if (cpu_number() != master_cpu)
		return;

	cp_nanotime.nt_base_tsc = rntp->rnt_tsc;
	cp_nanotime.nt_base_ns = rntp->rnt_nanos;
	cp_nanotime.nt_scale = rntp->rnt_scale;
	cp_nanotime.nt_shift = rntp->rnt_shift;

	commpage_set_nanotime(&cp_nanotime);
}

static void
rtc_nanotime_init(void)
{
	rtc_nanotime_t	*rntp = &current_cpu_datap()->cpu_rtc_nanotime;
	rtc_nanotime_t	*master_rntp = &cpu_datap(master_cpu)->cpu_rtc_nanotime;

	if (cpu_number() == master_cpu) {
		rntp->rnt_tsc = rdtsc64();
		rntp->rnt_nanos = tsc_to_nanoseconds(rntp->rnt_tsc);
		rntp->rnt_scale = rtc_quant_scale;
		rntp->rnt_shift = rtc_quant_shift;
		rntp->rnt_step_tsc = 0ULL;
		rntp->rnt_step_nanos = 0ULL;
	} else {
		/*
		 * Copy master processor's nanotime info.
		 * Loop required in case this changes while copying.
		 */
		do {
			*rntp = *master_rntp;
		} while (rntp->rnt_tsc != master_rntp->rnt_tsc);
	}
}

static inline void
_rtc_nanotime_update(rtc_nanotime_t *rntp, uint64_t	tsc)
{
	uint64_t	tsc_delta;
	uint64_t	ns_delta;

	tsc_delta = tsc - rntp->rnt_step_tsc;
	ns_delta = tsc_to_nanoseconds(tsc_delta);
	rntp->rnt_nanos = rntp->rnt_step_nanos + ns_delta;
	rntp->rnt_tsc = tsc;
}

static void
rtc_nanotime_update(void)
{
	rtc_nanotime_t	*rntp = &current_cpu_datap()->cpu_rtc_nanotime;

	assert(get_preemption_level() > 0);
	assert(!ml_get_interrupts_enabled());
        
	_rtc_nanotime_update(rntp, rdtsc64());
	rtc_nanotime_set_commpage(rntp);
}

static void
rtc_nanotime_scale_update(void)
{
	rtc_nanotime_t	*rntp = &current_cpu_datap()->cpu_rtc_nanotime;
	uint64_t	tsc = rdtsc64();

	assert(!ml_get_interrupts_enabled());
        
	/*
	 * Update time based on past scale.
	 */
	_rtc_nanotime_update(rntp, tsc);

	/*
	 * Update scale and timestamp this update.
	 */
	rntp->rnt_scale = rtc_quant_scale;
	rntp->rnt_shift = rtc_quant_shift;
	rntp->rnt_step_tsc = rntp->rnt_tsc;
	rntp->rnt_step_nanos = rntp->rnt_nanos;

	/* Export update to userland */
	rtc_nanotime_set_commpage(rntp);
}

static uint64_t
_rtc_nanotime_read(void)
{
	rtc_nanotime_t	*rntp = &current_cpu_datap()->cpu_rtc_nanotime;
	uint64_t	rnt_tsc;
	uint32_t	rnt_scale;
	uint32_t	rnt_shift;
	uint64_t	rnt_nanos;
	uint64_t	tsc;
	uint64_t	tsc_delta;

	rnt_scale = rntp->rnt_scale;
	if (rnt_scale == 0)
		return 0ULL;

	rnt_shift = rntp->rnt_shift;
	rnt_nanos = rntp->rnt_nanos;
	rnt_tsc = rntp->rnt_tsc;
	tsc = rdtsc64();

	tsc_delta = tsc - rnt_tsc;
	if ((tsc_delta >> 32) != 0)
		return rnt_nanos + tsc_to_nanoseconds(tsc_delta);

	/* Let the compiler optimize(?): */
	if (rnt_shift == 32)
		return rnt_nanos + ((tsc_delta * rnt_scale) >> 32);	
	else 
		return rnt_nanos + ((tsc_delta * rnt_scale) >> rnt_shift);
}

uint64_t
rtc_nanotime_read(void)
{
	uint64_t	result;
	uint64_t	rnt_tsc;
	rtc_nanotime_t	*rntp = &current_cpu_datap()->cpu_rtc_nanotime;

	/*
	 * Use timestamp to ensure the uptime record isn't changed.
	 * This avoids disabling interrupts.
	 * And not this is a per-cpu structure hence no locking.
	 */
	do {
		rnt_tsc = rntp->rnt_tsc;
		result = _rtc_nanotime_read();
	} while (rnt_tsc != rntp->rnt_tsc);

	return result;
}


/*
 * This function is called by the speed-step driver when a
 * change of cpu clock frequency is about to occur.
 * The scale is not changed until rtc_clock_stepped() is called.
 * Between these times there is an uncertainty is exactly when
 * the change takes effect. FIXME: by using another timing source
 * we could eliminate this error.
 */
void
rtc_clock_stepping(__unused uint32_t new_frequency,
		   __unused uint32_t old_frequency)
{
	boolean_t	istate;

	istate = ml_set_interrupts_enabled(FALSE);
	rtc_nanotime_scale_update();
	ml_set_interrupts_enabled(istate);
}

/*
 * This function is called by the speed-step driver when a
 * change of cpu clock frequency has just occured. This change
 * is expressed as a ratio relative to the boot clock rate.
 */
void
rtc_clock_stepped(uint32_t new_frequency, uint32_t old_frequency)
{
	boolean_t	istate;

	istate = ml_set_interrupts_enabled(FALSE);
	if (rtc_boot_frequency == 0) {
		/*
		 * At the first ever stepping, old frequency is the real
		 * initial clock rate. This step and all others are based
		 * relative to this initial frequency at which the tsc
		 * calibration was made. Hence we must remember this base
		 * frequency as reference.
		 */
		rtc_boot_frequency = old_frequency;
	}
	rtc_set_cyc_per_sec(rtc_cycle_count * new_frequency /
				rtc_boot_frequency);
	rtc_nanotime_scale_update();
	ml_set_interrupts_enabled(istate);
}

/*
 * rtc_sleep_wakeup() is called from acpi on awakening from a S3 sleep
 */
void
rtc_sleep_wakeup(void)
{
	rtc_nanotime_t	*rntp = &current_cpu_datap()->cpu_rtc_nanotime;

	boolean_t	istate;

	istate = ml_set_interrupts_enabled(FALSE);

	/*
	 * Reset nanotime.
	 * The timestamp counter will have been reset
	 * but nanotime (uptime) marches onward.
	 * We assume that we're still at the former cpu frequency.
	 */
	rntp->rnt_tsc = rdtsc64();
	rntp->rnt_step_tsc = 0ULL;
	rntp->rnt_step_nanos = rntp->rnt_nanos;
	rtc_nanotime_set_commpage(rntp);

	/* Restart tick interrupts from the LAPIC timer */
	rtc_lapic_start_ticking();

	ml_set_interrupts_enabled(istate);
}

/*
 * Initialize the real-time clock device.
 * In addition, various variables used to support the clock are initialized.
 */
int
sysclk_init(void)
{
	uint64_t	cycles;

	mp_disable_preemption();
	if (cpu_number() == master_cpu) {
		/*
		 * Perform calibration.
		 * The PIT is used as the reference to compute how many
		 * TCS counts (cpu clock cycles) occur per second.
		 */
        	rtc_cycle_count = timeRDTSC();
		cycles = rtc_set_cyc_per_sec(rtc_cycle_count);

		/*
		 * Set min/max to actual.
		 * ACPI may update these later if speed-stepping is detected.
		 */
        	gPEClockFrequencyInfo.cpu_frequency_min_hz = cycles;
        	gPEClockFrequencyInfo.cpu_frequency_max_hz = cycles;
		printf("[RTCLOCK] frequency %llu (%llu)\n",
		       cycles, rtc_cyc_per_sec);

		rtc_lapic_timer_calibrate();

		/* Minimum interval is 1usec */
		rtc_decrementer_min = deadline_to_decrementer(NSEC_PER_USEC,
								0ULL);
		/* Point LAPIC interrupts to hardclock() */
		lapic_set_timer_func((i386_intr_func_t) rtclock_intr);

		clock_timebase_init();
		rtc_initialized = TRUE;
	}

	rtc_nanotime_init();

	rtc_lapic_start_ticking();

	mp_enable_preemption();

	return (1);
}

/*
 * Get the clock device time. This routine is responsible
 * for converting the device's machine dependent time value
 * into a canonical mach_timespec_t value.
 */
static kern_return_t
sysclk_gettime_internal(
	mach_timespec_t	*cur_time)	/* OUT */
{
	*cur_time = tsc_to_timespec();
	return (KERN_SUCCESS);
}

kern_return_t
sysclk_gettime(
	mach_timespec_t	*cur_time)	/* OUT */
{
	return sysclk_gettime_internal(cur_time);
}

void
sysclk_gettime_interrupts_disabled(
	mach_timespec_t	*cur_time)	/* OUT */
{
	(void) sysclk_gettime_internal(cur_time);
}

// utility routine 
// Code to calculate how many processor cycles are in a second...

static uint64_t
rtc_set_cyc_per_sec(uint64_t cycles)
{

        if (cycles > (NSEC_PER_SEC/20)) {
            // we can use just a "fast" multiply to get nanos
	    rtc_quant_shift = 32;
            rtc_quant_scale = create_mul_quant_GHZ(rtc_quant_shift, cycles);
            rtclock.timebase_const.numer = rtc_quant_scale; // timeRDTSC is 1/20
	    rtclock.timebase_const.denom = RTC_FAST_DENOM;
        } else {
	    rtc_quant_shift = 26;
            rtc_quant_scale = create_mul_quant_GHZ(rtc_quant_shift, cycles);
            rtclock.timebase_const.numer = NSEC_PER_SEC/20; // timeRDTSC is 1/20
            rtclock.timebase_const.denom = cycles;
        }
	rtc_cyc_per_sec = cycles*20;	// multiply it by 20 and we are done..
					// BUT we also want to calculate...

        cycles = ((rtc_cyc_per_sec + (UI_CPUFREQ_ROUNDING_FACTOR/2))
			/ UI_CPUFREQ_ROUNDING_FACTOR)
				* UI_CPUFREQ_ROUNDING_FACTOR;

	/*
	 * Set current measured speed.
	 */
        if (cycles >= 0x100000000ULL) {
            gPEClockFrequencyInfo.cpu_clock_rate_hz = 0xFFFFFFFFUL;
        } else {
            gPEClockFrequencyInfo.cpu_clock_rate_hz = (unsigned long)cycles;
        }
        gPEClockFrequencyInfo.cpu_frequency_hz = cycles;

	kprintf("[RTCLOCK] frequency %llu (%llu)\n", cycles, rtc_cyc_per_sec);
	return(cycles);
}

void
clock_get_system_microtime(
	uint32_t			*secs,
	uint32_t			*microsecs)
{
	mach_timespec_t		now;

	(void) sysclk_gettime_internal(&now);

	*secs = now.tv_sec;
	*microsecs = now.tv_nsec / NSEC_PER_USEC;
}

void
clock_get_system_nanotime(
	uint32_t			*secs,
	uint32_t			*nanosecs)
{
	mach_timespec_t		now;

	(void) sysclk_gettime_internal(&now);

	*secs = now.tv_sec;
	*nanosecs = now.tv_nsec;
}

/*
 * Get clock device attributes.
 */
kern_return_t
sysclk_getattr(
	clock_flavor_t		flavor,
	clock_attr_t		attr,		/* OUT */
	mach_msg_type_number_t	*count)		/* IN/OUT */
{
	if (*count != 1)
		return (KERN_FAILURE);
	switch (flavor) {

	case CLOCK_GET_TIME_RES:	/* >0 res */
		*(clock_res_t *) attr = rtc_intr_nsec;
		break;

	case CLOCK_ALARM_CURRES:	/* =0 no alarm */
	case CLOCK_ALARM_MAXRES:
	case CLOCK_ALARM_MINRES:
		*(clock_res_t *) attr = 0;
		break;

	default:
		return (KERN_INVALID_VALUE);
	}
	return (KERN_SUCCESS);
}

/*
 * Set next alarm time for the clock device. This call
 * always resets the time to deliver an alarm for the
 * clock.
 */
void
sysclk_setalarm(
	mach_timespec_t	*alarm_time)
{
	timer_call_enter(&rtclock_alarm_timer,
			 (uint64_t) alarm_time->tv_sec * NSEC_PER_SEC
				+ alarm_time->tv_nsec);
}

/*
 * Configure the calendar clock.
 */
int
calend_config(void)
{
	return bbc_config();
}

/*
 * Initialize calendar clock.
 */
int
calend_init(void)
{
	return (1);
}

/*
 * Get the current clock time.
 */
kern_return_t
calend_gettime(
	mach_timespec_t	*cur_time)	/* OUT */
{
	spl_t		s;

	RTC_LOCK(s);
	if (!rtclock.calend_is_set) {
		RTC_UNLOCK(s);
		return (KERN_FAILURE);
	}

	(void) sysclk_gettime_internal(cur_time);
	ADD_MACH_TIMESPEC(cur_time, &rtclock.calend_offset);
	RTC_UNLOCK(s);

	return (KERN_SUCCESS);
}

void
clock_get_calendar_microtime(
	uint32_t			*secs,
	uint32_t			*microsecs)
{
	mach_timespec_t		now;

	calend_gettime(&now);

	*secs = now.tv_sec;
	*microsecs = now.tv_nsec / NSEC_PER_USEC;
}

void
clock_get_calendar_nanotime(
	uint32_t			*secs,
	uint32_t			*nanosecs)
{
	mach_timespec_t		now;

	calend_gettime(&now);

	*secs = now.tv_sec;
	*nanosecs = now.tv_nsec;
}

void
clock_set_calendar_microtime(
	uint32_t			secs,
	uint32_t			microsecs)
{
	mach_timespec_t		new_time, curr_time;
	uint32_t			old_offset;
	spl_t		s;

	new_time.tv_sec = secs;
	new_time.tv_nsec = microsecs * NSEC_PER_USEC;

	RTC_LOCK(s);
	old_offset = rtclock.calend_offset.tv_sec;
	(void) sysclk_gettime_internal(&curr_time);
	rtclock.calend_offset = new_time;
	SUB_MACH_TIMESPEC(&rtclock.calend_offset, &curr_time);
	rtclock.boottime += rtclock.calend_offset.tv_sec - old_offset;
	rtclock.calend_is_set = TRUE;
	RTC_UNLOCK(s);

	(void) bbc_settime(&new_time);

	host_notify_calendar_change();
}

/*
 * Get clock device attributes.
 */
kern_return_t
calend_getattr(
	clock_flavor_t		flavor,
	clock_attr_t		attr,		/* OUT */
	mach_msg_type_number_t	*count)		/* IN/OUT */
{
	if (*count != 1)
		return (KERN_FAILURE);
	switch (flavor) {

	case CLOCK_GET_TIME_RES:	/* >0 res */
		*(clock_res_t *) attr = rtc_intr_nsec;
		break;

	case CLOCK_ALARM_CURRES:	/* =0 no alarm */
	case CLOCK_ALARM_MINRES:
	case CLOCK_ALARM_MAXRES:
		*(clock_res_t *) attr = 0;
		break;

	default:
		return (KERN_INVALID_VALUE);
	}
	return (KERN_SUCCESS);
}

#define tickadj		(40*NSEC_PER_USEC)	/* "standard" skew, ns / tick */
#define	bigadj		(NSEC_PER_SEC)		/* use 10x skew above bigadj ns */

uint32_t
clock_set_calendar_adjtime(
	int32_t				*secs,
	int32_t				*microsecs)
{
	int64_t			total, ototal;
	uint32_t		interval = 0;
	spl_t			s;

	total = (int64_t)*secs * NSEC_PER_SEC + *microsecs * NSEC_PER_USEC;

	RTC_LOCK(s);
	ototal = rtclock.calend_adjtotal;

	if (total != 0) {
		int32_t		delta = tickadj;

		if (total > 0) {
			if (total > bigadj)
				delta *= 10;
			if (delta > total)
				delta = total;
		}
		else {
			if (total < -bigadj)
				delta *= 10;
			delta = -delta;
			if (delta < total)
				delta = total;
		}

		rtclock.calend_adjtotal = total;
		rtclock.calend_adjdelta = delta;

		interval = NSEC_PER_HZ;
	}
	else
		rtclock.calend_adjdelta = rtclock.calend_adjtotal = 0;

	RTC_UNLOCK(s);

	if (ototal == 0)
		*secs = *microsecs = 0;
	else {
		*secs = ototal / NSEC_PER_SEC;
		*microsecs = ototal % NSEC_PER_SEC;
	}

	return (interval);
}

uint32_t
clock_adjust_calendar(void)
{
	uint32_t		interval = 0;
	int32_t			delta;
	spl_t			s;

	RTC_LOCK(s);
	delta = rtclock.calend_adjdelta;
	ADD_MACH_TIMESPEC_NSEC(&rtclock.calend_offset, delta);

	rtclock.calend_adjtotal -= delta;

	if (delta > 0) {
		if (delta > rtclock.calend_adjtotal)
			rtclock.calend_adjdelta = rtclock.calend_adjtotal;
	}
	else
	if (delta < 0) {
		if (delta < rtclock.calend_adjtotal)
			rtclock.calend_adjdelta = rtclock.calend_adjtotal;
	}

	if (rtclock.calend_adjdelta != 0)
		interval = NSEC_PER_HZ;

	RTC_UNLOCK(s);

	return (interval);
}

void
clock_initialize_calendar(void)
{
	mach_timespec_t	bbc_time, curr_time;
	spl_t		s;

	if (bbc_gettime(&bbc_time) != KERN_SUCCESS)
		return;

	RTC_LOCK(s);
	if (rtclock.boottime == 0)
		rtclock.boottime = bbc_time.tv_sec;
	(void) sysclk_gettime_internal(&curr_time);
	rtclock.calend_offset = bbc_time;
	SUB_MACH_TIMESPEC(&rtclock.calend_offset, &curr_time);
	rtclock.calend_is_set = TRUE;
	RTC_UNLOCK(s);

	host_notify_calendar_change();
}

void
clock_get_boottime_nanotime(
	uint32_t			*secs,
	uint32_t			*nanosecs)
{
	*secs = rtclock.boottime;
	*nanosecs = 0;
}

void
clock_timebase_info(
	mach_timebase_info_t	info)
{
	info->numer = info->denom =  1;
}	

void
clock_set_timer_deadline(
	uint64_t			deadline)
{
	spl_t		s;
	cpu_data_t	*pp = current_cpu_datap();
	rtclock_timer_t	*mytimer = &pp->cpu_rtc_timer;
	uint64_t	abstime;
	uint64_t	decr;

	assert(get_preemption_level() > 0);
	assert(rtclock_timer_expire);

	RTC_INTRS_OFF(s);
	mytimer->deadline = deadline;
	mytimer->is_set = TRUE;
	if (!mytimer->has_expired) {
		abstime = mach_absolute_time();
		if (mytimer->deadline < pp->cpu_rtc_tick_deadline) {
			decr = deadline_to_decrementer(mytimer->deadline,
						       abstime);
			rtc_lapic_set_timer(decr);
			pp->cpu_rtc_intr_deadline = mytimer->deadline;
			KERNEL_DEBUG_CONSTANT(
				MACHDBG_CODE(DBG_MACH_EXCP_DECI, 1) |
					DBG_FUNC_NONE, decr, 2, 0, 0, 0);
		}
	}
	RTC_INTRS_ON(s);
}

void
clock_set_timer_func(
	clock_timer_func_t		func)
{
	if (rtclock_timer_expire == NULL)
		rtclock_timer_expire = func;
}

/*
 * Real-time clock device interrupt.
 */
void
rtclock_intr(struct i386_interrupt_state *regs)
{
	uint64_t	abstime;
	uint32_t	latency;
	uint64_t	decr;
	uint64_t	decr_tick;
	uint64_t	decr_timer;
	cpu_data_t	*pp = current_cpu_datap();
	rtclock_timer_t	*mytimer = &pp->cpu_rtc_timer;

	assert(get_preemption_level() > 0);
	assert(!ml_get_interrupts_enabled());

        abstime = _rtc_nanotime_read();
	latency = (uint32_t) abstime - pp->cpu_rtc_intr_deadline;
	if (pp->cpu_rtc_tick_deadline <= abstime) {
		rtc_nanotime_update();
		clock_deadline_for_periodic_event(
			NSEC_PER_HZ, abstime, &pp->cpu_rtc_tick_deadline);
		hertz_tick(
#if STAT_TIME
			   NSEC_PER_HZ,
#endif
			   (regs->efl & EFL_VM) || ((regs->cs & 0x03) != 0),
			   regs->eip);
	}

	abstime = _rtc_nanotime_read();
	if (mytimer->is_set && mytimer->deadline <= abstime) {
		mytimer->has_expired = TRUE;
		mytimer->is_set = FALSE;
		(*rtclock_timer_expire)(abstime);
		assert(!ml_get_interrupts_enabled());
		mytimer->has_expired = FALSE;
	}

	/* Log the interrupt service latency (-ve value expected by tool) */
	KERNEL_DEBUG_CONSTANT(
		MACHDBG_CODE(DBG_MACH_EXCP_DECI, 0) | DBG_FUNC_NONE,
		-latency, (uint32_t)regs->eip, 0, 0, 0);

	abstime = _rtc_nanotime_read();
	decr_tick = deadline_to_decrementer(pp->cpu_rtc_tick_deadline, abstime);
	decr_timer = (mytimer->is_set) ?
			deadline_to_decrementer(mytimer->deadline, abstime) :
			DECREMENTER_MAX;
	decr = MIN(decr_tick, decr_timer);
	pp->cpu_rtc_intr_deadline = abstime + decr;

	rtc_lapic_set_timer(decr);

	/* Log the new decrementer value */
	KERNEL_DEBUG_CONSTANT(
		MACHDBG_CODE(DBG_MACH_EXCP_DECI, 1) | DBG_FUNC_NONE,
		decr, 3, 0, 0, 0);

}

static void
rtclock_alarm_expire(
	__unused timer_call_param_t	p0,
	__unused timer_call_param_t	p1)
{
	mach_timespec_t	clock_time;

	(void) sysclk_gettime_internal(&clock_time);

	clock_alarm_intr(SYSTEM_CLOCK, &clock_time);
}

void
clock_get_uptime(
	uint64_t		*result)
{
        *result = rtc_nanotime_read();
}

uint64_t
mach_absolute_time(void)
{
        return rtc_nanotime_read();
}

void
absolutetime_to_microtime(
	uint64_t			abstime,
	uint32_t			*secs,
	uint32_t			*microsecs)
{
	uint32_t	remain;

	asm volatile(
			"divl %3"
				: "=a" (*secs), "=d" (remain)
				: "A" (abstime), "r" (NSEC_PER_SEC));
	asm volatile(
			"divl %3"
				: "=a" (*microsecs)
				: "0" (remain), "d" (0), "r" (NSEC_PER_USEC));
}

void
clock_interval_to_deadline(
	uint32_t		interval,
	uint32_t		scale_factor,
	uint64_t		*result)
{
	uint64_t		abstime;

	clock_get_uptime(result);

	clock_interval_to_absolutetime_interval(interval, scale_factor, &abstime);

	*result += abstime;
}

void
clock_interval_to_absolutetime_interval(
	uint32_t		interval,
	uint32_t		scale_factor,
	uint64_t		*result)
{
	*result = (uint64_t)interval * scale_factor;
}

void
clock_absolutetime_interval_to_deadline(
	uint64_t		abstime,
	uint64_t		*result)
{
	clock_get_uptime(result);

	*result += abstime;
}

void
absolutetime_to_nanoseconds(
	uint64_t		abstime,
	uint64_t		*result)
{
	*result = abstime;
}

void
nanoseconds_to_absolutetime(
	uint64_t		nanoseconds,
	uint64_t		*result)
{
	*result = nanoseconds;
}

void
machine_delay_until(
	uint64_t		deadline)
{
	uint64_t		now;

	do {
		cpu_pause();
		now = mach_absolute_time();
	} while (now < deadline);
}
