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
/* $Source: /home/mike/src/cvs/openbsd/src/sys/arch/hppa/spmath/mpyucv.c,v $
 * $Revision: 1.1.1.1 $	$Author: mickey $
 * $State: Exp $   	$Locker:  $
 * $Date: 2002/05/07 21:37:19 $
 */


#include "../spmath/md.h"

VOID mpyucv(opnd1,opnd2,result)

unsigned int opnd1, opnd2;
struct mdsfu_register *result;
{
	impyu(&opnd1,&opnd2,result);
	overflow = FALSE;
	return;
}
