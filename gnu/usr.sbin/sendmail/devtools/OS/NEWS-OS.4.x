#	$Id: NEWS-OS.4.x,v 1.1.1.2 2001/01/15 20:51:59 millert Exp $
define(`confBEFORE', `limits.h')
define(`confMAPDEF', `-DNDBM')
define(`confLIBS', `-lmld')
define(`confMBINDIR', `/usr/lib')
define(`confSBINDIR', `/usr/etc')
define(`confUBINDIR', `/usr/ucb')
define(`confEBINDIR', `/usr/lib')
PUSHDIVERT(3)
limits.h:
	touch limits.h
POPDIVERT
