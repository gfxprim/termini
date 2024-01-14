#!/bin/sh

echo "// Generated file do not touch" > config.h

gcc  -o /dev/null -x c - > /dev/null 2>&1 << EOF
#include <vterm.h>
int main(void)
{
	VTermColor col;
	vterm_color_indexed(&col, 0);
	return 0;
}
EOF

if [ $? -ne 0 ]; then
	echo "// HAVE_COLOR_INDEXED is not set" >> config.h
else
	echo "#define HAVE_COLOR_INDEXED 1" >> config.h
fi
