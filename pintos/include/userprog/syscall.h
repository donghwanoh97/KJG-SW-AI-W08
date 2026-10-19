#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include "threads/synch.h"

struct file;

extern struct lock filesys_lock;

void syscall_init (void);

/* fd 테이블 */
void fd_table_create (void);
int fd_add (struct file *file);
void fd_close (int fd);
void fd_table_close_all (void);

#endif /* userprog/syscall.h */
