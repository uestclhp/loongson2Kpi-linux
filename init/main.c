/*
 *  linux/init/main.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  GK 2/5/95  -  Changed to support mounting root fs via NFS
 *  Added initrd & change_root: Werner Almesberger & Hans Lermen, Feb '96
 *  Moan early if gcc is old, avoiding bogus kernels - Paul Gortmaker, May '96
 *  Simplified starting of init:  Michael A. Griffith <grif@acm.org> 
 */

#define DEBUG		/* Enable initcall_debug */

#include <linux/types.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/stackprotector.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/initrd.h>
#include <linux/bootmem.h>
#include <linux/acpi.h>
#include <linux/tty.h>
#include <linux/percpu.h>
#include <linux/kmod.h>
#include <linux/vmalloc.h>
#include <linux/kernel_stat.h>
#include <linux/start_kernel.h>
#include <linux/security.h>
#include <linux/smp.h>
#include <linux/profile.h>
#include <linux/rcupdate.h>
#include <linux/moduleparam.h>
#include <linux/kallsyms.h>
#include <linux/writeback.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/cgroup.h>
#include <linux/efi.h>
#include <linux/tick.h>
#include <linux/interrupt.h>
#include <linux/taskstats_kern.h>
#include <linux/delayacct.h>
#include <linux/unistd.h>
#include <linux/rmap.h>
#include <linux/mempolicy.h>
#include <linux/key.h>
#include <linux/buffer_head.h>
#include <linux/page_cgroup.h>
#include <linux/debug_locks.h>
#include <linux/debugobjects.h>
#include <linux/lockdep.h>
#include <linux/kmemleak.h>
#include <linux/pid_namespace.h>
#include <linux/device.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/signal.h>
#include <linux/idr.h>
#include <linux/kgdb.h>
#include <linux/ftrace.h>
#include <linux/async.h>
#include <linux/kmemcheck.h>
#include <linux/sfi.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/perf_event.h>
#include <linux/file.h>
#include <linux/ptrace.h>
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/random.h>

#include <asm/io.h>
#include <asm/bugs.h>
#include <asm/setup.h>
#include <asm/sections.h>
#include <asm/cacheflush.h>

#ifdef CONFIG_X86_LOCAL_APIC
#include <asm/smp.h>
#endif

static int kernel_init(void *);

extern void init_IRQ(void);
extern void fork_init(unsigned long);
extern void mca_init(void);
extern void sbus_init(void);
extern void radix_tree_init(void);
#ifndef CONFIG_DEBUG_RODATA
static inline void mark_rodata_ro(void) { }
#endif

#ifdef CONFIG_TC
extern void tc_init(void);
#endif

/*
 * Debug helper: via this flag we know that we are in 'early bootup code'
 * where only the boot processor is running with IRQ disabled.  This means
 * two things - IRQ must not be enabled before the flag is cleared and some
 * operations which are not allowed with IRQ disabled are allowed while the
 * flag is set.
 */
bool early_boot_irqs_disabled __read_mostly;

enum system_states system_state __read_mostly;
EXPORT_SYMBOL(system_state);

/*
 * Boot command-line arguments
 */
#define MAX_INIT_ARGS CONFIG_INIT_ENV_ARG_LIMIT
#define MAX_INIT_ENVS CONFIG_INIT_ENV_ARG_LIMIT

extern void time_init(void);
/* Default late time init is NULL. archs can override this later. */
void (*__initdata late_time_init)(void);
extern void softirq_init(void);

/* Untouched command line saved by arch-specific code. */
char __initdata boot_command_line[COMMAND_LINE_SIZE];
/* Untouched saved command line (eg. for /proc) */
char *saved_command_line;
/* Command line for parameter parsing */
static char *static_command_line;

static char *execute_command;
static char *ramdisk_execute_command;

