# Makefile for miscelaneous programs
# Copyright (C) 2002-2006  Michael Richardson	<mcr@xelerance.com>
# 
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the
# Free Software Foundation; either version 2 of the License, or (at your
# option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
# 
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
# or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
# for more details.

LIBRESWANSRCDIR?=$(shell cd ../..; pwd)
include ${LIBRESWANSRCDIR}/Makefile.inc

PROGRAM=_updown.bsdkame

include ${srcdir}../Makefile.program

programs: ${PROGRAM}

cleanall::
	rm -f ${UPDOWN_PROGRAM} ${PROGRAM}


