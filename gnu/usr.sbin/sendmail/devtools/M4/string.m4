divert(-1)
#
# Copyright (c) 1999 Sendmail, Inc. and its suppliers.
#	All rights reserved.
#
# By using this file, you agree to the terms and conditions set
# forth in the LICENSE file which can be found at the top level of
# the sendmail distribution.
#
#
#  Definitions for Makefile construction for sendmail
#
#	$Id: string.m4,v 1.1.1.2 2001/01/15 20:51:53 millert Exp $
#
divert(0)dnl
define(`bldRINDEX',
`ifelse(index($1, $2), `-1', `-1', `eval(index($1, $2) + bldRINDEX(substr($1, eval(index($1, $2) + 1)), $2) + 1)')'dnl
)dnl
