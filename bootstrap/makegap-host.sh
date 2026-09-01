#!/bin/sh -
#
# Portable host implementation of the kernel makegap.sh helper.

random_uniform()
{
	upper_bound=$1
	if [ "${upper_bound}" -le 0 ]; then
		echo 0
		return
	fi

	# Field splitting removes padding emitted by some implementations of od.
	set -- $(od -An -N4 -tu4 /dev/urandom 2>/dev/null)
	random_value=${1:-0}
	echo $((random_value % upper_bound))
}

umask 007

if PAGE_SIZE=$(getconf PAGESIZE 2>/dev/null) && [ -n "${PAGE_SIZE}" ]; then
	:
elif PAGE_SIZE=$(sysctl -n hw.pagesize 2>/dev/null) &&
    [ -n "${PAGE_SIZE}" ]; then
	:
else
	PAGE_SIZE=4096
fi
case ${PAGE_SIZE} in
*[!0-9]*|'')
	PAGE_SIZE=4096
	;;
esac

PAD=$1
GAPDUMMY=$2

RANDOM1=$(random_uniform $((3 * PAGE_SIZE)))
RANDOM2=$(random_uniform ${PAGE_SIZE})
RANDOM3=$(random_uniform ${PAGE_SIZE})
RANDOM4=$(random_uniform ${PAGE_SIZE})
RANDOM5=$(random_uniform ${PAGE_SIZE})

cat > gap.link << __EOF__

PHDRS {
	text PT_LOAD FILEHDR PHDRS;
	rodata PT_LOAD;
	data PT_LOAD;
	bss PT_LOAD;
}

SECTIONS {
	.text : ALIGN($PAGE_SIZE) {
		LONG($PAD);
		. += $RANDOM1;
		. = ALIGN($PAGE_SIZE);
		endboot = .;
		PROVIDE (endboot = .);
		. = ALIGN($PAGE_SIZE);
		. += $RANDOM2;
		. = ALIGN(16);
		*(.text .text.*)
	} :text =$PAD

	.rodata : {
		LONG($PAD);
		. += $RANDOM3;
		. = ALIGN(16);
		*(.rodata .rodata.*)
	} :rodata =$PAD

	.data : {
		LONG($PAD);
		. = . + $RANDOM4;	/* fragment of page */
		. = ALIGN(16);
		*(.data .data.*)
	} :data =$PAD

	.bss : {
		. = . + $RANDOM5;	/* fragment of page */
		. = ALIGN(16);
		*(.bss .bss.*)
	} :bss
}
__EOF__

${LD} ${LDFLAGS} -r gap.link ${GAPDUMMY} -o gap.o