/*
 * If set, this is an indication to the drivers that reset the underlying
 * device before going ahead with the initialization otherwise driver might
 * rely on the BIOS and skip the reset operation.
 *
 * This is useful if kernel is booting in an unreliable environment.
 * For ex. kdump situaiton where previous kernel has crashed, BIOS has been
 * skipped and devices will be in unknown state.
 */
unsigned int reset_devices;
EXPORT_SYMBOL(reset_devices);

static int __init set_reset_devices(char *str)
{
	reset_devices = 1;
	return 1;
}

__setup("reset_devices", set_reset_devices);

static const char * argv_init[MAX_INIT_ARGS+2] = { "init", NULL, };
const char * envp_init[MAX_INIT_ENVS+2] = { "HOME=/", "TERM=linux", NULL, };
static const char *panic_later, *panic_param;

extern const struct obs_kernel_param __setup_start[], __setup_end[];

static int __init obsolete_checksetup(char *line)
{
	const struct obs_kernel_param *p;
	int had_early_param = 0;

	p = __setup_start;
	do {
		int n = strlen(p->str);
		if (parameqn(line, p->str, n)) {
			if (p->early) {
				/* Already done in parse_early_param?
				 * (Needs exact match on param part).
				 * Keep iterating, as we can have early
				 * params and __setups of same names 8( */
				if (line[n] == '\0' || line[n] == '=')
					had_early_param = 1;
			} else if (!p->setup_func) {
				pr_warn("Parameter %s is obsolete, ignored\n",
					p->str);
				return 1;
			} else if (p->setup_func(line + n))
				return 1;
		}
		p++;
	} while (p < __setup_end);

	return had_early_param;
}

/*
 * This should be approx 2 Bo*oMips to start (note initial shift), and will
 * still work even if initially too large, it will just take slightly longer
 */
unsigned long loops_per_jiffy = (1<<12);

EXPORT_SYMBOL(loops_per_jiffy);

static int __init debug_kernel(char *str)
{
	console_loglevel = 10;
	return 0;
}

static int __init quiet_kernel(char *str)
{
	console_loglevel = 4;
	return 0;
}

early_param("debug", debug_kernel);
early_param("quiet", quiet_kernel);

static int __init loglevel(char *str)
{
	int newlevel;

	/*
	 * Only update loglevel value when a correct setting was passed,
	 * to prevent blind crashes (when loglevel being set to 0) that
	 * are quite hard to debug
	 */
	if (get_option(&str, &newlevel)) {
		console_loglevel = newlevel;
		return 0;
	}

	return -EINVAL;
}

early_param("loglevel", loglevel);

/* Change NUL term back to "=", to make "param" the whole string. */
static int __init repair_env_string(char *param, char *val, const char *unused)
{
	if (val) {
		/* param=val or param="val"? */
		if (val == param+strlen(param)+1)
			val[-1] = '=';
		else if (val == param+strlen(param)+2) {
			val[-2] = '=';
			memmove(val-1, val, strlen(val)+1);
			val--;
		} else
			BUG();
	}
	return 0;
}

/*
 * Unknown boot options get handed to init, unless they look like
 * unused parameters (modprobe will find them in /proc/cmdline).
 */
static int __init unknown_bootoption(char *param, char *val, const char *unused)
{
	repair_env_string(param, val, unused);

	/* Handle obsolete-style parameters */
	if (obsolete_checksetup(param))
		return 0;

	/* Unused module parameter. */
	if (strchr(param, '.') && (!val || strchr(param, '.') < val))
		return 0;

	if (panic_later)
		return 0;

	if (val) {
		/* Environment option */
		unsigned int i;
		for (i = 0; envp_init[i]; i++) {
			if (i == MAX_INIT_ENVS) {
				panic_later = "Too many boot env vars at `%s'";
				panic_param = param;
			}
			if (!strncmp(param, envp_init[i], val - param))
				break;
		}
		envp_init[i] = param;
	} else {
		/* Command line option */
		unsigned int i;
		for (i = 0; argv_init[i]; i++) {
			if (i == MAX_INIT_ARGS) {
				panic_later = "Too many boot init vars at `%s'";
				panic_param = param;
			}
		}
		argv_init[i] = param;
	}
	return 0;
}

