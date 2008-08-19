/*	$OpenBSD: db_interface.c,v 1.8 2008/08/19 08:26:20 kettenis Exp $	*/
/*      $NetBSD: db_interface.c,v 1.12 2001/07/22 11:29:46 wiz Exp $ */

#include <sys/param.h>
#include <sys/proc.h>
#include <sys/systm.h>

#include <dev/cons.h>

#include <machine/db_machdep.h>
#include <ddb/db_extern.h>

int db_active = 0;

int ddb_trap_glue(struct trapframe *frame); /* called from locore */

void
Debugger()
{
	ddb_trap();
}

int
ddb_trap_glue(struct trapframe *frame)
{
	if (!(frame->srr1 & PSL_PR)
	    && (frame->exc == EXC_TRC
		|| (frame->exc == EXC_PGM && (frame->srr1 & 0x20000))
		|| frame->exc == EXC_BPT)) {

		bcopy(frame->fixreg, DDB_REGS->tf.fixreg,
			32 * sizeof(u_int32_t));
		DDB_REGS->tf.srr0 = frame->srr0;
		DDB_REGS->tf.srr1 = frame->srr1;

		db_active++;
		cnpollc(TRUE);
		db_trap(T_BREAKPOINT, 0);
		cnpollc(FALSE);
		db_active--;

		bcopy(DDB_REGS->tf.fixreg, frame->fixreg,
			32 * sizeof(u_int32_t));

		return 1;
	}
	return 0;
}
