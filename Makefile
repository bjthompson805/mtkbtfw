PROG=		mtkbtfw
MAN=		mtkbtfw.8
LDADD+=		-lusb
WARNS?=		6
BINDIR?=	/usr/local/sbin
MANDIR?=	/usr/local/share/man/man
FWDIR?=		/usr/local/share/mtkbt-firmware

install-devd:
	install -m 0644 mtkbtfw.conf /usr/local/etc/devd/mtkbtfw.conf

.include <bsd.prog.mk>
