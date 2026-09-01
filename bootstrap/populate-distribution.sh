#!/bin/sh
#
# populate-distribution.sh - stage the useful parts of etc(8)'s
# distribution-etc-root-var target from a GNU/Linux host.
#
# Usage:
#   ./bootstrap/populate-distribution.sh [MACHINE=amd64] [DESTDIR=bootstrap/dest]
#
# This intentionally does not run OpenBSD pwd_mkdb(8).  It installs
# master.passwd and a generated passwd text file, then warns about the
# missing pwd.db/spwd.db files.
#
set -e

SCRIPTDIR="$(cd "$(dirname "$0")" && pwd)"
SRCDIR="$(cd "${SCRIPTDIR}/.." && pwd)"

MACHINE="${MACHINE:-amd64}"
DESTDIR="${DESTDIR:-${SCRIPTDIR}/obj/dest-${MACHINE}}"
BOOTSTRAP_HOSTNAME="${BOOTSTRAP_HOSTNAME:-openbsd-bootstrap}"
FSTAB_ROOT="${FSTAB_ROOT:-/dev/wd0a}"

need_file() {
	if [ ! -f "$1" ]; then
		echo "missing required file: $1" >&2
		exit 1
	fi
}

install_file() {
	mode="$1"
	src="$2"
	dst="$3"

	need_file "${src}"
	mkdir -p "$(dirname "${dst}")"
	rm -f "${dst}"
	cp "${src}" "${dst}"
	chmod "${mode}" "${dst}"
	echo "    ${dst#${DESTDIR}/}"
}

install_empty() {
	mode="$1"
	dst="$2"

	mkdir -p "$(dirname "${dst}")"
	rm -f "${dst}"
	: > "${dst}"
	chmod "${mode}" "${dst}"
	echo "    ${dst#${DESTDIR}/}"
}

make_symlink() {
	target="$1"
	link="$2"

	if [ -L "${link}" ] || [ -e "${link}" ]; then
		rm -f "${link}" 2>/dev/null || {
			echo "warning: not replacing non-file ${link#${DESTDIR}/}" >&2
			return
		}
	fi
	mkdir -p "$(dirname "${link}")"
	ln -s "${target}" "${link}"
	echo "    ${link#${DESTDIR}/} -> ${target}"
}

