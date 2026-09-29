#!/bin/sh
#-----------------------------------------------------------------------------
# @file    version.sh
# @brief   Print the HanOS version string
# @details
# @verbatim
#
#   The version core (MAJOR.MINOR.PATCH) lives in the top-level VERSION file.
#   Every build stamps it with the build date, so the reported version is
#   "<core>.<YYMMDD>" (for example 0.1.2.260929). The kernel build and the
#   documentation both use this script so the format is defined once.
#
# @endverbatim
#-----------------------------------------------------------------------------
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
core=$(cat "$root/VERSION")

printf '%s.%s\n' "$core" "$(date +%y%m%d)"