static int __init init_setup(char *str)
{
	unsigned int i;

	execute_command = str;
	/*
	 * In case LILO is going to boot us with default command line,
	 * it prepends "auto" before the whole cmdline which makes
	 * the shell think it should execute a script with such name.
	 * So we ignore all arguments entered _before_ init=... [MJ]
	 */
	for (i = 1; i < MAX_INIT_ARGS; i++)
		argv_init[i] = NULL;
	return 1;
}
__setup("init=", init_setup);

static int __init rdinit_setup(char *str)
{
	unsigned int i;

	ramdisk_execute_command = str;
	/* See "auto" comment in init_setup */
	for (i = 1; i < MAX_INIT_ARGS; i++)
		argv_init[i] = NULL;
	return 1;
}
__setup("rdinit=", rdinit_setup);

#ifndef CONFIG_SMP
static const unsigned int setup_max_cpus = NR_CPUS;
#ifdef CONFIG_X86_LOCAL_APIC
static void __init smp_init(void)
{
	APIC_init_uniprocessor();
}
#else
#define smp_init()	do { } while (0)
#endif

static inline void setup_nr_cpu_ids(void) { }
static inline void smp_prepare_cpus(unsigned int maxcpus) { }
#endif

/*
 * We need to store the untouched command line for future reference.
 * We also need to store the touched command line since the parameter
 * parsing is performed in place, and we should allow a component to
 * store reference of name/value for future reference.
 */
static void __init setup_command_line(char *command_line)
{
	saved_command_line = alloc_bootmem(strlen (boot_command_line)+1);
	static_command_line = alloc_bootmem(strlen (command_line)+1);
	strcpy (saved_command_line, boot_command_line);
	strcpy (static_command_line, command_line);
}

/*
 * We need to finalize in a non-__init function or else race conditions
 * between the root thread and the init thread may cause start_kernel to
 * be reaped by free_initmem before the root thread has proceeded to
 * cpu_idle.
 *
 * gcc-3.4 accidentally inlines this function, so use noinline.
 */

static __initdata DECLARE_COMPLETION(kthreadd_done);

static noinline void __init_refok rest_init(void)
{
	int pid;

	rcu_scheduler_starting();
	/*
	 * We need to spawn init first so that it obtains pid 1, however
	 * the init task will end up wanting to create kthreads, which, if
	 * we schedule it before we create kthreadd, will OOPS.
	 */
	kernel_thread(kernel_init, NULL, CLONE_FS | CLONE_SIGHAND);
	numa_default_policy();
	pid = kernel_thread(kthreadd, NULL, CLONE_FS | CLONE_FILES);
	rcu_read_lock();
	kthreadd_task = find_task_by_pid_ns(pid, &init_pid_ns);
	rcu_read_unlock();
	complete(&kthreadd_done);

	/*
	 * The boot idle thread must execute schedule()
	 * at least once to get things moving:
	 */
	init_idle_bootup_task(current);
	schedule_preempt_disabled();
	/* Call into cpu_idle with preempt disabled */
	cpu_startup_entry(CPUHP_ONLINE);
}

/* Check for early params. */
static int __init do_early_param(char *param, char *val, const char *unused)
{
	const struct obs_kernel_param *p;

	for (p = __setup_start; p < __setup_end; p++) {
		if ((p->early && parameq(param, p->str)) ||
		    (strcmp(param, "console") == 0 &&
		     strcmp(p->str, "earlycon") == 0)
		) {
			if (p->setup_func(val) != 0)
				pr_warn("Malformed early option '%s'\n", param);
		}
	}
	/* We accept everything at this stage. */
	return 0;
}

void __init parse_early_options(char *cmdline)
{
	parse_args("early options", cmdline, NULL, 0, 0, 0, do_early_param);
}

