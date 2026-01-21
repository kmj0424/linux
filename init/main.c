// SPDX-License-Identifier: GPL-2.0-only
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
#include <linux/export.h>
#include <linux/extable.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/binfmts.h>
#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/stackprotector.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/initrd.h>
#include <linux/memblock.h>
#include <linux/acpi.h>
#include <linux/bootconfig.h>
#include <linux/console.h>
#include <linux/nmi.h>
#include <linux/percpu.h>
#include <linux/kmod.h>
#include <linux/kprobes.h>
#include <linux/kmsan.h>
#include <linux/vmalloc.h>
#include <linux/kernel_stat.h>
#include <linux/start_kernel.h>
#include <linux/security.h>
#include <linux/smp.h>
#include <linux/profile.h>
#include <linux/kfence.h>
#include <linux/rcupdate.h>
#include <linux/srcu.h>
#include <linux/moduleparam.h>
#include <linux/kallsyms.h>
#include <linux/buildid.h>
#include <linux/writeback.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/memcontrol.h>
#include <linux/cgroup.h>
#include <linux/tick.h>
#include <linux/sched/isolation.h>
#include <linux/interrupt.h>
#include <linux/taskstats_kern.h>
#include <linux/delayacct.h>
#include <linux/unistd.h>
#include <linux/utsname.h>
#include <linux/rmap.h>
#include <linux/mempolicy.h>
#include <linux/key.h>
#include <linux/debug_locks.h>
#include <linux/debugobjects.h>
#include <linux/lockdep.h>
#include <linux/kmemleak.h>
#include <linux/padata.h>
#include <linux/pid_namespace.h>
#include <linux/device/driver.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/sched/init.h>
#include <linux/signal.h>
#include <linux/idr.h>
#include <linux/kgdb.h>
#include <linux/ftrace.h>
#include <linux/async.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/perf_event.h>
#include <linux/ptrace.h>
#include <linux/pti.h>
#include <linux/blkdev.h>
#include <linux/sched/clock.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/context_tracking.h>
#include <linux/random.h>
#include <linux/moduleloader.h>
#include <linux/list.h>
#include <linux/integrity.h>
#include <linux/proc_ns.h>
#include <linux/io.h>
#include <linux/cache.h>
#include <linux/rodata_test.h>
#include <linux/jump_label.h>
#include <linux/kcsan.h>
#include <linux/init_syscalls.h>
#include <linux/stackdepot.h>
#include <linux/randomize_kstack.h>
#include <linux/pidfs.h>
#include <linux/ptdump.h>
#include <linux/time_namespace.h>
#include <net/net_namespace.h>

#include <asm/io.h>
#include <asm/setup.h>
#include <asm/sections.h>
#include <asm/cacheflush.h>

#define CREATE_TRACE_POINTS
#include <trace/events/initcall.h>

#include <kunit/test.h>

static int kernel_init(void *);

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

/* Default late time init is NULL. archs can override this later. */
void (*__initdata late_time_init)(void);

/* Untouched command line saved by arch-specific code. */
char __initdata boot_command_line[COMMAND_LINE_SIZE];
/* Untouched saved command line (eg. for /proc) */
char *saved_command_line __ro_after_init;
unsigned int saved_command_line_len __ro_after_init;
/* Command line for parameter parsing */
static char *static_command_line;
/* Untouched extra command line */
static char *extra_command_line;
/* Extra init arguments */
static char *extra_init_args;

#ifdef CONFIG_BOOT_CONFIG
/* Is bootconfig on command line? */
static bool bootconfig_found;
static size_t initargs_offs;
#else
# define bootconfig_found false
# define initargs_offs 0
#endif

static char *execute_command;
static char *ramdisk_execute_command = "/init";

/*
 * Used to generate warnings if static_key manipulation functions are used
 * before jump_label_init is called.
 */
bool static_key_initialized __read_mostly;
EXPORT_SYMBOL_GPL(static_key_initialized);

/*
 * If set, this is an indication to the drivers that reset the underlying
 * device before going ahead with the initialization otherwise driver might
 * rely on the BIOS and skip the reset operation.
 *
 * This is useful if kernel is booting in an unreliable environment.
 * For ex. kdump situation where previous kernel has crashed, BIOS has been
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

static const char *argv_init[MAX_INIT_ARGS+2] = { "init", NULL, };
const char *envp_init[MAX_INIT_ENVS+2] = { "HOME=/", "TERM=linux", NULL, };
static const char *panic_later, *panic_param;

static bool __init obsolete_checksetup(char *line)
{
	const struct obs_kernel_param *p;
	bool had_early_param = false;

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
					had_early_param = true;
			} else if (!p->setup_func) {
				pr_warn("Parameter %s is obsolete, ignored\n",
					p->str);
				return true;
			} else if (p->setup_func(line + n))
				return true;
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
	console_loglevel = CONSOLE_LOGLEVEL_DEBUG;
	return 0;
}

static int __init quiet_kernel(char *str)
{
	console_loglevel = CONSOLE_LOGLEVEL_QUIET;
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

#ifdef CONFIG_BLK_DEV_INITRD
static void * __init get_boot_config_from_initrd(size_t *_size)
{
	u32 size, csum;
	char *data;
	u32 *hdr;
	int i;

	if (!initrd_end)
		return NULL;

	data = (char *)initrd_end - BOOTCONFIG_MAGIC_LEN;
	/*
	 * Since Grub may align the size of initrd to 4, we must
	 * check the preceding 3 bytes as well.
	 */
	for (i = 0; i < 4; i++) {
		if (!memcmp(data, BOOTCONFIG_MAGIC, BOOTCONFIG_MAGIC_LEN))
			goto found;
		data--;
	}
	return NULL;

found:
	hdr = (u32 *)(data - 8);
	size = le32_to_cpu(hdr[0]);
	csum = le32_to_cpu(hdr[1]);

	data = ((void *)hdr) - size;
	if ((unsigned long)data < initrd_start) {
		pr_err("bootconfig size %d is greater than initrd size %ld\n",
			size, initrd_end - initrd_start);
		return NULL;
	}

	if (xbc_calc_checksum(data, size) != csum) {
		pr_err("bootconfig checksum failed\n");
		return NULL;
	}

	/* Remove bootconfig from initramfs/initrd */
	initrd_end = (unsigned long)data;
	if (_size)
		*_size = size;

	return data;
}
#else
static void * __init get_boot_config_from_initrd(size_t *_size)
{
	return NULL;
}
#endif

#ifdef CONFIG_BOOT_CONFIG

static char xbc_namebuf[XBC_KEYLEN_MAX] __initdata;

#define rest(dst, end) ((end) > (dst) ? (end) - (dst) : 0)

static int __init xbc_snprint_cmdline(char *buf, size_t size,
				      struct xbc_node *root)
{
	struct xbc_node *knode, *vnode;
	char *end = buf + size;
	const char *val, *q;
	int ret;

	xbc_node_for_each_key_value(root, knode, val) {
		ret = xbc_node_compose_key_after(root, knode,
					xbc_namebuf, XBC_KEYLEN_MAX);
		if (ret < 0)
			return ret;

		vnode = xbc_node_get_child(knode);
		if (!vnode) {
			ret = snprintf(buf, rest(buf, end), "%s ", xbc_namebuf);
			if (ret < 0)
				return ret;
			buf += ret;
			continue;
		}
		xbc_array_for_each_value(vnode, val) {
			/*
			 * For prettier and more readable /proc/cmdline, only
			 * quote the value when necessary, i.e. when it contains
			 * whitespace.
			 */
			q = strpbrk(val, " \t\r\n") ? "\"" : "";
			ret = snprintf(buf, rest(buf, end), "%s=%s%s%s ",
				       xbc_namebuf, q, val, q);
			if (ret < 0)
				return ret;
			buf += ret;
		}
	}

	return buf - (end - size);
}
#undef rest

/* Make an extra command line under given key word */
static char * __init xbc_make_cmdline(const char *key)
{
	struct xbc_node *root;
	char *new_cmdline;
	int ret, len = 0;

	root = xbc_find_node(key);
	if (!root)
		return NULL;

	/* Count required buffer size */
	len = xbc_snprint_cmdline(NULL, 0, root);
	if (len <= 0)
		return NULL;

	new_cmdline = memblock_alloc(len + 1, SMP_CACHE_BYTES);
	if (!new_cmdline) {
		pr_err("Failed to allocate memory for extra kernel cmdline.\n");
		return NULL;
	}

	ret = xbc_snprint_cmdline(new_cmdline, len + 1, root);
	if (ret < 0 || ret > len) {
		pr_err("Failed to print extra kernel cmdline.\n");
		memblock_free(new_cmdline, len + 1);
		return NULL;
	}

	return new_cmdline;
}

static int __init bootconfig_params(char *param, char *val,
				    const char *unused, void *arg)
{
	if (strcmp(param, "bootconfig") == 0) {
		bootconfig_found = true;
	}
	return 0;
}

static int __init warn_bootconfig(char *str)
{
	/* The 'bootconfig' has been handled by bootconfig_params(). */
	return 0;
}

