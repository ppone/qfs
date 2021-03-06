#!/bin/sh
#
# $Id$
#
# Created 2010/10/20
# Author: Mike Ovsiannikov
#
# Copyright 2010-2011 Quantcast Corp.
#
# This file is part of Kosmos File System (KFS).
#
# Licensed under the Apache License, Version 2.0
# (the "License"); you may not use this file except in compliance with
# the License. You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
# implied. See the License for the specific language governing
# permissions and limitations under the License.
#
#

if [ x"$1" = x'-g' -o  x"$1" = x'-get' ]; then
    while [ $# -gt 1 ]; do
        shift
        strings -a "$1" | awk '/KFS_BUILD_INFO_START/,/KFS_BUILD_INFO_END/'
    done
    exit
fi

if [ $# -lt 3 ]; then
    echo "Usage: $0 <build type> <source dir> <dest file>"
    echo "or: $0 -g <kfs executable>"
    exit 1;
fi

buildtype=$1
shift
sourcedir=$1
shift
outfile=$1
shift

if [ x"$P4PORT" = x -o x"$KFS_BUILD_VERS_NO_P4" != x ]; then
    lastchangenum=0
else
    lastchange=`p4 changes -m 1 -t "$sourcedir/...#have"`
    lastchangenum=`echo "$lastchange" | awk '/Change /{c=$2;} END{printf("%d", c);}'`
fi
if [ $lastchangenum -ne 0 ]; then
    p4path=`p4 have "$sourcedir/CMakeLists.txt" | sed -e 's/CMakeLists.txt.*$//'`
else
    p4path='//unspecified/'
fi

tmpfile="$outfile.$$.tmp";

{
echo '
// Generated by '"$0"'. Do not edit.

#include "Version.h"
#include "hsieh_hash.h"

namespace KFS {

const std::string KFS_BUILD_INFO_STRING='

{
echo KFS_BUILD_INFO_START
echo "host: `hostname`"
echo "user: $USER"
echo "date: `date`" 
echo "build type: $buildtype"
while [ $# -gt 0 ]; do
    echo "$1"
    shift
done
echo "p4: $P4PORT"
if [ $lastchangenum -ne 0 ]; then
    p4 info
    echo "$lastchange"
    echo "${p4path}...@$lastchangenum"
    {
        p4 have "$sourcedir"/...
        echo 'opened:'
        p4 opened "$sourcedir"/... 2>/dev/null
    } | sed -e 's/\(#[0-9]*\) - .*$/\1/'
else
    echo 'p4 source build version disabled'
    echo "KFS_BUILD_VERS_NO_P4: $KFS_BUILD_VERS_NO_P4"
fi
echo KFS_BUILD_INFO_END
} | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$/\\n"/'

echo ';
static std::string MakeVersionHash()
{
    Hsieh_hash_fcn f;
    unsigned int h = (unsigned int)f(KFS_BUILD_INFO_STRING);
    std::string ret(2 * sizeof(h), '"'0'"');
    for (size_t i = ret.length() - 1; h != 0; i--) {
        ret[i] = "0123456789ABCDEF"[h & 0xF];
        h >>= 4;
    }
    return ret;
}
const std::string KFS_BUILD_VERSION_STRING(
    std::string("'"${lastchangenum}-${buildtype}"'-") +
    MakeVersionHash()
);
const std::string KFS_SOURCE_REVISION_STRING(
    "'"${p4path}...@$lastchangenum"'"
);
}
'

} > "$tmpfile"
mv "$tmpfile" $outfile