/* Arch code calls this early on, or if not, just before other parsing. */
void __init parse_early_param(void)
{
	static __initdata int done = 0;
	static __initdata char tmp_cmdline[COMMAND_LINE_SIZE];

	if (done)
		return;

	/* All fall through to do_early_param. */
	strlcpy(tmp_cmdline, boot_command_line, COMMAND_LINE_SIZE);
	parse_early_options(tmp_cmdline);
	done = 1;
}

/*
 *	Activate the first processor.
 */

static void __init boot_cpu_init(void)
{
	int cpu = smp_processor_id();
	/* Mark the boot cpu "present", "online" etc for SMP and UP case */
	set_cpu_online(cpu, true);
	set_cpu_active(cpu, true);
	set_cpu_present(cpu, true);
	set_cpu_possible(cpu, true);
}

void __init __weak smp_setup_processor_id(void)
{
}

# if THREAD_SIZE >= PAGE_SIZE
void __init __weak thread_info_cache_init(void)
{
}
#endif

/*
 * Set up kernel memory allocators
 */
static void __init mm_init(void)
{
	/*
	 * page_cgroup requires contiguous pages,
	 * bigger than MAX_ORDER unless SPARSEMEM.
	 */
	page_cgroup_init_flatmem();
	mem_init();
	kmem_cache_init();
	percpu_init_late();
	pgtable_cache_init();
	vmalloc_init();
}

