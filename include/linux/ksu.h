#ifndef __LINUX_KSU_H
#define __LINUX_KSU_H

#include <linux/types.h>
#include <linux/uaccess.h>

struct filename;
struct file;

#ifdef CONFIG_KSU
#include "../../drivers/kernelsu/include/ksu_hook.h"
#else

static inline int ksu_handle_execveat(int *fd,
				      struct filename **filename_ptr,
				      void *argv, void *envp, int *flags)
{
	return 0;
}

static inline int ksu_handle_faccessat(int *dfd,
				       const char __user **filename_user,
				       int *mode, int *flags)
{
	return 0;
}

static inline int ksu_handle_stat(int *dfd,
				  const char __user **filename_user,
				  int *flags)
{
	return 0;
}

static inline int ksu_handle_vfs_read(struct file **file_ptr,
				      char __user **buf_ptr,
				      size_t *count_ptr,
				      loff_t **pos_ptr)
{
	return 0;
}

static inline int ksu_handle_input_handle_event(unsigned int *type,
						unsigned int *code,
						int *value)
{
	return 0;
}

#endif /* CONFIG_KSU */

#endif /* __LINUX_KSU_H */
