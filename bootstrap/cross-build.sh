#!/bin/sh
#
# Run the supported cross-build stages in dependency order.

set -e

SCRIPTDIR="$(cd "$(dirname "$0")" && pwd)"

run_stage()
{
	case "$1" in
	bmake)
		"${SCRIPTDIR}/bootstrap-bmake.sh"
		;;
	tools)
		"${SCRIPTDIR}/build-tools.sh"
		;;
	userland)
		"${SCRIPTDIR}/cross-build-userland.sh"
		;;
	kernel)
		"${SCRIPTDIR}/cross-build-kernel.sh"
		;;
	*)
		echo "usage: $0 [bmake|tools|userland|kernel ...]" >&2
		exit 1
		;;
	esac
}

if [ "$#" -eq 0 ]; then
	set -- bmake tools userland kernel
fi

for stage do
	run_stage "${stage}"
done