asmlinkage void __init start_kernel(void)
{
	char * command_line;
	extern const struct kernel_param __start___param[], __stop___param[];

	/*
	 * Need to run as early as possible, to initialize the
	 * lockdep hash:
	 */
	lockdep_init();                         //lockdep 哈希数组头初始化
	smp_setup_processor_id();               //do nothing
	debug_objects_early_init();             //initialize the hash buckets and link the static object pool objects into the poll list.

	/*
	 * Set up the the initial canary ASAP:
	 */
	boot_init_stack_canary();               //do nothing

	cgroup_init_early();                    //初始化需要early init的cgroup子系统，不考虑模块化的cgroup子系统

	local_irq_disable();                    //关闭本地中断，因为尚未对中断代码和向量表、中断处理器进行初始化，所以必须关中断
	early_boot_irqs_disabled = true;

/*
 * Interrupts are still disabled. Do necessary setups, then
 * enable them
 */
	boot_cpu_init();                        //Mark the boot cpu "present", "online", "active", "possible"
	page_address_init();                    //初始化page address哈希数组，该数组重要用于查询物理页面映射到的虚拟地址
	pr_notice("%s", linux_banner);
	setup_arch(&command_line);              //架构相关初始化
	mm_init_owner(&init_mm, &init_task);    //绑定init任务描述符和init内存描述符
	mm_init_cpumask(&init_mm);
	setup_command_line(command_line);       //备份command_line
	setup_nr_cpu_ids();
	setup_per_cpu_areas();                  //建立per-cpu数据区域
	smp_prepare_boot_cpu();	/* arch-specific boot-cpu hooks */      //mips -- preload SMP state for boot cpu

	build_all_zonelists(NULL, NULL);        //建立内存域备用列表，为页分配器做准备
	page_alloc_init();                      //在CPU事件通知链中注册page_alloc_cpu_notify()回调函数，CPU被热插拔时处理与该CPU相关的页和vm_stat值

	pr_notice("Kernel command line: %s\n", boot_command_line);
	parse_early_param();                    //2K1000B在setup_arch()中已经调用过该函数了
	parse_args("Booting kernel", static_command_line, __start___param,
		   __stop___param - __start___param,
		   -1, -1, &unknown_bootoption);

	jump_label_init();                      //do nothing

	/*
	 * These use large bootmem allocations and must precede
	 * kmem_cache_init()
	 */
	setup_log_buf(0);                       //使用memblock分配一个启动信息的缓冲区
	pidhash_init();                         //初始化PID hash表，以便于从进程的PID获得对应的进程描述指针
	vfs_caches_init_early();                //虚拟文件系统的早期初始化，主要负责dentry和inode的hashtable的初始化工作。
	sort_main_extable();                    //对内核异常向量表进行排序
	trap_init();                            //异常向量初始化
	mm_init();                              //初始化内核内存分配器，过度到伙伴系统，启动slab机制，初始化非连续内存区

	/*
	 * Set up the scheduler prior starting any interrupts (such as the
	 * timer interrupt). Full topology setup happens at smp_init()
	 * time - but meanwhile we still have a functioning scheduler.
	 */
	sched_init();                           //核心进程调度器初始化并创建运行队列， 调度器的初始化的优先级要高于任何中断的建立,
                                            //并且初始化进程0，即idle进程，但是并没有设置idle进程的NEED_RESCHED标志，  
                                            //所以还会继续完成内核初始化剩下的事情。  
                                            //这里仅仅为进程调度程序的执行做准备。
	/*
	 * Disable preemption - early bootup scheduling is extremely
	 * fragile until we cpu_idle() for the first time.
	 */
	preempt_disable();
	if (WARN(!irqs_disabled(), "Interrupts were enabled *very* early, fixing it\n"))
		local_irq_disable();
	idr_init_cache();                       //IDR slab缓存建立。IDR机制在Linux内核中指的是整数ID管理机制。实质上来讲，这就是一种将一个整数ID号和一个指针关联在一起的机制。
	perf_event_init();                      //初始化perf机制。
	rcu_init();                             //初始化RCU机制
	tick_nohz_init();                       //cpu处在idle状态时关闭tick中断的优化初始化
	radix_tree_init();                      //radix树的初始化，供页面查找
	/* init some links before init_ISA_irqs() */
	early_irq_init();                       //中断初始化，初始化irq_desc数组
	init_IRQ();                             //中断初始化，调用2K1000B平台的中断初始化函数，初始化中断控制器及相关数据结构
	tick_init();                            //时钟滴答的初始化
	init_timers();                          //初始化本CPU上的软件时钟相关的数据结构，注册时钟软中断TIMER_SOFTIRQ
	hrtimers_init();                        //高分辨率定时器框架初始化
	softirq_init();                         //软中断初始化，注册tasklet和HI_SOFTIRQ类软中断处理函数
	timekeeping_init();                     //初始化时钟源和需要和时钟代码共同管理的时间相关值（timerkeeper结构）
	time_init();                            //平台相关时钟初始化，2K1000B高精度时钟初始化
	profile_init();                         //对内核的profile（一个内核性能调式工具）功能进行初始化，分配好存放profile信息的内存
	call_function_init();                   //初始化SMP下跨cpu的函数传递功能
	WARN(!irqs_disabled(), "Interrupts were enabled early\n");
	early_boot_irqs_disabled = false;
	local_irq_enable();                     //打开本地中断

	kmem_cache_init_late();                 //完成slab分配器初始化的剩余工作

	/*
	 * HACK ALERT! This is early. We're enabling the console before
	 * we've done PCI setups etc, and console_init() must be aware of
	 * this. But we do want output early, in case something goes wrong.
	 */
	console_init();                         //初始化控制台设备
	if (panic_later)
		panic(panic_later, panic_param);

	lockdep_info();                         //打印lockdep调试模块信息

	/*
	 * Need to run this when irqs are enabled, because it wants
	 * to self-test [hard/soft]-irqs on/off lock inversion bugs
	 * too:
	 */
	locking_selftest();                     //锁测试，2K1000B do nothing

#ifdef CONFIG_BLK_DEV_INITRD                //检查initrd的位置是否符合要求，2K1000B do nothing
	if (initrd_start && !initrd_below_start_ok &&
	    page_to_pfn(virt_to_page((void *)initrd_start)) < min_low_pfn) {
		pr_crit("initrd overwritten (0x%08lx < 0x%08lx) - disabling it.\n",
		    page_to_pfn(virt_to_page((void *)initrd_start)),
		    min_low_pfn);
		initrd_start = 0;
	}
#endif
	page_cgroup_init();                     //page cgroup结构的分配与初始化。page cgroup机制用于管理物理页被cgroup内存子系统使用的情况。
	debug_objects_mem_init();               //对象debug机制的相关数据结构的缓存建立与自检
	kmemleak_init();                        //内存泄露检测机制的初始化
	setup_per_cpu_pageset();                //建立单页页分配的per-cpu缓存
	numa_policy_init();                     //do nothing
	if (late_time_init)                     //do nothing
		late_time_init();
	sched_clock_init();                     //为每个CPU初始化调度时钟数据结构
	calibrate_delay();                      //用于计算BogoMIPS，表征1jiffy内消耗多少cpu
	pidmap_init();                          //进程PID位图分配映射初始化
	anon_vma_init();                        //创建anon_vma的slab缓存，用于建立匿名页的反向映射机制，从而快速的定位指向同一页框的所有页表项
#ifdef CONFIG_X86
	if (efi_enabled(EFI_RUNTIME_SERVICES))
		efi_enter_virtual_mode();
#endif
#ifdef CONFIG_X86_ESPFIX64
	/* Should be run before the first non-init thread is created */
	init_espfix_bsp();
#endif
	thread_info_cache_init();               //创建进程thread_info的slab高速缓存
	cred_init();                            //创建credential的slab高速缓存
	fork_init(totalram_pages);              //创建任务描述符的slab高速缓存，根据物理页数量计算线程限制
	proc_caches_init();                     //创建进程所需的各种结构体的slab缓存
	buffer_init();                          //创建buffer_head结构体的slab高速缓存
	key_init();                             //初始化密钥管理系统
	security_init();                        //初始化内核安全框架，主要是访问控制
	dbg_late_init();                        //内核调试系统后期初始化，kgdb初始化
	vfs_caches_init(totalram_pages);        //创建虚拟文件系统数据结构的slab高速缓存并初始化，包括：dentry, inode, file, bdev, chrdev等
	signals_init();                         //创建sigqueue结构高速缓存
	/* rootfs populating might need page-writeback */
	page_writeback_init();                  //页回写机制初始化
#ifdef CONFIG_PROC_FS
	proc_root_init();                       //注册并初始化proc文件系统，主要提供内核与用户进行交互的平台，方便用户实时查看进程的信息。
#endif
	cgroup_init();                          //注册未初始化的cgroup子系统
	cpuset_init();                          //注册cpuset文件系统，主要为控制组提供CPU和内存节点的管理的结构。
	taskstats_init_early();                 //初始化任务统计信息接口，创建任务状态信息结构体的高速缓存，并初始化互斥机制。
	delayacct_init();                       //初始化任务延迟机制，初始化每个任务延时计数。当一个任务等待CPU或者IO同步的时候，都需要计算等待时间。

	check_bugs();                           //检查CPU配置、FPU等是否非法使用不具备的功能，检查CPU BUG，软件规避BUG。，检查写缓冲一致性，daddi指令检测

	acpi_early_init(); /* before LAPIC and SMP init */  //电源相关的初始化
	sfi_init_late();                        //simple firmware interface 

	if (efi_enabled(EFI_RUNTIME_SERVICES)) {
		efi_late_init();
		efi_free_boot_services();
	}

	ftrace_init();                          //ftrace机制初始化

	/* Do the rest non-__init'ed, we're now alive */
	rest_init();                            //执行剩余的初始化任务
}