static void __init setup_boot_config(void)
{
	static char tmp_cmdline[COMMAND_LINE_SIZE] __initdata;
	const char *msg, *data;
	int pos, ret;
	size_t size;
	char *err;

	/* Cut out the bootconfig data even if we have no bootconfig option */
	data = get_boot_config_from_initrd(&size);
	/* If there is no bootconfig in initrd, try embedded one. */
	if (!data)
		data = xbc_get_embedded_bootconfig(&size);

	strscpy(tmp_cmdline, boot_command_line, COMMAND_LINE_SIZE);
	err = parse_args("bootconfig", tmp_cmdline, NULL, 0, 0, 0, NULL,
			 bootconfig_params);

	if (IS_ERR(err) || !(bootconfig_found || IS_ENABLED(CONFIG_BOOT_CONFIG_FORCE)))
		return;

	/* parse_args() stops at the next param of '--' and returns an address */
	if (err)
		initargs_offs = err - tmp_cmdline;

	if (!data) {
		/* If user intended to use bootconfig, show an error level message */
		if (bootconfig_found)
			pr_err("'bootconfig' found on command line, but no bootconfig found\n");
		else
			pr_info("No bootconfig data provided, so skipping bootconfig");
		return;
	}

	if (size >= XBC_DATA_MAX) {
		pr_err("bootconfig size %ld greater than max size %d\n",
			(long)size, XBC_DATA_MAX);
		return;
	}

	ret = xbc_init(data, size, &msg, &pos);
	if (ret < 0) {
		if (pos < 0)
			pr_err("Failed to init bootconfig: %s.\n", msg);
		else
			pr_err("Failed to parse bootconfig: %s at %d.\n",
				msg, pos);
	} else {
		xbc_get_info(&ret, NULL);
		pr_info("Load bootconfig: %ld bytes %d nodes\n", (long)size, ret);
		/* keys starting with "kernel." are passed via cmdline */
		extra_command_line = xbc_make_cmdline("kernel");
		/* Also, "init." keys are init arguments */
		extra_init_args = xbc_make_cmdline("init");
	}
	return;
}

static void __init exit_boot_config(void)
{
	xbc_exit();
}

#else	/* !CONFIG_BOOT_CONFIG */

static void __init setup_boot_config(void)
{
	/* Remove bootconfig data from initrd */
	get_boot_config_from_initrd(NULL);
}

static int __init warn_bootconfig(char *str)
{
	pr_warn("WARNING: 'bootconfig' found on the kernel command line but CONFIG_BOOT_CONFIG is not set.\n");
	return 0;
}

#define exit_boot_config()	do {} while (0)

#endif	/* CONFIG_BOOT_CONFIG */

early_param("bootconfig", warn_bootconfig);

bool __init cmdline_has_extra_options(void)
{
	return extra_command_line || extra_init_args;
}

/* Change NUL term back to "=", to make "param" the whole string. */
static void __init repair_env_string(char *param, char *val)
{
	if (val) {
		/* param=val or param="val"? */
		if (val == param+strlen(param)+1)
			val[-1] = '=';
		else if (val == param+strlen(param)+2) {
			val[-2] = '=';
			memmove(val-1, val, strlen(val)+1);
		} else
			BUG();
	}
}

/* Anything after -- gets handed straight to init. */
static int __init set_init_arg(char *param, char *val,
			       const char *unused, void *arg)
{
	unsigned int i;

	if (panic_later)
		return 0;

	repair_env_string(param, val);

	for (i = 0; argv_init[i]; i++) {
		if (i == MAX_INIT_ARGS) {
			panic_later = "init";
			panic_param = param;
			return 0;
		}
	}
	argv_init[i] = param;
	return 0;
}

/*
 * Unknown boot options get handed to init, unless they look like
 * unused parameters (modprobe will find them in /proc/cmdline).
 */
