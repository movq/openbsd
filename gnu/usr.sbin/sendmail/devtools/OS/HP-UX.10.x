#	$Id: HP-UX.10.x,v 1.1.1.2 2001/01/15 20:51:57 millert Exp $
define(`confCC', `cc -Aa')
define(`confMAPDEF', `-DNDBM -DNIS -DMAP_REGEX')
define(`confENVDEF', `-D_HPUX_SOURCE -DV4FS -D_PATH_SENDMAIL=\"/usr/sbin/sendmail\"')
define(`confOPTIMIZE', `+O3')
define(`confLIBS', `-lndbm')
define(`confSHELL', `/usr/bin/sh')
define(`confINSTALL', `${BUILDBIN}/install.sh')
define(`confSBINGRP', `mail')