/* Call all constructor functions linked into the kernel. */
static void __init do_ctors(void)
{
#ifdef CONFIG_CONSTRUCTORS
	ctor_fn_t *fn = (ctor_fn_t *) __ctors_start;

	for (; fn < (ctor_fn_t *) __ctors_end; fn++)
		(*fn)();
#endif
}

bool initcall_debug;
core_param(initcall_debug, initcall_debug, bool, 0644);

static char msgbuf[64];

static int __init_or_module do_one_initcall_debug(initcall_t fn)
{
	ktime_t calltime, delta, rettime;
	unsigned long long duration;
	int ret;

	pr_debug("calling  %pF @ %i\n", fn, task_pid_nr(current));
	calltime = ktime_get();
	ret = fn();
	rettime = ktime_get();
	delta = ktime_sub(rettime, calltime);
	duration = (unsigned long long) ktime_to_ns(delta) >> 10;
	pr_debug("initcall %pF returned %d after %lld usecs\n",
		 fn, ret, duration);

	return ret;
}

int __init_or_module do_one_initcall(initcall_t fn)
{
	int count = preempt_count();
	int ret;

	if (initcall_debug)
		ret = do_one_initcall_debug(fn);
	else
		ret = fn();

	msgbuf[0] = 0;

	if (preempt_count() != count) {
		sprintf(msgbuf, "preemption imbalance ");
		preempt_count() = count;
	}
	if (irqs_disabled()) {
		strlcat(msgbuf, "disabled interrupts ", sizeof(msgbuf));
		local_irq_enable();
	}
	WARN(msgbuf[0], "initcall %pF returned with %s\n", fn, msgbuf);

	return ret;
}


