#	$Id: NCR.m4,v 1.1.1.2 2001/01/15 20:51:53 millert Exp $
depend: ${BEFORE} ${LINKS}
	@mv Makefile Makefile.old
	@sed -e '/^# Do not edit or remove this line or anything below it.$$/,$$d' < Makefile.old > Makefile
	@echo "# Do not edit or remove this line or anything below it." >> Makefile
	${CC} -w0 -Hmake ${COPTS} ${SRCS} >> Makefile

#	End of $RCSfile: NCR.m4,v $
