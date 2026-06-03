#!/bin/bash
BINARY=$1
INSTALLPATH="$2"
WORKDIR="$3"

if [ -z "$DESTDIR" ]; then
	OUTDIR="$INSTALLPATH"
else
	if echo "$INSTALLPATH" | grep "^[A-Z]:" >/dev/null 2>&1; then
		INSTALLPATH="${INSTALLPATH:3}"
	fi
	OUTDIR="$WORKDIR/$DESTDIR/$INSTALLPATH"
fi

mkdir -p "$OUTDIR"

IFS=$'\n'
if which ntldd 2>&1 | grep /ntldd >/dev/null 2>&1; then
	DLLS=$(ntldd -R "$BINARY" | grep -i mingw | cut -d">" -f2 | sed -e 's/(0x[0-9a-f]\+)//' -e 's/^ \+//' -e 's/ \+$//' -e 's,\\,/,g')
elif which gdb 2>&1 | grep /gdb >/dev/null 2>&1; then
	DLLS=$(gdb "$BINARY" --command=$(dirname $0)/dlls.gdb | grep -i mingw | cut -d" " -f7- | sed -e 's/^ \+//' -e 's/ \+$//' -e 's,\\,/,g')
else
	echo "Please install gdb or ntldd for deploying DLLs"
fi
if [ -n "$DLLS" ]; then
	cp -vu $DLLS "$OUTDIR"
fi
for MGBA_DLL in "$(dirname "$BINARY")/mgba.dll" "$(dirname "$BINARY")/libmgba.dll"; do
	if [ -f "$MGBA_DLL" ]; then
		cp -vu "$MGBA_DLL" "$OUTDIR"
	fi
done
if which windeployqt 2>&1 | grep /windeployqt >/dev/null 2>&1; then
	windeployqt --no-opengl-sw --release --dir "$OUTDIR" "$BINARY"
fi