extern initcall_t __initcall_start[];
extern initcall_t __initcall0_start[];
extern initcall_t __initcall1_start[];
extern initcall_t __initcall2_start[];
extern initcall_t __initcall3_start[];
extern initcall_t __initcall4_start[];
extern initcall_t __initcall5_start[];
extern initcall_t __initcall6_start[];
extern initcall_t __initcall7_start[];
extern initcall_t __initcall_end[];

static initcall_t *initcall_levels[] __initdata = {
	__initcall0_start,
	__initcall1_start,
	__initcall2_start,
	__initcall3_start,
	__initcall4_start,
	__initcall5_start,
	__initcall6_start,
	__initcall7_start,
	__initcall_end,
};

/* Keep these in sync with initcalls in include/linux/init.h */
static char *initcall_level_names[] __initdata = {
	"early",
	"core",
	"postcore",
	"arch",
	"subsys",
	"fs",
	"device",
	"late",
};

static void __init do_initcall_level(int level)
{
	extern const struct kernel_param __start___param[], __stop___param[];
	initcall_t *fn;

	strcpy(static_command_line, saved_command_line);
	parse_args(initcall_level_names[level],
		   static_command_line, __start___param,
		   __stop___param - __start___param,
		   level, level,
		   &repair_env_string);

	for (fn = initcall_levels[level]; fn < initcall_levels[level+1]; fn++)
		do_one_initcall(*fn);
}

static void __init do_initcalls(void)
{
	int level;

	for (level = 0; level < ARRAY_SIZE(initcall_levels) - 1; level++)
		do_initcall_level(level);
}

/*
 * Ok, the machine is now initialized. None of the devices
 * have been touched yet, but the CPU subsystem is up and
 * running, and memory and process management works.
 *
 * Now we can finally start doing some real work..
 */
static void __init do_basic_setup(void)
{
	cpuset_init_smp();              //初始化内核control group的cpuset子系统
	usermodehelper_init();          //创建khelper单线程工作队列，用于协助新建和运行用户空间程序
	shmem_init();                   //初始化共享内存
	driver_init();                  //devtmpfs文件系统注册，初始化设备模型，建立sysfs文件系统内设备、驱动、总线的基本层次关系
	init_irq_proc();                //创建/proc/irq目录, 并初始化系统中所有中断对应的子目录
	do_ctors();                     //执行内核的构造函数，2K1000B do nothing
	usermodehelper_enable();
	do_initcalls();                 //调用level 0到level 7的initcall函数
	random_int_secret_init();       //初始化随机数生成池
}

static void __init do_pre_smp_initcalls(void)
{
	initcall_t *fn;

	for (fn = __initcall_start; fn < __initcall0_start; fn++)
		do_one_initcall(*fn);
}

/*
 * This function requests modules which should be loaded by default and is
 * called twice right after initrd is mounted and right before init is
 * exec'd.  If such modules are on either initrd or rootfs, they will be
 * loaded before control is passed to userland.
 */
