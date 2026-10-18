/* HanOS: set the FS base through SYSCALL_SET_FS_BASE (0x401) instead of Linux
 * arch_prctl (158). The argument is already in rdi. */
.text
.global __set_thread_area
.hidden __set_thread_area
.type __set_thread_area,@function
__set_thread_area:
	movl $0x401,%eax        /* SYSCALL_SET_FS_BASE */
	syscall
	ret