static int __init unknown_bootoption(char *param, char *val,
				     const char *unused, void *arg)
{
	size_t len = strlen(param);
	/*
	 * Well-known bootloader identifiers:
	 * 1. LILO/Grub pass "BOOT_IMAGE=...";
	 * 2. kexec/kdump (kexec-tools) pass "kexec".
	 */
	const char *bootloader[] = { "BOOT_IMAGE=", "kexec", NULL };

	/* Handle params aliased to sysctls */
	if (sysctl_is_alias(param))
		return 0;

	repair_env_string(param, val);

	/* Handle bootloader identifier */
	for (int i = 0; bootloader[i]; i++) {
		if (strstarts(param, bootloader[i]))
			return 0;
	}

	/* Handle obsolete-style parameters */
	if (obsolete_checksetup(param))
		return 0;

	/* Unused module parameter. */
	if (strnchr(param, len, '.'))
		return 0;

	if (panic_later)
		return 0;

	if (val) {
		/* Environment option */
		unsigned int i;
		for (i = 0; envp_init[i]; i++) {
			if (i == MAX_INIT_ENVS) {
				panic_later = "env";
				panic_param = param;
			}
			if (!strncmp(param, envp_init[i], len+1))
				break;
		}
		envp_init[i] = param;
	} else {
		/* Command line option */
		unsigned int i;
		for (i = 0; argv_init[i]; i++) {
			if (i == MAX_INIT_ARGS) {
				panic_later = "init";
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
	size_t len, xlen = 0, ilen = 0;

	if (extra_command_line)
		xlen = strlen(extra_command_line);
	if (extra_init_args) {
		extra_init_args = strim(extra_init_args); /* remove trailing space */
		ilen = strlen(extra_init_args) + 4; /* for " -- " */
	}

	len = xlen + strlen(boot_command_line) + ilen + 1;

	saved_command_line = memblock_alloc_or_panic(len, SMP_CACHE_BYTES);

	len = xlen + strlen(command_line) + 1;

	static_command_line = memblock_alloc_or_panic(len, SMP_CACHE_BYTES);

	if (xlen) {
		/*
		 * We have to put extra_command_line before boot command
		 * lines because there could be dashes (separator of init
		 * command line) in the command lines.
		 */
		strcpy(saved_command_line, extra_command_line);
		strcpy(static_command_line, extra_command_line);
	}
	strcpy(saved_command_line + xlen, boot_command_line);
	strcpy(static_command_line + xlen, command_line);

	if (ilen) {
		/*
		 * Append supplemental init boot args to saved_command_line
		 * so that user can check what command line options passed
		 * to init.
		 * The order should always be
		 * " -- "[bootconfig init-param][cmdline init-param]
		 */
		if (initargs_offs) {
			len = xlen + initargs_offs;
			strcpy(saved_command_line + len, extra_init_args);
			len += ilen - 4;	/* strlen(extra_init_args) */
			strcpy(saved_command_line + len,
				boot_command_line + initargs_offs - 1);
		} else {
			len = strlen(saved_command_line);
			strcpy(saved_command_line + len, " -- ");
			len += 4;
			strcpy(saved_command_line + len, extra_init_args);
		}
	}

	saved_command_line_len = strlen(saved_command_line);
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

static noinline void __ref __noreturn rest_init(void)
{
	struct task_struct *tsk;
	int pid;

	rcu_scheduler_starting();
	/*
	 * We need to spawn init first so that it obtains pid 1, however
	 * the init task will end up wanting to create kthreads, which, if
	 * we schedule it before we create kthreadd, will OOPS.
	 */
	pid = user_mode_thread(kernel_init, NULL, CLONE_FS);
	/*
	 * Pin init on the boot CPU. Task migration is not properly working
	 * until sched_init_smp() has been run. It will set the allowed
	 * CPUs for init to the non isolated CPUs.
	 */
	rcu_read_lock();
	tsk = find_task_by_pid_ns(pid, &init_pid_ns);
	tsk->flags |= PF_NO_SETAFFINITY;
	set_cpus_allowed_ptr(tsk, cpumask_of(smp_processor_id()));
	rcu_read_unlock();

	numa_default_policy();
	pid = kernel_thread(kthreadd, NULL, NULL, CLONE_FS | CLONE_FILES);
	rcu_read_lock();
	kthreadd_task = find_task_by_pid_ns(pid, &init_pid_ns);
	rcu_read_unlock();

	/*
	 * Enable might_sleep() and smp_processor_id() checks.
	 * They cannot be enabled earlier because with CONFIG_PREEMPTION=y
	 * kernel_thread() would trigger might_sleep() splats. With
	 * CONFIG_PREEMPT_VOLUNTARY=y the init task might have scheduled
	 * already, but it's stuck on the kthreadd_done completion.
	 */
	system_state = SYSTEM_SCHEDULING;

	complete(&kthreadd_done);

	/*
	 * The boot idle thread must execute schedule()
	 * at least once to get things moving:
	 */
	schedule_preempt_disabled();
	/* Call into cpu_idle with preempt disabled */
	cpu_startup_entry(CPUHP_ONLINE);
}

/* Check for early params. */
static int __init do_early_param(char *param, char *val,
				 const char *unused, void *arg)
{
	const struct obs_kernel_param *p;

	for (p = __setup_start; p < __setup_end; p++) {
		if (p->early && parameq(param, p->str)) {
			if (p->setup_func(val) != 0)
				pr_warn("Malformed early option '%s'\n", param);
		}
	}
	/* We accept everything at this stage. */
	return 0;
}

void __init parse_early_options(char *cmdline)
{
	parse_args("early options", cmdline, NULL, 0, 0, 0, NULL,
		   do_early_param);
}

/* Arch code calls this early on, or if not, just before other parsing. */
void __init parse_early_param(void)
{
	static int done __initdata;
	static char tmp_cmdline[COMMAND_LINE_SIZE] __initdata;

	if (done)
		return;

	/* All fall through to do_early_param. */
	strscpy(tmp_cmdline, boot_command_line, COMMAND_LINE_SIZE);
	parse_early_options(tmp_cmdline);
	done = 1;
}

void __init __weak arch_post_acpi_subsys_init(void) { }

void __init __weak smp_setup_processor_id(void)
{
}

void __init __weak smp_prepare_boot_cpu(void)
{
}

# if THREAD_SIZE >= PAGE_SIZE
void __init __weak thread_stack_cache_init(void)
{
}
#endif

void __init __weak poking_init(void) { }

void __init __weak pgtable_cache_init(void) { }

void __init __weak trap_init(void) { }

bool initcall_debug;
core_param(initcall_debug, initcall_debug, bool, 0644);

#ifdef TRACEPOINTS_ENABLED
static void __init initcall_debug_enable(void);
#else
static inline void initcall_debug_enable(void)
{
}
#endif

#ifdef CONFIG_RANDOMIZE_KSTACK_OFFSET
DEFINE_STATIC_KEY_MAYBE_RO(CONFIG_RANDOMIZE_KSTACK_OFFSET_DEFAULT,
			   randomize_kstack_offset);
DEFINE_PER_CPU(u32, kstack_offset);

static int __init early_randomize_kstack_offset(char *buf)
{
	int ret;
	bool bool_result;

	ret = kstrtobool(buf, &bool_result);
	if (ret)
		return ret;

	if (bool_result)
		static_branch_enable(&randomize_kstack_offset);
	else
		static_branch_disable(&randomize_kstack_offset);
	return 0;
}
early_param("randomize_kstack_offset", early_randomize_kstack_offset);
#endif

static void __init print_unknown_bootoptions(void)
{
	char *unknown_options;
	char *end;
	const char *const *p;
	size_t len;

	if (panic_later || (!argv_init[1] && !envp_init[2]))
		return;

	/*
	 * Determine how many options we have to print out, plus a space
	 * before each
	 */
	len = 1; /* null terminator */
	for (p = &argv_init[1]; *p; p++) {
		len++;
		len += strlen(*p);
	}
	for (p = &envp_init[2]; *p; p++) {
		len++;
		len += strlen(*p);
	}

	unknown_options = memblock_alloc(len, SMP_CACHE_BYTES);
	if (!unknown_options) {
		pr_err("%s: Failed to allocate %zu bytes\n",
			__func__, len);
		return;
	}
	end = unknown_options;

	for (p = &argv_init[1]; *p; p++)
		end += sprintf(end, " %s", *p);
	for (p = &envp_init[2]; *p; p++)
		end += sprintf(end, " %s", *p);

	/* Start at unknown_options[1] to skip the initial space */
	pr_notice("Unknown kernel command line parameters \"%s\", will be passed to user space.\n",
		&unknown_options[1]);
	memblock_free(unknown_options, len);
}

static void __init early_numa_node_init(void)
{
#ifdef CONFIG_USE_PERCPU_NUMA_NODE_ID
#ifndef cpu_to_node
	int cpu;

	/* The early_cpu_to_node() should be ready here. */
	for_each_possible_cpu(cpu)
		set_cpu_numa_node(cpu, early_cpu_to_node(cpu));
#endif
#endif
}

asmlinkage __visible __init __no_sanitize_address __noreturn __no_stack_protector
void start_kernel(void) //시작
	/*
	아키텍처별로 asm 초기화를 치고 들어옴
	linux asm? 공격 표면 관리??
	최소한의 메모리 매핑/스택이 준비됨
	전역 데이터가 초기화 가능한 상태
	커널 부팅의 메인 파이프라인이 시작됨
	ARM은 저전력, 고효율을 특징으로 하는 RISC 기반 CPU 설계 모델
	ISA(Instruction Set Architecture) : CPU가 인식, 해석, 실행할 수 있는 명령어 집합
	ISA가 같은 CPU끼리는 서로의 명령어를 이해할 수 있지만 ISA가 다르면 서로의 명령어를 이해하지 못한다???
	RISC : Load-Store 구조, 1클럭 내외로 실행되는 단순하고 적은 수의 고정 길이 명령어 집합
	CISC : 복잡하고 다양한 명령어 집합, 다양한 주소 지정 방식과 특별한 명령어, 파이프라이닝의 어려움? 실행 시간 불규칙성 ????
	inline : 함수 호출 오버헤드 제거 / 부트 초반 커널 코드에서 필수
	*/
{
	char *command_line;
	char *after_dashes;

	/*
	커널은 하나지만 태스크마다 독립적인 커널 스택을 가짐
	스택·힙·데이터가 한 덩어리로 구성되는 것은 사용자 프로세스 주소 공간의 개념
	커널 주소 공간은 모든 태스크가 공유하는 전역 공간
	태스크별 커널 스택은 전역 메모리 풀에서 따로 할당될 뿐 태스크의 다른 구성요소들과 물리적으로 묶여 있지 않음
	커널은 CPU의 MMU 동작 방식에 맞추기 위해 메모리를 페이지 단위로 관리하며, 커널 스택 역시 예외 없이 페이지 단위로 할당
	set_task_stack_end_magic(&init_task)가 호출되는 시점은 커널 부팅 초기에 이미 존재하는 유일한 태스크인 init_task가
	자신의 커널 스택을 사용 중인 상태에서 아직 일반적인 태스크 생성 메커니즘이 시작되기 전에
	task란 커널이 관리하는 실행 단위(execution context)이며 CPU에서 실행될 수 있는 최소 단위

	리눅스 커널에서 task는 CPU에서 실행될 수 있는 최소 실행 단위로
	프로세스와 스레드를 구분하지 않고 모두 task_struct로 관리
	task_struct는 스케줄링, 상태, 메모리, 부모-자식 관계 등 실행에 필요한 모든 정보를 담고 있음
	각 task는 커널 모드 실행을 위해 반드시 독립적인 커널 스택을 하나씩 가짐
	일반적인 task는 실행 중 copy_process를 통해 동적으로 생성
	이 과정에서 PID가 부여되고 커널 스택과 그 끝에 대한 보호(magic 값 설정)가 자동으로 이루어짐
	반면 init_task는 커널 부팅을 가능하게 하기 위해 커널 이미지에 정적으로 정의된 최초의 task(PID 0)
	일반 생성 경로를 거치지 않기 때문에 커널 스택 보호 역시 예외적으로 수동 설정
	이후 init_task는 PID 1을 생성하고 ID 1은 부모를 잃은 프로세스의 최종 부모로서 좀비 프로세스를 회수하는 역할을 수행

	함수의 목적
	set_task_stack_end_magic() 함수의 목적은 각 태스크가 커널 모드에서 사용하는 커널 스택의 끝(바닥)에 magic 값을 기록하여
	커널 스택이 할당된 범위를 침범했는지를 이후 검사 시점에 감지할 수 있도록 하는 것
	이 함수 자체가 오버플로우를 검사하는 것은 아니며, 검사를 가능하게 하는 표식(magic 값)을 설치하는 역할

	커널 메모리와 페이지(Page) 단위 관리
	리눅스 커널은 메모리를 바이트 단위가 아니라 페이지(page) 단위로 관리
	페이지는 CPU의 MMU(메모리 관리 장치)가 이해하는 최소 관리 단위로 일반적으로 4KB 크기
	커널은 페이지 단위로 메모리를 할당·해제하고, 접근 권한이나 매핑 역시 페이지 단위로 설정
	이러한 설계는 하드웨어 구조에 맞춘 것, 메모리 보호와 성능을 동시에 고려한 결과

	커널 스택과 페이지 단위 할당
	각 태스크(task)는 커널 모드에서 실행될 때 사용할 전용 커널 스택을 반드시 하나씩 가짐
	이 커널 스택은 힙처럼 가변 크기가 아니라, 고정된 크기(THREAD_SIZE)의 연속된 페이지들로 할당
	커널 스택 크기가 8KB -> 4KB 페이지 두 개가 연속된 가상 주소 공간으로 할당된 것
	커널 스택이 페이지 단위로 연속 할당되는 이유는 주소 계산을 단순하게 하고, 인터럽트나 예외 상황에서도 안전하게 접근할 수 있도록 하기 위함

	스택 성장 방향과 페이지 경계 문제
	커널 스택은 아래 방향(낮은 주소 방향)으로 성장, 함수 호출이 깊어질수록 스택 포인터는 점점 낮은 주소로 이동
	만약 스택 사용량이 커널 스택에 할당된 페이지 범위를 넘어가면, 스택은 더 낮은 주소의 다른 커널 메모리 페이지를 침범하게 됨
	이 페이지는 다른 태스크의 커널 스택일 수도 있고, task_struct나 다른 커널 자료구조가 들어 있는 페이지일 수도 있음

	커널 스택 오버플로우가 치명적인 이유
	커널 주소 공간은 모든 태스크가 공유하며, 사용자 공간처럼 강력한 보호 장치가 적용 x
	따라서 커널 스택이 페이지 경계를 넘어 다른 커널 메모리 페이지를 덮어쓰면, 단순히 해당 태스크만 문제가 되는 것이 아니라 시스템 전체의 무결성이 깨짐
	이런 손상은 즉시 오류를 발생시키지 않고, 나중에 알 수 없는 시점에서 커널 패닉이나 데이터 손상으로 나타날 수 있기 때문에 매우 위험

	guard page 방식과 커널에서의 한계
	사용자 공간에서는 스택의 끝에 접근 불가능한 페이지를 하나 두는 guard page 방식을 사용해 스택 오버플로우를 감지
	이 방식은 스택이 해당 페이지에 접근하는 순간 페이지 폴트를 발생시켜 즉시 오류를 알림
	커널 모드는 인터럽트나 예외 처리 중에는 페이지 폴트 자체가 치명적일 수 있으며, 처리 불가능한 상황으로 이어질 수 있음
	따라서 커널 스택에는 guard page 방식이 일반적으로 사용되지 않고 커널은 예외를 발생시키지 않는 방식으로 스택의 끝에 미리 magic 값을 기록해 두고 이를 비교하는 방식을 선택

	end_of_stack(task)의 의미와 계산 방식
	커널은 각 태스크의 커널 스택이 차지하는 페이지 범위를 정확히 알고 있기 때문에, 스택의 끝 주소를 계산할 수 있음
	end_of_stack(task)는 해당 태스크의 커널 스택 시작 주소(task->stack)에 스택 크기(THREAD_SIZE)를 더하고 그 태스크가 사용할 수 있는 커널 스택의 최하단 주소를 산출
	이 주소는 페이지 경계 기준으로 계산된 절대 침범해서는 안 되는 스택의 경계 지점

	magic 값(STACK_END_MAGIC)과 페이지 침범 감지
	set_task_stack_end_magic() 함수는 end_of_stack(task)로 계산된 커널 스택의 끝 위치에 STACK_END_MAGIC이라는 고정된 값을 기록
	이 값은 정상적인 실행 중에는 절대로 변경되지 않아야 함
	만약 커널 스택이 할당된 페이지 범위를 넘어 다른 페이지로 침범하게 되면
	이 magic 값이 덮어써지게 되고, 커널은 이후 검사 과정에서 이를 감지해 스택 오버플로우 발생 사실을 알 수 있음

// TODO : init_task 특별대우?
	init_task는 커널 부팅 시 이미 존재하는 최초의 태스크(PID 0)
	커널은 항상 실행 중인 태스크(current)가 있다고 가정하므로, init_task는 실행의 출발점 역할
	init_task는 커널 스택을 제공해 부팅 초기 함수 호출과 인터럽트 처리를 가능하게 함
	모든 사용자 프로세스(PID 1 포함)는 init_task로부터 파생
	PID 1은 고아 프로세스 회수
	정적 태스크이기 때문에 일반 태스크 생성 경로를 타지 않아 수동 초기화 등 특별대우가 필요

	init_task와 예외적인 수동 호출
	init_task는 커널 부팅 시점에 이미 정적으로 존재하는 최초의 태스크(PID 0)로 일반적인 태스크 생성 경로를 거치지 않음
	일반 태스크들은 생성 과정에서 커널 스택이 페이지 단위로 할당될 때 자동으로 set_task_stack_end_magic()가 호출지만 init_task는 이러한 자동 경로를 타지 않음
	그래서 커널 부팅 초기에 예외적으로 set_task_stack_end_magic(&init_task)를 호출해 커널 스택 끝 페이지에 magic 값을 설정한다.
	*/
	set_task_stack_end_magic(&init_task); //init/init_task.c kernel/fork.c
	/*
	지금 코드를 실행중인 CPU processor id 확정
	cpu processor id -> cpu siblings -> 소켓
	??SMP : 대칭형 다중 처리
	->모든 CPU 동등(커널 코드 실행 가능, 인터럽트 처리 가능, 스케줄링 대상)
	cat /proc/cpuinfo
	?? ALU는 필수 FPU는 선택??
	물리 코어 : cpu
	하이퍼스레딩 : 하나의 물리 코어를 두 개의 논리 실행 단위처럼 보이게 만드는 기술
	-> 코어가 놀고 있는 시간(파이프라인 버블)을 다른 스레드로 채우자
	-> 연산 능력을 늘리는 기술보다 파이프라인 버블을 숨기는 기술
	계산 유닛(ALU, FPU)은 공유
	ALU : 산술 논리 장치(정수 연산, 논리연산)
	FPU : 부동 소수점 장치(실수 포함 부동소수점 연산)
	?레지스터 상태, 프로그램 카운터 같은 아키텍처 상태는 분리
	?MIPS란  밉스 테크놀리지에서 개발한 RISC 기반의 마이크로 프로세서 명령어 집합 구조
	?mips : 컴퓨터 아키텍처의 한 종류인 연동 파이프라인 스테이지가 없는 마이크로프로세서
	-Microprocessor without Interlocked Pipeline Stages의 약자
	-단순한 RISC 구조를 가진 프로세서 자체
	파이프라인 or 파이프라이닝 : 프로세서로 가는 명령어들의 움직임, 연산 병렬
	IF (Instruction Fetch) : 명령어를 메모리부터 가져온다.
	ID (Instruction Decode) : 명령어를 해독하고 동시에 레지스터를 읽는다.
	EX (Execute) : ALU를 통해서 해당 연산을 수행하거나 주소를 계산한다.
	MEM (Data Memory access) : 데이터 메모리에 있는 피연산자를 접근한다.
	WB (Write Back) : 결과값을 레지스터에 쓴다.
	?파이프라인 종류
	Super Scalar
	Super Pipeline
	Superpipelined, Superscalar
	VLIW
	해저드 : 파이프라인 오류
	구조적 해저드
	데이터 해저드
	제어 해저드
	?파이프라인 버블
	논리 코어 : OS가 인식하는 CPU 단위 스케줄러가 태스크를 올린다고 생각하는 대상
	siblings : 같은 물리 CPU 패키지(소켓) 안에 있는 논리 CPU 개수
	core id : 어떤 논리 CPU들이 같은 물리 코어를 공유하는지
	physical id(Cpu 소켓) : 메인보드에 꽂힌 CPU 패키지 번호
	?패킷
	?APIC 레지스터
	?비트마스킹 : 정수의 이진수 표현을 자료구조로 쓰는 기법
	?apic

	하드웨어 CPU ID 읽기 (MPIDR)
	부트 CPU의 logical id = 0으로 매핑
	부트 CPU의 percpu 오프셋을 0으로 확정(초기 hang 방지)

	SMP란 무엇인가
	SMP(Symmetric Multi-Processing)는 여러 개의 CPU가 동등한 권한으로 하나의 커널을 공유하며 실행되는 구조를 의미
	SMP 환경에서는 모든 CPU가 동일한 커널 코드를 실행할 수 있고, 인터럽트 처리, 시스템 콜 처리, 스케줄링에 모두 참여 가능
	CPU마다 다른 커널을 실행하는 구조가 아니라, 커널은 하나이고 실행 주체(CPU)만 여러 개인 구조
	반대로 단일 CPU(UP, Uni-Processor) 환경에서는 CPU가 하나뿐이기 때문에 현재 실행 중인 CPU가 누구인가라는 문제 자체가 발생하지 않음
	이 경우 커널은 CPU 식별, CPU 간 동기화, CPU 간 인터럽트(IPI) 같은 SMP 관련 복잡성을 대부분 제거하거나 단순화할 수 있음

	함수의 목적
	smp_setup_processor_id()의 목적은 커널 부팅 극초반에 현재 실행 중인 CPU가 누구인지를 커널 내부 기준으로 확정하는 것
	SMP 커널에서는 여러 CPU가 동시에 커널 코드를 실행할 수 있기 때문에, 커널은 반드시 지금 이 코드가 어느 CPU에서 실행되고 있는지를 알아야 함
	이를 위해 커널은 CPU를 내부적으로 logical CPU ID(0, 1, 2, …)로 관리하며, smp_setup_processor_id()는 부팅을 시작한 CPU를 logical CPU 0으로 고정하는 역할을 수행
	이 결정은 이후 스케줄링, per-CPU 데이터, CPU 마스크 초기화 전반의 기준점이 됨

	커널이 CPU를 다루는 방식 (physical CPU vs logical CPU)
	하드웨어는 각 CPU를 물리적 식별자(physical CPU ID)로 구분.
	이 식별자는 아키텍처 의존적이며, arm64에서는 MPIDR 레지스터의 affinity 필드들이 CPU의 물리적 위치와 정체를 나타냄.
	반면 리눅스 커널은 CPU를 logical CPU ID라는 추상화된 번호 체계로 관리.
	커널은 이 CPU가 소켓 몇 번, 코어 몇 번인가보다는, 이 CPU가 logical CPU 0인가, 1인가에만 관심을 가짐.
	따라서 SMP 커널이 동작하려면, physical CPU ID → logical CPU ID로의 매핑이 반드시 필요하며, smp_setup_processor_id()는 이 매핑의 첫 단계를 담당.

	단일 CPU(UP) 커널과 SMP 커널의 차이
	리눅스 커널은 설정에 따라 단일 CPU 커널로도, SMP 커널로도 빌드될 수 있음.
	단일 CPU 커널에서는 CPU가 하나뿐이므로, CPU 식별 과정이 사실상 불필요.
	SMP 커널에서는 여러 CPU가 존재할 수 있으므로, 반드시 CPU 식별과 매핑 과정이 필요.	
	이 차이를 코드 레벨에서 처리하기 위해 커널에는 is_smp()와 같은 조건 분기들이 존재.
	is_smp()는 현재 커널이 SMP 환경으로 동작 중인지 여부를 반환하며, 단일 CPU 커널이거나 SMP가 비활성화된 경우에는 false를 반환.
	이를 통해 커널은 CPU가 여러 개일 가능성이 있는 경우에만 하드웨어 CPU ID를 읽거나, SMP 전용 초기화 코드를 실행하도록 분기.

	smp_setup_processor_id() 내부의 조건 분기 의미
	SMP 환경일 경우 → 현재 CPU의 하드웨어 식별자(예: MPIDR)를 읽어와 logical CPU 0에 매핑.
	SMP가 아닌 경우(단일 CPU) → 하드웨어 CPU ID를 읽을 필요가 없으므로, CPU ID를 0으로 간주.
	이 분기는 SMP 커널이지만 실제로는 CPU가 하나뿐인 경우나, 아예 단일 CPU 커널로 빌드된 경우 모두를 포괄하기 위한 장치.
	커널은 불필요한 하드웨어 레지스터 접근을 피하고, 단일 CPU 환경에서는 CPU 0 하나만 존재한다고 가정해 초기화를 단순화.

	부트 CPU를 logical CPU 0으로 고정하는 이유
	smp_setup_processor_id()는 현재 실행 중인 CPU를 항상 logical CPU 0으로 고정.
	이는 이후 커널 초기화 코드 전반이 부트 CPU = CPU 0이라는 가정 위에서 작성되어 있기 때문.
	예를 들어 초기 스케줄링, CPU 마스크 구성, per-CPU 데이터 초기화, 디버그 코드들은 CPU 0이 이미 존재하고 실행 중이라는 전제를 사용.
	이 함수는 이 전제를 코드 차원에서 확정짓는 역할을 하며, 이후 다른 CPU들은 secondary CPU로 취급되어 별도의 초기화 경로를 탐.

// TODO : cpu_logical_map(0) = cpu; set_my_cpu_offset(0); -> 용도? 왜 여기서? 특별대우?
	per-CPU 변수와 초기 오프셋 설정
	per-CPU 변수는 CPU마다 독립적인 값을 가지는 커널 데이터.
	내부적으로는 per-CPU 베이스 주소 + CPU별 오프셋 방식으로 접근.
	커널 부팅 극초반에는 per-CPU 메모리 영역이 아직 완전히 구성되지 않았지만, 이후 실행되는 디버그 코드나 초기화 루틴 일부는 per-CPU 변수를 참조 가능.
	이를 안전하게 처리하기 위해 smp_setup_processor_id()는 부트 CPU의 per-CPU 오프셋을 0으로 설정하여, 현재는 CPU 0만 존재한다는 가정 하에 안정적인 접근이 가능하도록 만든다.

	SMP 전체 초기화 과정에서의 위치
	중요한 점은 smp_setup_processor_id()가 SMP 환경을 완성하는 함수는 아님.
	이 함수는 다른 CPU를 깨우지 않으며, 다중 CPU 스케줄링을 시작하지도 않음.
	대신 이 함수는 SMP 초기화가 가능해지기 위한 최소한의 전제 조건만을 마련.
	이후 커널은 디바이스 트리나 ACPI를 통해 시스템에 존재하는 모든 CPU를 탐색하고, 각 CPU를 logical ID에 매핑한 뒤, 펌웨어를 통해 secondary CPU들을 기동하여 완전한 SMP 환경으로 진입.
	*/
	smp_setup_processor_id(); //arch/arm/kernel/setup.c
	/*
	커널의 디버그 오브젝트 추적 서브 시스템을 초기에 켜는 작업
	디버그오브젝트도 smp처럼 의미가 있는 것인가?
	커널 안에 debugobjects라는 디버깅 서브시스템(프레임워크)이 있음
	그게 타이머/워크큐/RCU head 같은 커널 오브젝트들의 수명(lifetime) 규칙을 추적하기 위해
	내부적으로 추적용 엔트리(=debug object record)를 만들어 관리
	디버그 오브젝트는 실제 커널 오브젝트(커널에 의해 생성되는 메모립 블록)가 아닌 추적하긴 메타데이터(데이터에 관한 데이터) 레코드(묶음)
	커널 객체의 잘못된 생명주기를 잡아냄
	초기화 안 된 객체 사용
	이미 해제된 객체 재사용
	double init(두번 초기화) / double free(두번 할당 해제)
	대표적으로 감시하는 객체들 : timers / workqueues / rcu head / completion / perf events 등
	- 타이머(timer):
	한 번만 초기화되어야 하며, 등록(add/mod)된 상태에서는 재초기화되거나
	해제되면 안 된다. del_timer() 없이 객체를 해제하거나 이중 초기화를 하면
	타이머 리스트 손상이나 use-after-free로 이어질 수 있다.
	- 워크큐(workqueue):
	하나의 work 객체는 동시에 여러 번 큐에 들어가면 안 되며,
	실행 중이거나 대기 중인 상태에서 해제되거나 재사용되면 안 된다.
	이를 어길 경우 이중 실행, 데이터 레이스, use-after-free가 발생할 수 있다.
	- RCU 콜백:
	RCU grace period가 끝나기 전에 객체를 해제해서는 안 되며,
	동일한 rcu_head를 두 번 call_rcu()해서도 안 된다.
	규칙 위반 시 즉각적인 오류 없이 메모리 오염이나 지연된 크래시가 발생할 수 있다.
 
	어떤 커널 객체들의 생명주기를 체크해서 이중 초기화나 이중 해제 같은 문제를 해결하는 debugobjects 서브 시스템이 부팅 초반에도 그 검사를 할 수 있도록 해시테이블 락과 정적 메타 데이터 풀을 준비하는 함수.

	early boot 환경이란
	early boot는 커널이 실행되기 시작했지만, 아직 커널의 핵심 인프라가 완전히 준비되지 않은 상태
	동적 메모리 할당(kmalloc, slab allocator)을 신뢰할 수 없음
	스케줄러 미동작 (sleep, block 불가)
	lockdep(락 디버깅) 미초기화
	인터럽트/IRQ 관리가 제한적
	SMP 환경 미완성 (부트 CPU만 동작)

	debugobjects란
	debugobjects는 커널 객체들의 생명주기(init → use → free)를 추적하는 디버깅 서브시스템.
	'타이머, 워크큐, RCU 콜백'과 같은 객체들은 명확한 사용 규칙을 가지며,	이 규칙이 어긋나면 커널은 매우 치명적인 '버그'에 빠질 수 있다.
	debugobjects는 이중 초기화,	이중 해제, '해제된 객체 재사용, 잘못된 상태 전이' 등을 감지한다.
	이를 위해 debugobjects는 실제 객체를 수정하지 않고,	해당 객체의 주소를 키로 삼아 추적 메타데이터(debug_obj) 를 별도로 관리.
	어떤 객체를 추적할지는 타이머, 워커, rcu 같은 코드가 init/free등 할 때 체크 함수가 객체 주소 넘겨줌
	메모리 주소가 이미 확보된 상태고 debugobjects는 이 주소를 키로 추적 메타데이터를 관리하므로,
	동일한 주소에 대해 초기화가 다시 수행되어도 메모리 주소는 바뀌지 않음

	추적 메타데이터(debug_obj)
	debug_obj는 추적 대상 객체의 상태를 기록하는 관리용 구조체.
	obj_static_pool[] 정적 배열에 담겨있음.
	추적 대상 객체의 주소, 현재 생명주기 상태, 해당 객체 타입의 규칙, 리스트 연결용 hlist_node 등을 가짐

	해시 테이블(obj_hash[])
	// TODO : 같은 해시값 충돌
	같은 해시값으로 충돌하면, 그 버킷 안의 hlist를 순회해서 주소를 직접 비교
	debugobjects는 객체 주소를 빠르게 찾기 위해 해시 테이블을 사용.
	빠른 이유는 찾을 위치를 미리 계산해서 바로 점프하기 때문에 비교 기반 탐색 구조(리스트, 트리 등)보다 평균적으로 훨씬 빠르다.
	객체 주소 → 해시 함수 → 버킷 인덱스
	각 버킷은 raw_spinlock, hlist_head를 가짐
	버킷 안의 hlist에는 현재 추적 중인 객체들의 debug_obj 메타데이터가 연결되어 있다.
	해시 테이블을 사용함으로써, debugobjects는 평균적으로 O(1) 시간에 객체 상태를 조회할 수 있다.
	O(1)의 의미는 데이터 개수가 늘어나도 연산 횟수가 거의 늘어나지 않는다

	왜 버킷마다 락이 필요한가
	객체 등록, 상태 검사, 해제 과정에서 버킷의 hlist는 수정되는데 이 작업은 인터럽트 컨텍스트나 다른 CPU 경로에서도 동시에 발생할 수 있음
	동기화 없이 접근하면 리스트 구조 손상, 잘못된 debug_obj 참조, 커널 크래시 등의 문제가 생김.

	왜 raw spinlock을 사용하는가
	early boot 환경에서는 일반 spinlock이 내부적으로 의존하는 요소들인 lockdep 미초기화, 디버그 훅 재귀 가능성, 스케줄러/IRQ 상태 불안정등이 아직 안전하지 않음
	raw_spinlock은 이러한 부가 기능을 배제한 가장 원시적인 동기화 수단으로, early boot에서도 잠들지 않고, 의존성 없이 확실한 동기화를 제공한다.

	정적 debug_obj 풀과 pool_boot
	obj_static_pool[] → debug_obj 실체가 들어 있는 정적 배열
	pool_boot → 미사용 debug_obj들을 관리하는 free-list(hlist_head)
	미사용 상태 → pool_boot
	사용 중 상태 → 해시 버킷 hlist

	첫 번째 반복문
	해시 테이블 obj_hash[]의 각 버킷 락을 raw spinlock으로 초기화
	early boot에서도 버킷 hlist를 안전하게 조작할 수 있도록 동기화 기반 마련
	두 번째 반복문
	정적 debug_obj 배열(obj_static_pool[])의 원소들을 pool_boot free-list(hlist)에 전부 연결
	early boot에서 사용할 미사용 debug_obj 공급원 준비
	*/
	debug_objects_early_init();
	/*
	커널에서 에러가 나면 그 주소만 뜸
	어떤 커널에서 어떤 함수들을 거쳐서 어디서 터졌는지에 대한 호출 경로를 쌓는 곳이 스택 트레이스

	지금 실행 중인 커널(vmlinux)의 Build ID를 부팅 초기에 계산해서 전역 배열(vmlinux_build_id[])에 저장
	vmlinux : vmlinux는 압축되지 않은 커널 이미지를 ELF 형식으로 담고 있는 정적 링크된 실행 파일이라서, 사실상 커널 그 자체
	커널 이미지 : 커널이 하나의 파일로 디스크에 저장되어 있는 것
	Build ID란 이 커널 바이너리가 정확히 어떤 빌드 결과물인지 식별하는 지문(fingerprint) 같은 값
	커널 크래시 로그/스택트레이스 분석
	커널이 뿌린 주소/심볼이 어떤 vmlinux 디버그 심볼 파일과 맞는지 확인해야 함
	Build ID가 있으면 이 덤프/로그는 이 vmlinu와 정확히 한 쌍이라는 걸 강하게 보장 가능
	?build_id = vmlinux_build_id / 결과를 저장할 전역 배열(최대 BUILD_ID_SIZE_MAX 바이트)

	kdump(vmcore) 분석
	vmcore를 crash 툴로 분석할 때도 커널 빌드 식별이 필요하고, vmcoreinfo와 엮여서 분석 정확도를 올림

	커널이 notes 섹션 시작/끝 주소를 링커 심볼로 받음
	그 범위를 build-id가 들어있는 곳으로 보고 크기 계산
	builld_id_parse_buf()러 notes를 파싱 후
	찾은 Build ID를 전역 배열 vmlinux_builld_id에 저장
	init 이후에는 __ro_after_init로 보호(읽기 전용)

	note란 : ELF 파일 안에 들어가는 작은 메타데이터 블록이다(이 바이너리에 대한 설명서 조각 같은 것 / 코드 x, 데이터 x)
	ELF 파일에는 코드 섹션(.text), 데이터 섹션(.data, .rodata), 심볼 정보, 디버그 정보, note 섹션
	note 섹션 특징 : 실행 흐름과는 무관, CPU가 실행하지 않음, 툴/커널/디버거가 정보 일기용으로만 사용
	Build ID는 ELF note 안에 존재
	Build-id = note 안의 desc 데이터
	
	notes 시작 주소
	│
	├─ note #1
	│   ├─ Elf32_Nhdr
	│   ├─ name ("GNU\0" 등) + padding
	│   └─ desc (데이터) + padding
	│
	├─ note #2
	│   ├─ Elf32_Nhdr
	│   ├─ name
	│   └─ desc
	│
	├─ note #3
	│   ...
	│
	└─ notes 끝 주소

	init_vmlinux_build_id()가 호출되는 시점은 커닐 부팅 중
	자기 자신(vmlinux)을 이미 메모리 로드해서 실행 중인 상태
	notes 섹션은 커널 바이너리(vmlinux)의 일부 섹션
	-> notes 섹션은 이미 커널 메모리 안에 존재함
	커널 링커 스크립트가 자동으로 만들어주는 심볼인 __start_notes, __stop_notes
	커널 링크 단계 : 여러 개의 컴파일된 결과물(.o)을 하나의 실행 파일(vmlinux)로 합치는 단계
	링커가 vmlinux를 만들면서 .text, .data, .notes 같은 섹션을 배치하고
	__start_notes,__stop_notes 같은 주소 심볼 확정
	부팅 시 그 주소가 메모리에서 그대로 유지
	notes 섹션 시작 주소 : __start_notes, notes 섹션 끝 주소 : __stop_notes
	
	ELF 형식은 리눅스에서 실행 파일과 커널이 따르는 표준 바이너리 포맷
	파일 내부에는 실행을 위한 코드 영역(.text), 전역 및 정적 데이터 영역(.data, .bss), 심볼 테이블과 디버깅 정보를 담은 섹션, 그리고 바이너리 식별과 메타데이터를 위한 ELF NOTE 영역이 함께 포함
	vmlinux는 이러한 ELF 형식으로 빌드된 리눅스 커널의 원본 실행 파일로, 압축되지 않은 상태에서 코드·데이터·심볼·디버그 정보·build-id NOTE를 모두 포함
	init_vmlinux_build_id()는 커널 부팅 초기에 메모리에 로드된 vmlinux의 ELF NOTE 영역을 파싱하여 build-id를 추출하고 이를 커널 내부에 저장함
	이후 크래시 덤프 분석이나 디버깅 과정에서 실행 중이던 커널을 정확히 식별할 수 있도록 함
	*/
	init_vmlinux_build_id(); //lib/buildid.c
	/*
	cgroup(control group)은 리눅스 커널이 제공하는 자원 관리 프레임워크
	프로세스들을 논리적인 그룹으로 묶고, 그 그룹 단위로 cpu, 메모리, I/O 같은 시스템 자원을 제한,추적,분배 하기 위한 커널 매커니즘
	프로세스 관리 기능 x, 스케줄러나 메모리 관리자를 대체하지 않음 대신 스케줄러, 메모리 관리, 입출력 레이어 에 정책을 붙이는 틀

	cgroup(tree root)
		|- group A
		|		|- cpu 설정
		|		|- mem 설정
		|		|- I/O 설정
		|		|- ...
		|- group B
		|		|- cpu 설정
		|		|- mem 설정
		|		|- I/O 설정
		|		|- ...
		|- ...

	task_struct -> css_set -> cgroup -> group A

	root cgroup
	├─ cpu 설정/통계
	├─ memory 설정/통계
	├─ io 설정/통계
	│
	├─ group1 cgroup
	│   ├─ cpu 설정/통계
	│   ├─ memory 설정/통계
	│   └─ io 설정/통계
	│
	└─ group2 cgroup
		├─ cpu 설정/통계
		├─ memory 설정/통계
		└─ io 설정/통계

	cgroup 루트는 시스템 전체 자원의 기준점을 가지고 있으며,
	자식 cgroup들은 그 범위 안에서 CPU·메모리·IO에 대한 제한이나 비중 규칙을 가지며,
	실행 시 커널은 각 프로세스가 속한 cgroup과 그 부모 체인을 기준으로 자원 사용을 판단한다.

	cgroup에서 부모는 자원의 상한을 정하고, 자식은 그 범위 안에서 비중·제한 규칙을 가지며,
	실행 시 컨트롤러는 자식부터 부모·루트까지의 설정을 함께 고려해 경쟁 상황에서 자원 사용을 결정한다.

	프로세스가 자원을 쓰려고 하면:
	1. 자기 cgroup(자식) 의 규칙을 본다
	-> 이 그룹에서 허용되는가?
	2. 부모 cgroup 의 규칙을 본다
	-> 부모가 정한 상한을 넘는가?
	3. root cgroup 의 규칙을 본다
	-> 시스템 전체 한도를 넘는가?
	4. 위 조건을 모두 만족하면 실행
	-> 하나라도 막히면 제한/대기/실패
	자원은 루트에서 내려오는 게 아니라 허용 여부를 위로 확인하는 구조
	*/
	cgroup_init_early();
	/*
	목적 : 현재 CPU에서 실행 중인 코드가 인터럽트에 의해 중단·침범되지 않도록 보장하기 위해서 인터럽트를 비활성화하는 매크로
	*/
	local_irq_disable();
	/*
	아직 인터럽트 없는 부팅 초반 구간이다를 커널 전체에 선언하는 상태 플래그
	irq 관련 API나 락 코드들이 early boot 특수 상황을 고려해 동작하도록 돕는다.
	*/
	early_boot_irqs_disabled = true;

	/*
	 * Interrupts are still disabled. Do necessary setups, then
	 * enable them.
	 */
	boot_cpu_init();
	page_address_init();
	pr_notice("%s", linux_banner); //
	setup_arch(&command_line);
	/* Static keys and static calls are needed by LSMs */
	jump_label_init();
	static_call_init();
	early_security_init();
	setup_boot_config();
	setup_command_line(command_line);
	setup_nr_cpu_ids();
	setup_per_cpu_areas();
	smp_prepare_boot_cpu();	/* arch-specific boot-cpu hooks */
	early_numa_node_init();
	boot_cpu_hotplug_init();

	pr_notice("Kernel command line: %s\n", saved_command_line);
	/* parameters may set static keys */
	parse_early_param();
	after_dashes = parse_args("Booting kernel",
				  static_command_line, __start___param,
				  __stop___param - __start___param,
				  -1, -1, NULL, &unknown_bootoption);
	print_unknown_bootoptions();
	if (!IS_ERR_OR_NULL(after_dashes))
		parse_args("Setting init args", after_dashes, NULL, 0, -1, -1,
			   NULL, set_init_arg);
	if (extra_init_args)
		parse_args("Setting extra init args", extra_init_args,
			   NULL, 0, -1, -1, NULL, set_init_arg);

	/* Architectural and non-timekeeping rng init, before allocator init */
	random_init_early(command_line);

	/*
	 * These use large bootmem allocations and must precede
	 * initalization of page allocator
	 */
	setup_log_buf(0);
	vfs_caches_init_early();
	sort_main_extable();
	trap_init();
	mm_core_init();
	maple_tree_init();
	poking_init();
	ftrace_init();

	/* trace_printk can be enabled here */
	early_trace_init();

	/*
	 * Set up the scheduler prior starting any interrupts (such as the
	 * timer interrupt). Full topology setup happens at smp_init()
	 * time - but meanwhile we still have a functioning scheduler.
	 */
	sched_init();

	if (WARN(!irqs_disabled(),
		 "Interrupts were enabled *very* early, fixing it\n"))
		local_irq_disable();
	radix_tree_init();

	/*
	 * Set up housekeeping before setting up workqueues to allow the unbound
	 * workqueue to take non-housekeeping into account.
	 */
	housekeeping_init();

	/*
	 * Allow workqueue creation and work item queueing/cancelling
	 * early.  Work item execution depends on kthreads and starts after
	 * workqueue_init().
	 */
	workqueue_init_early();

	rcu_init();
	kvfree_rcu_init();

	/* Trace events are available after this */
	trace_init();

	if (initcall_debug)
		initcall_debug_enable();

	context_tracking_init();
	/* init some links before init_ISA_irqs() */
	early_irq_init();
	init_IRQ();
	tick_init();
	rcu_init_nohz();
	timers_init();
	srcu_init();
	hrtimers_init();
	softirq_init();
	timekeeping_init();
	time_init();

	/* This must be after timekeeping is initialized */
	random_init();

	/* These make use of the fully initialized rng */
	kfence_init();
	boot_init_stack_canary();

	perf_event_init();
	profile_init();
	call_function_init();
	WARN(!irqs_disabled(), "Interrupts were enabled early\n");

	early_boot_irqs_disabled = false;
	local_irq_enable();

	kmem_cache_init_late();

	/*
	 * HACK ALERT! This is early. We're enabling the console before
	 * we've done PCI setups etc, and console_init() must be aware of
	 * this. But we do want output early, in case something goes wrong.
	 */
	console_init();
	if (panic_later)
		panic("Too many boot %s vars at `%s'", panic_later,
		      panic_param);

	lockdep_init();

	/*
	 * Need to run this when irqs are enabled, because it wants
	 * to self-test [hard/soft]-irqs on/off lock inversion bugs
	 * too:
	 */
	locking_selftest();

#ifdef CONFIG_BLK_DEV_INITRD
	if (initrd_start && !initrd_below_start_ok &&
	    page_to_pfn(virt_to_page((void *)initrd_start)) < min_low_pfn) {
		pr_crit("initrd overwritten (0x%08lx < 0x%08lx) - disabling it.\n",
		    page_to_pfn(virt_to_page((void *)initrd_start)),
		    min_low_pfn);
		initrd_start = 0;
	}
#endif
	setup_per_cpu_pageset();
	numa_policy_init();
	acpi_early_init();
	if (late_time_init)
		late_time_init();
	sched_clock_init();
	calibrate_delay();

	arch_cpu_finalize_init();

	pid_idr_init();
	anon_vma_init();
	thread_stack_cache_init();
	cred_init();
	fork_init();
	proc_caches_init();
	uts_ns_init();
	time_ns_init();
	key_init();
	security_init();
	dbg_late_init();
	net_ns_init();
	vfs_caches_init();
	pagecache_init();
	signals_init();
	seq_file_init();
	proc_root_init();
	nsfs_init();
	pidfs_init();
	cpuset_init();
	mem_cgroup_init();
	cgroup_init();
	taskstats_init_early();
	delayacct_init();

	acpi_subsystem_init();
	arch_post_acpi_subsys_init();
	kcsan_init();

	/* Do the rest non-__init'ed, we're now alive */
	rest_init();

	/*
	 * Avoid stack canaries in callers of boot_init_stack_canary for gcc-10
	 * and older.
	 */
#if !__has_attribute(__no_stack_protector__)
	prevent_tail_call_optimization();
#endif
}

/* Call all constructor functions linked into the kernel. */
static void __init do_ctors(void)
{
/*
 * For UML, the constructors have already been called by the
 * normal setup code as it's just a normal ELF binary, so we
 * cannot do it again - but we do need CONFIG_CONSTRUCTORS
 * even on UML for modules.
 */
#if defined(CONFIG_CONSTRUCTORS) && !defined(CONFIG_UML)
	ctor_fn_t *fn = (ctor_fn_t *) __ctors_start;

	for (; fn < (ctor_fn_t *) __ctors_end; fn++)
		(*fn)();
#endif
}

#ifdef CONFIG_KALLSYMS
struct blacklist_entry {
	struct list_head next;
	char *buf;
};

static __initdata_or_module LIST_HEAD(blacklisted_initcalls);

static int __init initcall_blacklist(char *str)
{
	char *str_entry;
	struct blacklist_entry *entry;

	/* str argument is a comma-separated list of functions */
	do {
		str_entry = strsep(&str, ",");
		if (str_entry) {
			pr_debug("blacklisting initcall %s\n", str_entry);
			entry = memblock_alloc_or_panic(sizeof(*entry),
					       SMP_CACHE_BYTES);
			entry->buf = memblock_alloc_or_panic(strlen(str_entry) + 1,
						    SMP_CACHE_BYTES);
			strcpy(entry->buf, str_entry);
			list_add(&entry->next, &blacklisted_initcalls);
		}
	} while (str_entry);

	return 1;
}

static bool __init_or_module initcall_blacklisted(initcall_t fn)
{
	struct blacklist_entry *entry;
	char fn_name[KSYM_SYMBOL_LEN];
	unsigned long addr;

	if (list_empty(&blacklisted_initcalls))
		return false;

	addr = (unsigned long) dereference_function_descriptor(fn);
	sprint_symbol_no_offset(fn_name, addr);

	/*
	 * fn will be "function_name [module_name]" where [module_name] is not
	 * displayed for built-in init functions.  Strip off the [module_name].
	 */
	strreplace(fn_name, ' ', '\0');

	list_for_each_entry(entry, &blacklisted_initcalls, next) {
		if (!strcmp(fn_name, entry->buf)) {
			pr_debug("initcall %s blacklisted\n", fn_name);
			return true;
		}
	}

	return false;
}
#else
static int __init initcall_blacklist(char *str)
{
	pr_warn("initcall_blacklist requires CONFIG_KALLSYMS\n");
	return 0;
}

static bool __init_or_module initcall_blacklisted(initcall_t fn)
{
	return false;
}
#endif
__setup("initcall_blacklist=", initcall_blacklist);

static __init_or_module void
trace_initcall_start_cb(void *data, initcall_t fn)
{
	ktime_t *calltime = data;

	printk(KERN_DEBUG "calling  %pS @ %i\n", fn, task_pid_nr(current));
	*calltime = ktime_get();
}

static __init_or_module void
trace_initcall_finish_cb(void *data, initcall_t fn, int ret)
{
	ktime_t rettime, *calltime = data;

	rettime = ktime_get();
	printk(KERN_DEBUG "initcall %pS returned %d after %lld usecs\n",
		 fn, ret, (unsigned long long)ktime_us_delta(rettime, *calltime));
}

static __init_or_module void
trace_initcall_level_cb(void *data, const char *level)
{
	printk(KERN_DEBUG "entering initcall level: %s\n", level);
}

static ktime_t initcall_calltime;

#ifdef TRACEPOINTS_ENABLED
static void __init initcall_debug_enable(void)
{
	int ret;

	ret = register_trace_initcall_start(trace_initcall_start_cb,
					    &initcall_calltime);
	ret |= register_trace_initcall_finish(trace_initcall_finish_cb,
					      &initcall_calltime);
	ret |= register_trace_initcall_level(trace_initcall_level_cb, NULL);
	WARN(ret, "Failed to register initcall tracepoints\n");
}
# define do_trace_initcall_start	trace_initcall_start
# define do_trace_initcall_finish	trace_initcall_finish
# define do_trace_initcall_level	trace_initcall_level
#else
static inline void do_trace_initcall_start(initcall_t fn)
{
	if (!initcall_debug)
		return;
	trace_initcall_start_cb(&initcall_calltime, fn);
}
static inline void do_trace_initcall_finish(initcall_t fn, int ret)
{
	if (!initcall_debug)
		return;
	trace_initcall_finish_cb(&initcall_calltime, fn, ret);
}
static inline void do_trace_initcall_level(const char *level)
{
	if (!initcall_debug)
		return;
	trace_initcall_level_cb(NULL, level);
}
#endif /* !TRACEPOINTS_ENABLED */

int __init_or_module do_one_initcall(initcall_t fn)
{
	int count = preempt_count();
	char msgbuf[64];
	int ret;

	if (initcall_blacklisted(fn))
		return -EPERM;

	do_trace_initcall_start(fn);
	ret = fn();
	do_trace_initcall_finish(fn, ret);

	msgbuf[0] = 0;

	if (preempt_count() != count) {
		sprintf(msgbuf, "preemption imbalance ");
		preempt_count_set(count);
	}
	if (irqs_disabled()) {
		strlcat(msgbuf, "disabled interrupts ", sizeof(msgbuf));
		local_irq_enable();
	}
	WARN(msgbuf[0], "initcall %pS returned with %s\n", fn, msgbuf);

	add_latent_entropy();
	return ret;
}


static initcall_entry_t *initcall_levels[] __initdata = {
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
static const char *initcall_level_names[] __initdata = {
	"pure",
	"core",
	"postcore",
	"arch",
	"subsys",
	"fs",
	"device",
	"late",
};

static int __init ignore_unknown_bootoption(char *param, char *val,
			       const char *unused, void *arg)
{
	return 0;
}

static void __init do_initcall_level(int level, char *command_line)
{
	initcall_entry_t *fn;

	parse_args(initcall_level_names[level],
		   command_line, __start___param,
		   __stop___param - __start___param,
		   level, level,
		   NULL, ignore_unknown_bootoption);

	do_trace_initcall_level(initcall_level_names[level]);
	for (fn = initcall_levels[level]; fn < initcall_levels[level+1]; fn++)
		do_one_initcall(initcall_from_entry(fn));
}

static void __init do_initcalls(void)
{
	int level;
	size_t len = saved_command_line_len + 1;
	char *command_line;

	command_line = kzalloc(len, GFP_KERNEL);
	if (!command_line)
		panic("%s: Failed to allocate %zu bytes\n", __func__, len);

	for (level = 0; level < ARRAY_SIZE(initcall_levels) - 1; level++) {
		/* Parser modifies command_line, restore it each time */
		strcpy(command_line, saved_command_line);
		do_initcall_level(level, command_line);
	}

	kfree(command_line);
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
	cpuset_init_smp();
	driver_init();
	init_irq_proc();
	do_ctors();
	do_initcalls();
}

static void __init do_pre_smp_initcalls(void)
{
	initcall_entry_t *fn;

	do_trace_initcall_level("early");
	for (fn = __initcall_start; fn < __initcall0_start; fn++)
		do_one_initcall(initcall_from_entry(fn));
}

static int run_init_process(const char *init_filename)
{
	const char *const *p;

	argv_init[0] = init_filename;
	pr_info("Run %s as init process\n", init_filename);
	pr_debug("  with arguments:\n");
	for (p = argv_init; *p; p++)
		pr_debug("    %s\n", *p);
	pr_debug("  with environment:\n");
	for (p = envp_init; *p; p++)
		pr_debug("    %s\n", *p);
	return kernel_execve(init_filename, argv_init, envp_init);
}

static int try_to_run_init_process(const char *init_filename)
{
	int ret;

	ret = run_init_process(init_filename);

	if (ret && ret != -ENOENT) {
		pr_err("Starting init: %s exists but couldn't execute it (error %d)\n",
		       init_filename, ret);
	}

	return ret;
}

static noinline void __init kernel_init_freeable(void);

#if defined(CONFIG_STRICT_KERNEL_RWX) || defined(CONFIG_STRICT_MODULE_RWX)
bool rodata_enabled __ro_after_init = true;

#ifndef arch_parse_debug_rodata
static inline bool arch_parse_debug_rodata(char *str) { return false; }
#endif

static int __init set_debug_rodata(char *str)
{
	if (arch_parse_debug_rodata(str))
		return 0;

	if (str && !strcmp(str, "on"))
		rodata_enabled = true;
	else if (str && !strcmp(str, "off"))
		rodata_enabled = false;
	else
		pr_warn("Invalid option string for rodata: '%s'\n", str);
	return 0;
}
early_param("rodata", set_debug_rodata);
#endif

static void mark_readonly(void)
{
	if (IS_ENABLED(CONFIG_STRICT_KERNEL_RWX) && rodata_enabled) {
		/*
		 * load_module() results in W+X mappings, which are cleaned
		 * up with init_free_wq. Let's make sure that queued work is
		 * flushed so that we don't hit false positives looking for
		 * insecure pages which are W+X.
		 */
		flush_module_init_free_work();
		jump_label_init_ro();
		mark_rodata_ro();
		debug_checkwx();
		rodata_test();
	} else if (IS_ENABLED(CONFIG_STRICT_KERNEL_RWX)) {
		pr_info("Kernel memory protection disabled.\n");
	} else if (IS_ENABLED(CONFIG_ARCH_HAS_STRICT_KERNEL_RWX)) {
		pr_warn("Kernel memory protection not selected by kernel config.\n");
	} else {
		pr_warn("This architecture does not have kernel memory protection.\n");
	}
}

void __weak free_initmem(void)
{
	free_initmem_default(POISON_FREE_INITMEM);
}

static int __ref kernel_init(void *unused)
{
	int ret;

	/*
	 * Wait until kthreadd is all set-up.
	 */
	wait_for_completion(&kthreadd_done);

	kernel_init_freeable();
	/* need to finish all async __init code before freeing the memory */
	async_synchronize_full();

	system_state = SYSTEM_FREEING_INITMEM;
	kprobe_free_init_mem();
	ftrace_free_init_mem();
	kgdb_free_init_mem();
	exit_boot_config();
	free_initmem();
	mark_readonly();

	/*
	 * Kernel mappings are now finalized - update the userspace page-table
	 * to finalize PTI.
	 */
	pti_finalize();

	system_state = SYSTEM_RUNNING;
	numa_default_policy();

	rcu_end_inkernel_boot();

	do_sysctl_args();

	if (ramdisk_execute_command) {
		ret = run_init_process(ramdisk_execute_command);
		if (!ret)
			return 0;
		pr_err("Failed to execute %s (error %d)\n",
		       ramdisk_execute_command, ret);
	}

	/*
	 * We try each of these until one succeeds.
	 *
	 * The Bourne shell can be used instead of init if we are
	 * trying to recover a really broken machine.
	 */
	if (execute_command) {
		ret = run_init_process(execute_command);
		if (!ret)
			return 0;
		panic("Requested init %s failed (error %d).",
		      execute_command, ret);
	}

	if (CONFIG_DEFAULT_INIT[0] != '\0') {
		ret = run_init_process(CONFIG_DEFAULT_INIT);
		if (ret)
			pr_err("Default init %s failed (error %d)\n",
			       CONFIG_DEFAULT_INIT, ret);
		else
			return 0;
	}

	if (!try_to_run_init_process("/sbin/init") ||
	    !try_to_run_init_process("/etc/init") ||
	    !try_to_run_init_process("/bin/init") ||
	    !try_to_run_init_process("/bin/sh"))
		return 0;

	panic("No working init found.  Try passing init= option to kernel. "
	      "See Linux Documentation/admin-guide/init.rst for guidance.");
}

/* Open /dev/console, for stdin/stdout/stderr, this should never fail */
void __init console_on_rootfs(void)
{
	struct file *file = filp_open("/dev/console", O_RDWR, 0);

	if (IS_ERR(file)) {
		pr_err("Warning: unable to open an initial console.\n");
		return;
	}
	init_dup(file);
	init_dup(file);
	init_dup(file);
	fput(file);
}

static noinline void __init kernel_init_freeable(void)
{
	/* Now the scheduler is fully set up and can do blocking allocations */
	gfp_allowed_mask = __GFP_BITS_MASK;

	/*
	 * init can allocate pages on any node
	 */
	set_mems_allowed(node_states[N_MEMORY]);

	cad_pid = get_pid(task_pid(current));

	smp_prepare_cpus(setup_max_cpus);

	workqueue_init();

	init_mm_internals();

	do_pre_smp_initcalls();
	lockup_detector_init();

	smp_init();
	sched_init_smp();

	workqueue_init_topology();
	async_init();
	padata_init();
	page_alloc_init_late();

	do_basic_setup();

	kunit_run_all_tests();

	wait_for_initramfs();
	console_on_rootfs();

	/*
	 * check if there is an early userspace init.  If yes, let it do all
	 * the work
	 */
	int ramdisk_command_access;
	ramdisk_command_access = init_eaccess(ramdisk_execute_command);
	if (ramdisk_command_access != 0) {
		pr_warn("check access for rdinit=%s failed: %i, ignoring\n",
			ramdisk_execute_command, ramdisk_command_access);
		ramdisk_execute_command = NULL;
		prepare_namespace();
	}

	/*
	 * Ok, we have completed the initial bootup, and
	 * we're essentially up and running. Get rid of the
	 * initmem segments and start the user-mode stuff..
	 *
	 * rootfs is available now, try loading the public keys
	 * and default modules
	 */

	integrity_load_keys();
}
