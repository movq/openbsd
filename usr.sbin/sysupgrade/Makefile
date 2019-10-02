#	$OpenBSD: Makefile,v 1.1.2.1 2019/10/02 17:18:14 benno Exp $

MAN=	sysupgrade.8

SCRIPT=	sysupgrade.sh

realinstall:
	${INSTALL} ${INSTALL_COPY} -o ${BINOWN} -g ${BINGRP} -m ${BINMODE} \
		${.CURDIR}/${SCRIPT} ${DESTDIR}${BINDIR}/sysupgrade

.include <bsd.prog.mk>
