#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include "threads/synch.h"

struct file;

extern struct lock filesys_lock;

#define FD_MAX 64	/* fd 테이블 칸 수 */

void syscall_init (void);

/* fd 테이블 */
void fd_table_create (void);
int fd_add (struct file *file);
void fd_close (int fd);
void fd_table_close_all (void);
void sys_exit (int status);
#endif /* userprog/syscall.h */