void __init load_default_modules(void)
{
	load_default_elevator_module();
}

static int run_init_process(const char *init_filename)
{
	argv_init[0] = init_filename;
	return do_execve(init_filename,
		(const char __user *const __user *)argv_init,
		(const char __user *const __user *)envp_init);
}

static noinline void __init kernel_init_freeable(void);

static int __ref kernel_init(void *unused)
{
	kernel_init_freeable();                 //等待kthreadd线程创建完成，注册内核驱动模块do_basic_setup()、启动默认控制台/dev/console
	/* need to finish all async __init code before freeing the memory */
	async_synchronize_full();               //等待所有异步调用执行完成
	free_initmem();                         //释放init段内存
	mark_rodata_ro();
	system_state = SYSTEM_RUNNING;
	numa_default_policy();

	flush_delayed_fput();                   //同步所有延时fput

    //接下来依次执行可用的init程序
	if (ramdisk_execute_command) {
		if (!run_init_process(ramdisk_execute_command))
			return 0;
		pr_err("Failed to execute %s\n", ramdisk_execute_command);
	}

	/*
	 * We try each of these until one succeeds.
	 *
	 * The Bourne shell can be used instead of init if we are
	 * trying to recover a really broken machine.
	 */
	if (execute_command) {
		if (!run_init_process(execute_command))
			return 0;
		pr_err("Failed to execute %s.  Attempting defaults...\n",
			execute_command);
	}
	if (!run_init_process("/sbin/init") ||
	    !run_init_process("/etc/init") ||
	    !run_init_process("/bin/init") ||
	    !run_init_process("/bin/sh"))
		return 0;

	panic("No init found.  Try passing init= option to kernel. "
	      "See Linux Documentation/init.txt for guidance.");
}

static noinline void __init kernel_init_freeable(void)
{
	/*
	 * Wait until kthreadd is all set-up.
	 */
	wait_for_completion(&kthreadd_done);

	/* Now the scheduler is fully set up and can do blocking allocations */
	gfp_allowed_mask = __GFP_BITS_MASK;

	/*
	 * init can allocate pages on any node
	 */
	set_mems_allowed(node_states[N_MEMORY]);
	/*
	 * init can run on any cpu.
	 */
	set_cpus_allowed_ptr(current, cpu_all_mask);

	cad_pid = task_pid(current);

	smp_prepare_cpus(setup_max_cpus);

	do_pre_smp_initcalls();                 //执行__initcall_start到__initcall0_start之间的初始化调用
	lockup_detector_init();                 //启动死锁检测，创建watchdog线程。2K1000B do nothing

	smp_init();                             //为从核创建idle进程并启动从核
	sched_init_smp();                       //初始化调度域和负载均衡，初始化从核的调度器

	do_basic_setup();

    //将控制台设置为init进程的0,1,2号文件描述符（标准输入、输出、错误）
    //之后所有进程都会继承这三个文件描述符
    //而这个控制台由启动参数指定
	/* Open the /dev/console on the rootfs, this should never fail */
	if (sys_open((const char __user *) "/dev/console", O_RDWR, 0) < 0)
		pr_err("Warning: unable to open an initial console.\n");

	(void) sys_dup(0);
	(void) sys_dup(0);
	/*
	 * check if there is an early userspace init.  If yes, let it do all
	 * the work
	 */

	if (!ramdisk_execute_command)
		ramdisk_execute_command = "/init";

	if (sys_access((const char __user *) ramdisk_execute_command, 0) != 0) {
		ramdisk_execute_command = NULL;
		prepare_namespace();                //挂载根文件系统
	}

	/*
	 * Ok, we have completed the initial bootup, and
	 * we're essentially up and running. Get rid of the
	 * initmem segments and start the user-mode stuff..
	 */

	/* rootfs is available now, try loading default modules */
	load_default_modules();                 //加载电梯io调度器
}
