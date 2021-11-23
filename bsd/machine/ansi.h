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
/* Copyright (c) 1995 NeXT Computer, Inc. All Rights Reserved */
/*
 * The NEXTSTEP Software License Agreement specifies the terms
 * and conditions for redistribution.
 *
 */

#ifndef	_MACHINE_ANSI_H_
#define	_MACHINE_ANSI_H_

#if defined (__ppc__)
#include "ppc/ansi.h"
#elif defined (__i386__)
#include "i386/ansi.h"
#else
#error architecture not supported
#endif

#ifdef KERNEL
#ifndef offsetof
#define offsetof(type, member)  ((size_t)(&((type *)0)->member))
#endif /* offsetof */
#endif /* KERNEL */

#endif	/* _MACHINE_ANSI_H_ */
