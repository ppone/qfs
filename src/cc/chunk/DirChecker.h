//---------------------------------------------------------- -*- Mode: C++ -*-
// $Id$
//
// Created 2011/7/10
// Author: Mike Ovsiannikov
//
// Copyright 2011-2012 Quantcast Corp.
//
// This file is part of Kosmos File System (KFS).
//
// Licensed under the Apache License, Version 2.0
// (the "License"); you may not use this file except in compliance with
// the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied. See the License for the specific language governing
// permissions and limitations under the License.
//
// \file DirChecker.h
// \brief Checks if directories / drives to re-appears.
//
//----------------------------------------------------------------------------

#ifndef DIR_CHECKER_H
#define DIR_CHECKER_H

#include <set>
#include <string>
#include <map>
#include <inttypes.h>

#include <boost/shared_ptr.hpp>

namespace KFS
{

using std::set;
using std::string;
using std::map;
using std::pair;
using boost::shared_ptr;

// "Off line" chunk directory monitor.
// When chunk directory deemed to be "off line" / unusable chunk manager adds to
// the directory monitoring thread. Once chunk directory becomes "available" the
// monitoring thread acquires lock, deletes all files in this directory, and
// adds directory to "available list". Chunk manager periodically invokes
// GetNewlyAvailable() and puts newly available directories in use.
// Directories with files with names from the "black" / "don't use" list aren't
// considered available until such files are removed / renamed. Typically the
// "black" list contains "evacuate", and "evacuate.done".
class DirChecker
{
public:
    class LockFd
    {
    public:
        LockFd(
            int inFd = -1)
            : mFd(inFd)
            {}
        ~LockFd();
    private:
        const int mFd;
    private:
        LockFd(
            const LockFd& inLockFd);
        LockFd& operator=(
            const LockFd& inLockFd);
    };
    typedef shared_ptr<LockFd> LockFdPtr;
    typedef int64_t     DeviceId;
    typedef set<string> FileNames;
    typedef FileNames   DirNames;
    typedef map<string, pair<DeviceId, LockFdPtr> > DirsAvailable;

    DirChecker();
    ~DirChecker();
    void Clear();
    bool Add(
        const string& inDirName);
    bool Add(
        const string& inDirName,
        LockFdPtr&    ioLockFdPtr);
    bool Remove(
        const string& inDirName);
    bool Add(
        const DirNames& inDirNames);
    bool Remove(
        const DirNames& inDirNames);
    void GetNewlyAvailable(
        DirsAvailable& outDirs,
        bool           inSyncFlag = false);
    void Start(
        DirsAvailable& outDirs);
    void Stop();
    void SetInterval(
        int inTimeMilliSec);
    int GetInterval();
    void AddSubDir(
        const string& inDirName);
    void SetDontUseIfExist(
        const FileNames& inFileNames);
    void SetLockFileName(
        const string& inName);
    void SetRemoveFilesFlag(
        bool inFlag);
private:
    class Impl;
    Impl& mImpl;
private:
    DirChecker(
        const DirChecker& inChecker);
    DirChecker& operator=(
        const DirChecker& inChecker);
};

};

#endif /* DIR_CHECKER_H */