create_mtree_dirs() {
	awk '
		/^[[:space:]]*(#|$|\/set)/ { next }
		{
			line = $0
			sub(/#.*/, "", line)
			sub(/^[[:space:]]+/, "", line)
			sub(/[[:space:]]+$/, "", line)
			if (line == "")
				next
			split(line, field, /[[:space:]]+/)
			name = field[1]
			if (name == ".")
				next
			if (name == "..") {
				if (depth > 0)
					depth--
				next
			}
			path = ""
			for (i = 0; i < depth; i++)
				path = path stack[i] "/"
			print path name
			stack[depth++] = name
		}
	' "${SRCDIR}/etc/mtree/4.4BSD.dist" | while IFS= read -r dir; do
		mkdir -p "${DESTDIR}/${dir}"
	done
}

install_into_etc() {
	mode="$1"
	shift
	for src in "$@"; do
		install_file "${mode}" "${SRCDIR}/etc/${src}" \
		    "${DESTDIR}/etc/$(basename "${src}")"
	done
}

echo "==> Populating OpenBSD distribution files"
echo "    SRCDIR   = ${SRCDIR}"
echo "    DESTDIR  = ${DESTDIR}"
echo "    MACHINE  = ${MACHINE}"
echo ""

need_file "${SRCDIR}/etc/etc.${MACHINE}/MAKEDEV"
mkdir -p "${DESTDIR}"

echo "==> Creating mtree directories"
create_mtree_dirs
chmod 755 "${DESTDIR}/etc" "${DESTDIR}/var" "${DESTDIR}/var/db" \
    "${DESTDIR}/var/log" "${DESTDIR}/var/run" "${DESTDIR}/var/mail" \
    "${DESTDIR}/var/yp" 2>/dev/null || true
chmod 1777 "${DESTDIR}/tmp"
chmod 700 "${DESTDIR}/root" "${DESTDIR}/root/.ssh" \
    "${DESTDIR}/etc/acme" "${DESTDIR}/etc/ssl/private" \
    "${DESTDIR}/etc/ldap/certs" \
    "${DESTDIR}/etc/iked/private" "${DESTDIR}/etc/isakmpd/private" 2>/dev/null || true
make_symlink "usr/src/sys" "${DESTDIR}/sys"
echo ""

echo "==> Installing /etc base files"
install_into_etc 644 \
	changelist \
	etc.${MACHINE}/disktab \
	etc.${MACHINE}/login.conf \
	ftpusers \
	gettytab \
	group \
	ksh.kshrc \
	locate.rc \
	mailer.conf \
	moduli \
	monthly \
	netstart \
	newsyslog.conf \
	ntpd.conf \
	pf.os \
	protocols \
	rc \
	rc.conf \
	rpc \
	services \
	shells \
	syslog.conf \
	weekly

sh "${SRCDIR}/etc/ttys.pty" | cat "${SRCDIR}/etc/etc.${MACHINE}/ttys" - \
    > "${DESTDIR}/etc/ttys"
chmod 644 "${DESTDIR}/etc/ttys"
echo "    etc/ttys"

cat "${SRCDIR}/etc/examples/sysctl.conf" \
    "${SRCDIR}/etc/etc.${MACHINE}/sysctl.conf" \
    > "${DESTDIR}/etc/examples/sysctl.conf"
chmod 644 "${DESTDIR}/etc/examples/sysctl.conf"
echo "    etc/examples/sysctl.conf"

cat "${SRCDIR}/etc/fbtab.head" "${SRCDIR}/etc/etc.${MACHINE}/fbtab" \
    "${SRCDIR}/etc/fbtab.tail" > "${DESTDIR}/etc/fbtab"
chmod 644 "${DESTDIR}/etc/fbtab"
echo "    etc/fbtab"

install_file 664 "${SRCDIR}/etc/motd" "${DESTDIR}/etc/motd"
install_file 600 "${SRCDIR}/etc/master.passwd" "${DESTDIR}/etc/master.passwd"
awk -F: 'NF >= 10 { print $1 ":*:" $3 ":" $4 ":" $8 ":" $9 ":" $10 }' \
    "${DESTDIR}/etc/master.passwd" > "${DESTDIR}/etc/passwd"
chmod 644 "${DESTDIR}/etc/passwd"
echo "    etc/passwd"

install_file 600 "${SRCDIR}/etc/pf.conf" "${DESTDIR}/etc/pf.conf"
install_file 640 "${SRCDIR}/etc/nsd.conf" "${DESTDIR}/var/nsd/etc/nsd.conf"
install_file 644 "${SRCDIR}/etc/unbound.conf" "${DESTDIR}/var/unbound/etc/unbound.conf"
install_file 555 "${SRCDIR}/etc/etc.${MACHINE}/MAKEDEV" "${DESTDIR}/dev/MAKEDEV"
install_file 600 "${SRCDIR}/etc/crontab" "${DESTDIR}/var/cron/tabs/root"

if [ ! -e "${DESTDIR}/etc/fstab" ]; then
	printf '%s / ffs rw 1 1\n' "${FSTAB_ROOT}" > "${DESTDIR}/etc/fstab"
	chmod 644 "${DESTDIR}/etc/fstab"
	echo "    etc/fstab"
fi
if [ ! -e "${DESTDIR}/etc/myname" ]; then
	printf '%s\n' "${BOOTSTRAP_HOSTNAME}" > "${DESTDIR}/etc/myname"
	chmod 644 "${DESTDIR}/etc/myname"
	echo "    etc/myname"
fi
echo ""

echo "==> Installing root and skeleton files"
install_file 644 "${SRCDIR}/etc/root/dot.cshrc" "${DESTDIR}/root/.cshrc"
install_file 644 "${SRCDIR}/etc/root/dot.login" "${DESTDIR}/root/.login"
install_file 644 "${SRCDIR}/etc/root/dot.profile" "${DESTDIR}/root/.profile"
install_file 644 "${SRCDIR}/etc/root/dot.Xdefaults" "${DESTDIR}/root/.Xdefaults"
install_file 644 "${SRCDIR}/etc/root/dot.cvsrc" "${DESTDIR}/root/.cvsrc"
install_empty 600 "${DESTDIR}/root/.ssh/authorized_keys"
install_file 644 "${SRCDIR}/etc/root/dot.cshrc" "${DESTDIR}/.cshrc"
install_file 644 "${SRCDIR}/etc/root/dot.profile" "${DESTDIR}/.profile"

install_file 644 "${SRCDIR}/etc/skel/dot.cshrc" "${DESTDIR}/etc/skel/.cshrc"
install_file 644 "${SRCDIR}/etc/skel/dot.login" "${DESTDIR}/etc/skel/.login"
install_file 644 "${SRCDIR}/etc/skel/dot.mailrc" "${DESTDIR}/etc/skel/.mailrc"
install_file 644 "${SRCDIR}/etc/skel/dot.profile" "${DESTDIR}/etc/skel/.profile"
install_file 644 "${SRCDIR}/etc/skel/dot.Xdefaults" "${DESTDIR}/etc/skel/.Xdefaults"
install_file 644 "${SRCDIR}/etc/skel/dot.cvsrc" "${DESTDIR}/etc/skel/.cvsrc"
install_empty 600 "${DESTDIR}/etc/skel/.ssh/authorized_keys"
echo ""

echo "==> Installing configuration examples and service data"
install_file 644 "${SRCDIR}/etc/amd/master.sample" "${DESTDIR}/etc/amd/master.sample"
for src in chap-secrets options options.sample chatscript.sample pap-secrets; do
	install_file 600 "${SRCDIR}/etc/ppp/${src}" "${DESTDIR}/etc/ppp/${src}"
done
for src in afrinic.tal apnic.tal arin.tal lacnic.tal ripe.tal \
    afrinic.constraints apnic.constraints arin.constraints \
    lacnic.constraints ripe.constraints; do
	install_file 644 "${SRCDIR}/etc/rpki/${src}" "${DESTDIR}/etc/rpki/${src}"
done

for src in acme-client.conf chio.conf dhcpd.conf exports httpd.conf \
    ifstated.conf inetd.conf man.conf mixerctl.conf mrouted.conf ntpd.conf \
    printcap rad.conf rbootd.conf remote sensorsd.conf wsconsctl.conf; do
	install_file 644 "${SRCDIR}/etc/examples/${src}" \
	    "${DESTDIR}/etc/examples/${src}"
done
for src in bgpd.conf doas.conf dvmrpd.conf eigrpd.conf hostapd.conf \
    iked.conf ipsec.conf ldapd.conf ldpd.conf login_ldap.conf ospf6d.conf \
    ospfd.conf pf.conf radiusd.conf rc.local rc.securelevel rc.shutdown \
    relayd.conf ripd.conf sasyncd.conf snmpd.conf vm.conf ypldap.conf; do
	install_file 600 "${SRCDIR}/etc/examples/${src}" \
	    "${DESTDIR}/etc/examples/${src}"
done
chmod 640 "${DESTDIR}/etc/examples/login_ldap.conf"

for src in "${SRCDIR}/etc/signify/"*.pub; do
	install_file 644 "${src}" "${DESTDIR}/etc/signify/$(basename "${src}")"
done

make_symlink "/usr/share/zoneinfo/Canada/Mountain" "${DESTDIR}/etc/localtime"
make_symlink "/usr/sbin/rmt" "${DESTDIR}/etc/rmt"
echo ""

echo "==> Installing subtarget distribution files"
install_file 644 "${SRCDIR}/usr.bin/ssh/ssh_config" "${DESTDIR}/etc/ssh/ssh_config"
install_file 644 "${SRCDIR}/usr.bin/ssh/sshd_config" "${DESTDIR}/etc/ssh/sshd_config"
install_file 444 "${SRCDIR}/lib/libcrypto/openssl.cnf" "${DESTDIR}/etc/ssl/openssl.cnf"
install_file 444 "${SRCDIR}/lib/libcrypto/cert.pem" "${DESTDIR}/etc/ssl/cert.pem"
install_file 444 "${SRCDIR}/lib/libcrypto/x509v3.cnf" "${DESTDIR}/etc/ssl/x509v3.cnf"
install_file 644 "${SRCDIR}/usr.sbin/ikectl/ikeca.cnf" "${DESTDIR}/etc/ssl/ikeca.cnf"
for src in bsd.schema core.schema inetorgperson.schema nis.schema; do
	install_file 644 "${SRCDIR}/usr.sbin/ldapd/schema/${src}" \
	    "${DESTDIR}/etc/ldap/${src}"
done
install_file 600 "${SRCDIR}/usr.sbin/npppd/npppd/npppd.conf" \
    "${DESTDIR}/etc/npppd/npppd.conf"
install_file 600 "${SRCDIR}/usr.sbin/npppd/npppd/npppd-users" \
    "${DESTDIR}/etc/npppd/npppd-users"
install_file 644 "${SRCDIR}/etc/mail/aliases" "${DESTDIR}/etc/mail/aliases"
install_file 644 "${SRCDIR}/etc/mail/smtpd.conf" "${DESTDIR}/etc/mail/smtpd.conf"
install_file 644 "${SRCDIR}/usr.bin/mail/misc/mail.rc" "${DESTDIR}/etc/mail.rc"
install_file 444 "${SRCDIR}/usr.bin/mail/misc/mail.help" "${DESTDIR}/usr/share/misc/mail.help"
install_file 444 "${SRCDIR}/usr.bin/mail/misc/mail.tildehelp" \
    "${DESTDIR}/usr/share/misc/mail.tildehelp"
for src in Makefile.main Makefile.yp; do
	install_file 644 "${SRCDIR}/usr.sbin/ypserv/ypinit/${src}" \
	    "${DESTDIR}/var/yp/${src}"
done
for src in bgplg.head bgplg.foot bgplg.css; do
	install_file 644 "${SRCDIR}/usr.bin/bgplg/${src}" \
	    "${DESTDIR}/var/www/conf/${src}"
done
install_file 644 "${SRCDIR}/usr.bin/bgplg/openbgpd.gif" \
    "${DESTDIR}/var/www/htdocs/bgplg/openbgpd.gif"
install_file 644 "${SRCDIR}/usr.bin/bgplg/index.html" \
    "${DESTDIR}/var/www/htdocs/bgplg/index.html"
echo ""

echo "==> Installing rc.d scripts"
install_file 644 "${SRCDIR}/etc/rc.d/rc.subr" "${DESTDIR}/etc/rc.d/rc.subr"
for src in amd apmd bgpd bgplgd bootparamd bpflogd cron dhcpd dhcpleased \
    dhcp6leased dhcrelay dhcrelay6 dvmrpd eigrpd ftpd ftpproxy ftpproxy6 \
    hostapd hotplugd httpd identd ifstated iked inetd isakmpd iscsid ldapd \
    ldattach ldomd ldpd lockd lpd mopd mountd mrouted nfsd npppd nsd ntpd \
    ospf6d ospfd pflogd portmap rad radiusd rarpd rbootd relayd resolvd \
    ripd route6d sasyncd sensorsd slowcgi slaacd smtpd sndiod snmpd spamd \
    spamlogd sshd statd syslogd tftpd tftpproxy unbound unwind vmd \
    watchdogd wsmoused xenodm ypbind ypldap ypserv; do
	install_file 555 "${SRCDIR}/etc/rc.d/${src}" "${DESTDIR}/etc/rc.d/${src}"
done
echo ""

echo "==> Creating empty state files"
install_empty 644 "${DESTDIR}/var/account/acct"
install_file 644 "${SRCDIR}/etc/minfree" "${DESTDIR}/var/crash/minfree"
install_empty 664 "${DESTDIR}/etc/dumpdates"
install_empty 660 "${DESTDIR}/var/cron/at.deny"
install_empty 660 "${DESTDIR}/var/cron/cron.deny"
install_empty 600 "${DESTDIR}/var/cron/log"
install_empty 444 "${DESTDIR}/var/db/locate.database"
install_empty 644 "${DESTDIR}/var/db/rpki-client/openbgpd"
for spec in \
    "640 authlog" \
    "640 daemon" \
    "600 failedlogin" \
    "640 ftpd" \
    "644 lastlog" \
    "640 lpd-errs" \
    "640 maillog" \
    "644 messages" \
    "600 secure" \
    "644 wtmp" \
    "640 xferlog"; do
	set -- ${spec}
	install_empty "$1" "${DESTDIR}/var/log/$2"
done
install_file 600 "${SRCDIR}/etc/root/root.mail" "${DESTDIR}/var/mail/root"
make_symlink "../tmp" "${DESTDIR}/var/tmp"
install_empty 644 "${DESTDIR}/var/sysmerge/etcsum"
echo ""

echo "==> Installing mtree files"
install_file 600 "${SRCDIR}/etc/mtree/special" "${DESTDIR}/etc/mtree/special"
install_file 444 "${SRCDIR}/etc/mtree/4.4BSD.dist" "${DESTDIR}/etc/mtree/4.4BSD.dist"
install_file 444 "${SRCDIR}/etc/mtree/BSD.x11.dist" "${DESTDIR}/etc/mtree/BSD.x11.dist"
echo ""

echo "==> Installing terminfo"
mkdir -p "${DESTDIR}/usr/share/terminfo"
tic -x -o "${DESTDIR}/usr/share/terminfo" "${SRCDIR}/share/termtypes/termtypes.master"
echo ""

echo "==> Distribution staging complete"
if [ ! -f "${DESTDIR}/etc/pwd.db" ] || [ ! -f "${DESTDIR}/etc/spwd.db" ]; then
	echo "warning: ${DESTDIR#${SRCDIR}/}/etc/pwd.db and spwd.db were not generated." >&2
	echo "warning: build or run an OpenBSD-compatible pwd_mkdb before relying on multi-user boot." >&2
fi
