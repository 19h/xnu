/*
 * Copyright (c) 2000 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * Copyright (c) 1999-2003 Apple Computer, Inc.  All Rights Reserved.
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
 * Copyright (c) 1992 NeXT Computer, Inc.
 *
 * Intel386 Family:	Processor exception frame.
 *
 * HISTORY
 *
 * 31 August 1992 ? at NeXT
 *	Added v86 mode stuff.
 *
 * 8 June 1992 ? at NeXT
 *	Changed name of write field in err_code_t
 *	which collided with write() in shlib.
 *
 * 30 March 1992 ? at NeXT
 *	Created.
 */

/*
 * Format of the error code
 * generated by the hardware
 * for certain exceptions.
 */
 
typedef union err_code {
    struct err_code_normal {
	unsigned int	ext	:1,
			tbl	:2,
#define ERR_GDT		0
#define ERR_IDT		1
#define ERR_LDT		2
			index	:13,
				:16;
    } normal;
    struct err_code_pgfault {
	unsigned int	prot	:1,
			wrtflt	:1,
			user	:1,
				:29;
    } pgfault;
} err_code_t;

#include <architecture/i386/sel.h>

/*
 * The actual hardware exception frame
 * is variable in size.  An error code is
 * only pushed for certain exceptions.
 * Previous stack information is only
 * pushed for exceptions that cause a
 * change in privilege level.  The dpl
 * field of the saved CS selector can be
 * used to determine whether this is the
 * case.  If the interrupted task was
 * executing in v86 mode, then the data
 * segment registers are also present in
 * the exception frame (in addition to
 * previous stack information).  This
 * case can be determined by examining
 * eflags.
 */

typedef struct except_frame {
    err_code_t		err;
    unsigned int	eip;
    sel_t		cs;
    unsigned int		:0;
    unsigned int	eflags;
    unsigned int	esp;
    sel_t		ss;
    unsigned int		:0;
    unsigned short	v_es;
    unsigned int		:0;
    unsigned short	v_ds;
    unsigned int		:0;
    unsigned short	v_fs;
    unsigned int		:0;
    unsigned short	v_gs;
    unsigned int		:0;
} except_frame_t;

/*
 * Values in eflags.
 */

#ifndef	EFL_CF	/* FIXME */
#define EFL_CF		0x00001
#define EFL_PF		0x00004
#define EFL_AF		0x00010
#define EFL_ZF		0x00040
#define EFL_SF		0x00080
#define EFL_TF		0x00100
#define EFL_IF		0x00200
#define EFL_DF		0x00400
#define EFL_OF		0x00800
#define EFL_IOPL	0x03000
#define EFL_NT		0x04000
#define EFL_RF		0x10000
#define EFL_VM		0x20000
#define EFL_AC		0x40000
#endif

#define EFL_CLR		0xfff88028
#define EFL_SET		0x00000002
