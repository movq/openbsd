/*
  (c) Copyright 1986 HEWLETT-PACKARD COMPANY
  To anyone who acknowledges that this file is provided "AS IS"
  without any express or implied warranty:
      permission to use, copy, modify, and distribute this file
  for any purpose is hereby granted without fee, provided that
  the above copyright notice and this notice appears in all
  copies, and that the name of Hewlett-Packard Company not be
  used in advertising or publicity pertaining to distribution
  of the software without specific, written prior permission.
  Hewlett-Packard Company makes no representations about the
  suitability of this software for any purpose.
*/
/* $Source: /home/mike/src/cvs/openbsd/src/sys/arch/hppa/spmath/mpyaccs.c,v $
 * $Revision: 1.1.1.1 $	$Author: mickey $
 * $State: Exp $   	$Locker:  $
 * $Date: 2002/05/07 21:37:19 $
 */


#include "../spmath/md.h"

VOID mpyaccs(opnd1,opnd2,result)

int opnd1, opnd2;
struct mdsfu_register *result;
{
	struct mdsfu_register temp;
	int carry, sign;

	impys(&opnd1,&opnd2,&temp);

	/* get result of low word add, and check for carry out */
	if ((result_lo += (unsigned)temp.rslt_lo) < (unsigned)temp.rslt_lo) 
		carry = 1;
	else carry = 0;

	/* get result of high word add, and determine overflow status */
	sign = result_hi ^ temp.rslt_hi;
	result_hi += temp.rslt_hi + carry;
	if (sign >= 0 && (temp.rslt_hi ^ result_hi) < 0) overflow = TRUE;

	return;
}
