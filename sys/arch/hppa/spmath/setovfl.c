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
/* $Source: /home/mike/src/cvs/openbsd/src/sys/arch/hppa/spmath/Attic/setovfl.c,v $
 * $Revision: 1.1.1.1 $	$Author: mickey $
 * $State: Exp $   	$Locker:  $
 * $Date: 2002/05/07 21:37:19 $
 */


#include "../spmath/float.h"
#include "../spmath/sgl_float.h"
#include "../spmath/dbl_float.h"

sgl_floating_point 

/*ARGSUSED*/
sgl_setoverflow(sign)

unsigned int sign;
{

	/* set result to infinity or largest number */
	/* ignore for now
	switch (Rounding_mode()) {
		case ROUNDPLUS:
			if (sign) {
				Sgl_setlargestnegative(result);
			}
			else {
				Sgl_setinfinitypositive(result);
			}
			break;
		case ROUNDMINUS:
			if (sign==0) {
				Sgl_setlargestpositive(result);
			}
			else {
				Sgl_setinfinitynegative(result);
			}
			break;
		case ROUNDNEAREST:
			Sgl_setinfinity(result,sign);
			break;
		case ROUNDZERO:
			Sgl_setlargest(result,sign);
	}
	return(result);
	*/
}

dbl_floating_point 

/*ARGSUSED*/
dbl_setoverflow(sign) 

unsigned int sign;
{


	/* set result to infinity or largest number */
	/* ignore for now
	switch (Rounding_mode()) {
		case ROUNDPLUS:
			if (sign) {
				Dbl_setlargestnegative(result);
			}
			else {
				Dbl_setinfinitypositive(result);
			}
			break;
		case ROUNDMINUS:
			if (sign==0) {
				Dbl_setlargestpositive(result);
			}
			else {
				Dbl_setinfinitynegative(result);
			}
			break;
		case ROUNDNEAREST:
			Dbl_setinfinity(result,sign);
			break;
		case ROUNDZERO:
			Dbl_setlargest(result,sign);
	}
	return(result);
	*/
}
