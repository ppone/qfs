//---------------------------------------------------------- -*- Mode: C++ -*-
// $Id$
//
// Created 2006/03/28
// Author: Sriram Rao
//
// Copyright 2008-2012 Quantcast Corp.
// Copyright 2006-2008 Kosmix Corp.
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
//
//----------------------------------------------------------------------------

#include <dirent.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/types.h>

#include "common/MsgLogger.h"
#include "common/kfstypes.h"

#include "ChunkManager.h"
#include "ChunkServer.h"
#include "MetaServerSM.h"
#include "LeaseClerk.h"
#include "AtomicRecordAppender.h"
#include "utils.h"
#include "Logger.h"
#include "DiskIo.h"
#include "Replicator.h"

#include "kfsio/Counter.h"
#include "kfsio/checksum.h"
#include "kfsio/Globals.h"
#include "qcdio/QCUtils.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <string>
#include <set>

namespace KFS
{
using std::ofstream;
using std::ostringstream;
using std::min;
using std::max;
using std::string;
using std::vector;
using std::make_pair;
using std::sort;
using std::unique;
using std::greater;
using std::binary_function;

using namespace KFS::libkfsio;

ChunkManager gChunkManager;

typedef QCDLList<ChunkInfoHandle, 0> ChunkList;
typedef QCDLList<ChunkInfoHandle, 1> ChunkDirList;
typedef ChunkList ChunkLru;

// Chunk directory state. The present production deployment use one chunk
// directory per physical disk.
struct ChunkManager::ChunkDirInfo
{
    ChunkDirInfo()
        : dirname(),
          usedSpace(0),
          availableSpace(0),
          totalSpace(0),
          pendingReadBytes(0),
          pendingWriteBytes(0),
          corruptedChunksCount(0),
          evacuateCheckIoErrorsCount(0),
          evacuateStartByteCount(0),
          evacuateStartChunkCount(-1),
          chunkCount(0),
          diskTimeoutCount(0),
          evacuateInFlightCount(0),
          rescheduleEvacuateThreshold(0),
          diskQueue(0),
          deviceId(-1),
          dirLock(),
          countFsSpaceAvailableFlag(true),
          fsSpaceAvailInFlightFlag(false),
          checkDirReadableFlightFlag(false),
          checkEvacuateFileInFlightFlag(false),
          evacuateChunksOpInFlightFlag(false),
          evacuateFlag(false),
          evacuateStartedFlag(false),
          evacuateDoneFlag(false),
          evacuateFileRenameInFlightFlag(false),
          placementSkipFlag(false),
          lastEvacuationActivityTime(
            globalNetManager().Now() - 365 * 24 * 60 * 60),
          fsSpaceAvailCb(),
          checkDirReadableCb(),
          checkEvacuateFileCb(),
          evacuateChunksCb(),
          evacuateChunksOp(0, &evacuateChunksCb)
    {
        fsSpaceAvailCb.SetHandler(this,
            &ChunkDirInfo::FsSpaceAvailDone);
        checkDirReadableCb.SetHandler(this,
            &ChunkDirInfo::CheckDirReadableDone);
        checkEvacuateFileCb.SetHandler(this,
            &ChunkDirInfo::CheckEvacuateFileDone);
        evacuateChunksCb.SetHandler(this,
            &ChunkDirInfo::EvacuateChunksDone);
        renameEvacuateFileCb.SetHandler(this,
            &ChunkDirInfo::RenameEvacuateFileDone);
        for (int i = 0; i < kChunkDirListCount; i++) {
            ChunkList::Init(chunkLists[i]);
            ChunkDirList::Init(chunkLists[i]);
        }
    }
    int FsSpaceAvailDone(int code, void* data);
    int CheckDirReadableDone(int code, void* data);
    int CheckEvacuateFileDone(int code, void* data);
    int RenameEvacuateFileDone(int code, void* data);
    void DiskError(int sysErr);
    int EvacuateChunksDone(int code, void* data);
    void ScheduleEvacuate(int maxChunkCount = -1);
    void RestartEvacuation();
    void UpdateLastEvacuationActivityTime()
    {
        lastEvacuationActivityTime = globalNetManager().Now();
    }
    void ChunkEvacuateDone()
    {
        UpdateLastEvacuationActivityTime();
        if (evacuateInFlightCount > 0 &&
                --evacuateInFlightCount <= rescheduleEvacuateThreshold) {
            ScheduleEvacuate();
        }
    }
    void Stop()
    {
        for (int i = 0; i < kChunkDirListCount; i++) {
            if (! ChunkDirList::IsEmpty(chunkLists[i])) {
                die("chunk dir stop: chunk list is not empty");
            }
        }
        if (chunkCount != 0) {
            die("chunk dir stop: invalid chunk count");
            chunkCount = 0;
        }
        if (diskQueue) {
            string err;
            if (! DiskIo::StopIoQueue(
                    diskQueue, dirname.c_str(), deviceId, &err)) {
                die("failed to stop io queue: " + err);
            }
            deviceId  = -1;
            diskQueue = 0;
        }
        availableSpace              = -1;
        rescheduleEvacuateThreshold = 0;
        evacuateFlag                = false;
        evacuateStartedFlag         = false;
        evacuateDoneFlag            = false;
        diskTimeoutCount            = 0;
        countFsSpaceAvailableFlag   = false;
        usedSpace                   = 0;
        totalSpace                  = 0;
        evacuateStartChunkCount     = -1;
        evacuateStartByteCount      = -1;
    }
    void SetEvacuateStarted()
    {
        evacuateStartedFlag = true;
        evacuateStartChunkCount = max(evacuateStartChunkCount, chunkCount);
        evacuateStartByteCount  = max(evacuateStartByteCount, usedSpace);
    }
    int GetEvacuateDoneChunkCount() const
    {
            return (max(evacuateStartChunkCount, chunkCount) - chunkCount);
    }
    int64_t GetEvacuateDoneByteCount() const
    {
            return (max(evacuateStartByteCount, usedSpace) - usedSpace);
    }

    string                dirname;
    int64_t               usedSpace;
    int64_t               availableSpace;
    int64_t               totalSpace;
    int64_t               pendingReadBytes;
    int64_t               pendingWriteBytes;
    int64_t               corruptedChunksCount;
    int64_t               evacuateCheckIoErrorsCount;
    int64_t               evacuateStartByteCount;
    int                   evacuateStartChunkCount;
    int                   chunkCount;
    int                   diskTimeoutCount;
    int                   evacuateInFlightCount;
    int                   rescheduleEvacuateThreshold;
    DiskQueue*            diskQueue;
    DirChecker::DeviceId  deviceId;
    DirChecker::LockFdPtr dirLock;
    bool                  countFsSpaceAvailableFlag:1;
    bool                  fsSpaceAvailInFlightFlag:1;
    bool                  checkDirReadableFlightFlag:1;
    bool                  checkEvacuateFileInFlightFlag:1;
    bool                  evacuateChunksOpInFlightFlag:1;
    bool                  evacuateFlag:1;
    bool                  evacuateStartedFlag:1;
    bool                  evacuateDoneFlag:1;
    bool                  evacuateFileRenameInFlightFlag:1;
    bool                  placementSkipFlag:1;
    time_t                lastEvacuationActivityTime;
    KfsCallbackObj        fsSpaceAvailCb;
    KfsCallbackObj        checkDirReadableCb;
    KfsCallbackObj        checkEvacuateFileCb;
    KfsCallbackObj        evacuateChunksCb;
    KfsCallbackObj        renameEvacuateFileCb;
    EvacuateChunksOp      evacuateChunksOp;

    enum { kChunkInfoHDirListCount = kChunkInfoHandleListCount + 1 };
    enum ChunkListType
    {
        kChunkDirList         = 0,
        kChunkDirEvacuateList = 1,
        kChunkDirListNone     = 2
    };
    enum { kChunkDirListCount = kChunkDirEvacuateList + 1 };
    typedef ChunkInfoHandle* ChunkLists[kChunkInfoHDirListCount];
    ChunkLists chunkLists[kChunkDirListCount];

private:
    ChunkDirInfo(const ChunkDirInfo&);
    ChunkDirInfo& operator=(const ChunkDirInfo&);
};

inline ChunkManager::ChunkDirs::~ChunkDirs()
{
    delete [] mChunkDirs;
}

inline ChunkManager::ChunkDirs::iterator
ChunkManager::ChunkDirs::end()
{
    return mChunkDirs + mSize;
}

inline ChunkManager::ChunkDirs::const_iterator
ChunkManager::ChunkDirs::end() const
{
    return mChunkDirs + mSize;
}

inline ChunkManager::ChunkDirInfo&
ChunkManager::ChunkDirs::operator[](size_t i)
{
    return mChunkDirs[i];
}

inline const ChunkManager::ChunkDirInfo&
ChunkManager::ChunkDirs::operator[](size_t i) const
{
    return mChunkDirs[i];
}

void
ChunkManager::ChunkDirs::Allocate(size_t size)
{
    delete [] mChunkDirs;
    mChunkDirs = 0;
    mSize      = 0;
    mChunkDirs = new ChunkDirInfo[size];
    mSize      = size;
}

// OP for reading/writing out the meta-data associated with each chunk.  This
// is an internally generated op (ops that generate this one are
// allocate/write/truncate/change-chunk-vers).
struct WriteChunkMetaOp : public KfsOp {
    kfsChunkId_t const chunkId;
    DiskIo* const      diskIo;  /* disk connection used for writing data */
    IOBuffer           dataBuf; /* buffer with the data to be written */
    WriteChunkMetaOp*  next;
    const kfsSeq_t     targetVersion;
    const bool         renameFlag;
    const bool         stableFlag;

    WriteChunkMetaOp(
        kfsChunkId_t    c,
        KfsCallbackObj* o,
        DiskIo*         d,
        bool            rename,
        bool            stable,
        kfsSeq_t        version)
        : KfsOp(CMD_WRITE_CHUNKMETA, 0, o),
          chunkId(c),
          diskIo(d),
          dataBuf(),
          next(0),
          targetVersion(version),
          renameFlag(rename),
          stableFlag(stable)
    {
        SET_HANDLER(this, &WriteChunkMetaOp::HandleDone);
    }
    ~WriteChunkMetaOp() {
        delete diskIo;
    }
    void Execute() {}
    inline bool IsRenameNeeded(const ChunkInfoHandle* cih) const;
    bool IsWaiting() const {
        return (! diskIo && ! renameFlag);
    }
    int Start(ChunkInfoHandle* cih);
    string Show() const {
        ostringstream os;
        os << "write-chunk-meta: "
            " chunkid: " << chunkId <<
            " rename:  " << renameFlag <<
            " stable:  " << stableFlag <<
            " version: " << targetVersion
        ;
        return os.str();

    }
    // Notify the op that is waiting for the write to finish that all
    // is done
    int HandleDone(int code, void *data) {
        if (clnt) {
            clnt->HandleEvent(code, data);
        }
        delete this;
        return 0;
    }
};

/// Encapsulate a chunk file descriptor and information about the
/// chunk such as name and version #.
class ChunkInfoHandle : public KfsCallbackObj
{
public:
    typedef ChunkManager::ChunkLists   ChunkLists;
    typedef ChunkManager::ChunkDirInfo ChunkDirInfo;
    typedef ChunkDirInfo::ChunkLists   ChunkDirLists;

    ChunkInfoHandle(ChunkDirInfo& chunkdir, bool stableFlag = true)
        : KfsCallbackObj(),
          chunkInfo(),
          dataFH(),
          lastIOTime(0),
          readChunkMetaOp(0),
          isBeingReplicated(false),
          mDeleteFlag(false),
          mWriteAppenderOwnsFlag(false),
          mWaitForWritesInFlightFlag(false),
          mMetaDirtyFlag(false),
          mStableFlag(stableFlag),
          mInDoneHandlerFlag(false),
          mKeepFlag(false),
          mChunkList(ChunkManager::kChunkLruList),
          mChunkDirList(ChunkDirInfo::kChunkDirList),
          mRenamesInFlight(0),
          mWritesInFlight(0),
          mWriteMetaOpsHead(0),
          mWriteMetaOpsTail(0),
          mChunkDir(chunkdir)
    {
        ChunkList::Init(*this);
        ChunkDirList::Init(*this);
        ChunkDirList::PushBack(mChunkDir.chunkLists[mChunkDirList], *this);
        SET_HANDLER(this, &ChunkInfoHandle::HandleChunkMetaWriteDone);
        mChunkDir.chunkCount++;
        assert(mChunkDir.chunkCount > 0);
    }

    void Delete(ChunkLists* chunkInfoLists) {
        const bool evacuateFlag = IsEvacuate();
        ChunkList::Remove(chunkInfoLists[mChunkList], *this);
        DetachFromChunkDir(evacuateFlag);
        if (mWriteAppenderOwnsFlag) {
            mWriteAppenderOwnsFlag = false;
            gAtomicRecordAppendManager.DeleteChunk(chunkInfo.chunkId);
        }
        if (mWriteMetaOpsHead || mInDoneHandlerFlag) {
            mDeleteFlag = true;
            const bool runHanlder = ! mInDoneHandlerFlag &&
                mWritesInFlight > 0 && mWaitForWritesInFlightFlag;
            mWaitForWritesInFlightFlag = false;
            mWritesInFlight = 0;
            if (runHanlder) {
                int res = -1;
                HandleEvent(EVENT_DISK_ERROR, &res);
            }
        } else {
            delete this;
        }
    }

    bool IsEvacuate() const {
        return (! IsStale() &&
            mChunkDirList == ChunkDirInfo::kChunkDirEvacuateList);
    }

    bool SetEvacuate(bool flag) {
        if (IsStale()) {
            return false;
        }
        if (IsEvacuate() == flag) {
            return true;
        }
        mChunkDir.evacuateInFlightCount += (flag ? 1 : -1);
        if (mChunkDir.evacuateInFlightCount < 0) {
            mChunkDir.evacuateInFlightCount = 0;
        }
        ChunkDirList::Remove(mChunkDir.chunkLists[mChunkDirList], *this);
        mChunkDirList = flag ?
            ChunkDirInfo::kChunkDirEvacuateList :
            ChunkDirInfo::kChunkDirList;
        ChunkDirList::PushBack(mChunkDir.chunkLists[mChunkDirList], *this);
        return true;
    }

    ChunkInfo_t      chunkInfo;
    /// Chunks are stored as files in he underlying filesystem; each
    /// chunk file is named by the chunkId.  Each chunk has a header;
    /// this header is hidden from clients; all the client I/O is
    /// offset by the header amount
    DiskIo::FilePtr  dataFH;
    // when was the last I/O done on this chunk
    time_t           lastIOTime;
    /// keep track of the op that is doing the read
    ReadChunkMetaOp* readChunkMetaOp;

    void Release(ChunkLists* chunkInfoLists);

    bool IsFileOpen() const {
        return (dataFH && dataFH->IsOpen());
    }

    bool IsFileInUse() const {
        return (IsFileOpen() && ! dataFH.unique());
    }

    bool IsStable() const {
        return mStableFlag;
    }

    void StartWrite(WriteOp* /* op */) {
        assert(mWritesInFlight >= 0);
        mWritesInFlight++;
        mMetaDirtyFlag = true;
    }

    void SetMetaDirty() {
        mMetaDirtyFlag = true;
    }

    void WriteDone(const WriteOp* op = 0) {
        assert(mWritesInFlight > 0);
        mWritesInFlight--;
        if (mWritesInFlight == 0 && mWaitForWritesInFlightFlag) {
            assert(mWriteMetaOpsHead);
            mWaitForWritesInFlightFlag = false;
            int res = mWriteMetaOpsHead->Start(this);
            if (res < 0) {
                HandleEvent(EVENT_DISK_ERROR, &res);
            }
        }
    }

    bool IsFileEquals(const DiskIo::File* file) const {
        return (file && file == dataFH.get());
    }

    bool IsFileEquals(const DiskIo* diskIo) const {
        return (diskIo && IsFileEquals(diskIo->GetFilePtr().get()));
    }

    bool IsFileEquals(const DiskIoPtr& diskIoPtr) const {
        return IsFileEquals(diskIoPtr.get());
    }

    bool SyncMeta() {
        if (mWriteMetaOpsHead || mWritesInFlight > 0) {
            return true;
        }
        if (mMetaDirtyFlag) {
            WriteChunkMetadata();
            return true;
        }
        return false;
    }

    inline void LruUpdate(ChunkLists* chunkInfoLists);
    inline void SetWriteAppenderOwns(ChunkLists* chunkInfoLists, bool flag);
    inline bool IsWriteAppenderOwns() const;
    int WriteChunkMetadata(
        KfsCallbackObj* cb,
        bool            renameFlag,
        bool            stableFlag,
        kfsSeq_t        targetVersion);
    int WriteChunkMetadata(
        KfsCallbackObj* cb = 0)
    {
        return WriteChunkMetadata(cb, false, mStableFlag,
            mStableFlag ? chunkInfo.chunkVersion : kfsSeq_t(0));
    }
    kfsSeq_t GetTargetStateAndVersion(bool& stableFlag) const {
        if (! mWriteMetaOpsTail || mRenamesInFlight <= 0) {
            stableFlag = mStableFlag;
            return chunkInfo.chunkVersion;
        }
        if (mWriteMetaOpsTail->renameFlag) {
            stableFlag = mWriteMetaOpsTail->stableFlag;
            return mWriteMetaOpsTail->targetVersion;
        }
        stableFlag = mStableFlag;
        kfsSeq_t theRet = chunkInfo.chunkVersion;
        for (const WriteChunkMetaOp*
                op = mWriteMetaOpsHead; op; op = op->next) {
            if (op->renameFlag) {
                theRet = op->targetVersion;
                stableFlag = mWriteMetaOpsTail->stableFlag;
            }
        }
        return theRet;
    }
    bool CanHaveVersion(kfsSeq_t vers) const {
        if (vers == chunkInfo.chunkVersion) {
            return true;
        }
        for (const WriteChunkMetaOp*
                op = mWriteMetaOpsHead; op; op = op->next) {
            if (op->renameFlag && vers == op->targetVersion) {
                return true;
            }
        }
        return false;
    }
    bool IsChunkReadable() const {
        return (! mWriteMetaOpsHead && mStableFlag && mWritesInFlight <= 0);
    }
    bool IsRenameInFlight() const {
        return (mRenamesInFlight > 0);
    }
    bool HasWritesInFlight() const {
        return (mWritesInFlight > 0);
    }
    bool IsStale() const {
        return (mChunkList == ChunkManager::kChunkStaleList ||
            mChunkList == ChunkManager::kChunkPendingStaleList);
    }
    bool IsKeep() const {
        return mKeepFlag;
    }
    void MakeStale(ChunkLists* chunkInfoLists, bool keepFlag) {
        if (IsStale()) {
            return;
        }
        mKeepFlag = keepFlag;
        if (mWriteAppenderOwnsFlag) {
            mWriteAppenderOwnsFlag = false;
            gAtomicRecordAppendManager.DeleteChunk(chunkInfo.chunkId);
        }
        UpdateStale(chunkInfoLists);
        // Chunk is no longer in the chunk table, no further write ops
        // completion notification will get here. Clear write op counter and
        // restart the next op if needed.
        if (mWritesInFlight > 0) {
            mWritesInFlight = 1;
            WriteDone();
        }
    }
    void UpdateStale(ChunkLists* chunkInfoLists) {
        const bool evacuateFlag = IsEvacuate();
        ChunkList::Remove(chunkInfoLists[mChunkList], *this);
        mChunkList = mRenamesInFlight > 0 ?
            ChunkManager::kChunkPendingStaleList :
            ChunkManager::kChunkStaleList;
        ChunkList::PushBack(chunkInfoLists[mChunkList], *this);
        DetachFromChunkDir(evacuateFlag);
    }
    const string& GetDirname() const       { return mChunkDir.dirname; }
    const ChunkDirInfo& GetDirInfo() const { return mChunkDir; }
    ChunkDirInfo& GetDirInfo()             { return mChunkDir; }

    bool isBeingReplicated:1;  // is the chunk being replicated from
                               // another server
private:
    bool                        mDeleteFlag:1;
    bool                        mWriteAppenderOwnsFlag:1;
    bool                        mWaitForWritesInFlightFlag:1;
    bool                        mMetaDirtyFlag:1;
    bool                        mStableFlag:1;
    bool                        mInDoneHandlerFlag:1;
    bool                        mKeepFlag:1;
    ChunkManager::ChunkListType mChunkList:2;
    ChunkDirInfo::ChunkListType mChunkDirList:2;
    unsigned int                mRenamesInFlight:19;
    // Chunk meta data updates need to be executed in order, allow only one
    // write in flight.
    int                         mWritesInFlight;
    WriteChunkMetaOp*           mWriteMetaOpsHead;
    WriteChunkMetaOp*           mWriteMetaOpsTail;
    ChunkDirInfo&               mChunkDir;
    ChunkInfoHandle*            mPrevPtr[ChunkDirInfo::kChunkInfoHDirListCount];
    ChunkInfoHandle*            mNextPtr[ChunkDirInfo::kChunkInfoHDirListCount];

    void DetachFromChunkDir(bool evacuateFlag) {
        if (mChunkDirList == ChunkDirInfo::kChunkDirListNone) {
            return;
        }
        ChunkDirList::Remove(mChunkDir.chunkLists[mChunkDirList], *this);
        assert(mChunkDir.chunkCount > 0);
        mChunkDir.chunkCount--;
        mChunkDirList = ChunkDirInfo::kChunkDirListNone;
        if (evacuateFlag) {
            mChunkDir.ChunkEvacuateDone();
        }
    }

    int HandleChunkMetaWriteDone(int code, void *data);
    virtual ~ChunkInfoHandle() {
        if (mWriteMetaOpsHead) {
            // Object is the "client" of this op.
            die("attempt to delete chunk info handle "
                "with meta data write in flight");
        }
        if (IsFileOpen()) {
            globals().ctrOpenDiskFds.Update(-1);
        }
    }
    void UpdateState() {
        if (mInDoneHandlerFlag) {
            return;
        }
        if (mDeleteFlag || IsStale()) {
            if (! mWriteMetaOpsHead) {
                if (IsStale()) {
                    gChunkManager.UpdateStale(*this);
                } else {
                    delete this;
                }
            }
        } else {
            gChunkManager.LruUpdate(*this);
        }
    }
    friend class QCDLListOp<ChunkInfoHandle, 0>;
    friend class QCDLListOp<ChunkInfoHandle, 1>;
private:
    ChunkInfoHandle(const  ChunkInfoHandle&);
    ChunkInfoHandle& operator=(const  ChunkInfoHandle&);
};

inline bool ChunkManager::IsInLru(const ChunkInfoHandle& cih) const {
    return (! cih.IsStale() &&
        ChunkList::IsInList(mChunkInfoLists[kChunkLruList], cih));
}

inline void ChunkInfoHandle::LruUpdate(ChunkInfoHandle::ChunkLists* chunkInfoLists) {
    if (IsStale()) {
        return;
    }
    lastIOTime = globalNetManager().Now();
    if (! mWriteAppenderOwnsFlag && ! isBeingReplicated && ! mWriteMetaOpsHead) {
        ChunkList::PushBack(chunkInfoLists[mChunkList], *this);
        assert(gChunkManager.IsInLru(*this));
    } else {
        ChunkList::Remove(chunkInfoLists[mChunkList], *this);
        assert(! gChunkManager.IsInLru(*this));
    }
}

inline void ChunkInfoHandle::SetWriteAppenderOwns(ChunkInfoHandle::ChunkLists* chunkInfoLists, bool flag) {
    if (mDeleteFlag || IsStale() || flag == mWriteAppenderOwnsFlag) {
        return;
    }
    mWriteAppenderOwnsFlag = flag;
    if (mWriteAppenderOwnsFlag) {
        ChunkList::Remove(chunkInfoLists[mChunkList], *this);
        assert(! gChunkManager.IsInLru(*this));
    } else {
        LruUpdate(chunkInfoLists);
    }
}

inline bool ChunkInfoHandle::IsWriteAppenderOwns() const
{
    return mWriteAppenderOwnsFlag;
}

inline void ChunkManager::LruUpdate(ChunkInfoHandle& cih) {
    cih.LruUpdate(mChunkInfoLists);
}

inline void ChunkManager::Release(ChunkInfoHandle& cih) {
    cih.Release(mChunkInfoLists);
}

inline void ChunkManager::Delete(ChunkInfoHandle& cih) {
    if (! cih.IsStale() && ! mPendingWrites.Delete(
            cih.chunkInfo.chunkId, cih.chunkInfo.chunkVersion)) {
        ostringstream os;
        os << "delete failed to cleanup pending writes: "
            " chunk: "   << cih.chunkInfo.chunkId <<
            " version: " << cih.chunkInfo.chunkVersion
        ;
        die(os.str());
    }
    cih.Delete(mChunkInfoLists);
}

inline void ChunkManager::UpdateStale(ChunkInfoHandle& cih) {
    assert(cih.IsStale());
    cih.UpdateStale(mChunkInfoLists);
    RunStaleChunksQueue();
}

void
ChunkInfoHandle::Release(ChunkInfoHandle::ChunkLists* chunkInfoLists)
{
    chunkInfo.UnloadChecksums();
    if (! IsFileOpen()) {
        return;
    }
    string errMsg;
    if (! dataFH->Close(
            chunkInfo.chunkSize + KFS_CHUNK_HEADER_SIZE,
            &errMsg)) {
        KFS_LOG_STREAM_INFO <<
            "chunk " << chunkInfo.chunkId << " close error: " << errMsg <<
        KFS_LOG_EOM;
        dataFH.reset();
    }
    KFS_LOG_STREAM_INFO <<
        "Closing chunk " << chunkInfo.chunkId << " and might give up lease" <<
    KFS_LOG_EOM;
    gLeaseClerk.RelinquishLease(chunkInfo.chunkId, chunkInfo.chunkSize);

    ChunkList::Remove(chunkInfoLists[mChunkList], *this);
    globals().ctrOpenDiskFds.Update(-1);
}

inline bool
WriteChunkMetaOp::IsRenameNeeded(const ChunkInfoHandle* cih) const
{
    return (
        renameFlag &&
        ((cih->IsStable() && cih->chunkInfo.chunkVersion != targetVersion) ||
        cih->IsStable() != stableFlag)
    );
}

int
WriteChunkMetaOp::Start(ChunkInfoHandle* cih)
{
    gChunkManager.LruUpdate(*cih);
    if (renameFlag) {
        if (! IsRenameNeeded(cih)) {
            int64_t res = 0;
            cih->HandleEvent(EVENT_DISK_RENAME_DONE, &res);
            return 0;
        }
        if (! DiskIo::Rename(
                gChunkManager.MakeChunkPathname(cih).c_str(),
                gChunkManager.MakeChunkPathname(
                    cih, stableFlag, targetVersion).c_str(),
                cih,
                &statusMsg)) {
            status = -EAGAIN;
            KFS_LOG_STREAM_ERROR <<
                Show() << " failed: " << statusMsg <<
            KFS_LOG_EOM;
        }
    } else {
        assert(diskIo);
        status = diskIo->Write(0, dataBuf.BytesConsumable(), &dataBuf);
    }
    return status;
}

int
ChunkInfoHandle::WriteChunkMetadata(
    KfsCallbackObj* cb,
    bool            renameFlag,
    bool            stableFlag,
    kfsSeq_t        targetVersion)
{
    if (renameFlag && (int)mRenamesInFlight + 1 <= 0) {
        // Overflow: too many renames in flight.
        return -ESERVERBUSY;
    }
    // If chunk is not stable and is not transitioning into stable, and there
    // are no pending ops, just assign the version and mark meta dirty.
    if (targetVersion > 0 && chunkInfo.chunkVersion != targetVersion &&
            mWritesInFlight <= 0 &&
            ! IsStable() && ! stableFlag && ! mWriteMetaOpsTail &&
            ! mInDoneHandlerFlag && IsFileOpen() &&
            ! mDeleteFlag && ! IsStale()) {
        mMetaDirtyFlag         = true;
        chunkInfo.chunkVersion = targetVersion;
        if (cb) {
            int res = 0;
            cb->HandleEvent(renameFlag ?
                EVENT_DISK_RENAME_DONE : EVENT_DISK_WROTE, &res);
        }
        UpdateState();
        return 0;
    }
    if (renameFlag) {
        // Queue the version update first, then immediately queue rename.
        // Not stable chunks on disk always have version 0.
        mMetaDirtyFlag = true;
        const int ret = WriteChunkMetadata(
            0, false, stableFlag, stableFlag ? targetVersion : kfsSeq_t(0));
        if (ret != 0) {
            return ret;
        }
    }
    DiskIo* d = 0;
    if (! renameFlag) {
        if (! mMetaDirtyFlag) {
            if (! cb) {
                return 0;
            }
            if (! mWriteMetaOpsTail) {
                assert(mRenamesInFlight <= 0);
                int res = 0;
                cb->HandleEvent(EVENT_DISK_WROTE, &res);
                UpdateState();
                return 0;
            }
        }
        if (mMetaDirtyFlag) {
            d = gChunkManager.SetupDiskIo(this, this);
            if (! d) {
                return -ESERVERBUSY;
            }
            mMetaDirtyFlag = false;
        } else {
            // Add to pending meta op to completion queue.
            assert(mWriteMetaOpsTail);
        }
    }
    WriteChunkMetaOp* const wcm = new WriteChunkMetaOp(chunkInfo.chunkId,
        cb, d, renameFlag, stableFlag, targetVersion);
    if (d) {
        const kfsSeq_t prevVersion = chunkInfo.chunkVersion;
        chunkInfo.chunkVersion = targetVersion;
        chunkInfo.Serialize(&wcm->dataBuf);
        chunkInfo.chunkVersion = prevVersion;
        const uint64_t checksum =
            ComputeBlockChecksum(&wcm->dataBuf, wcm->dataBuf.BytesConsumable());
        wcm->dataBuf.CopyIn(
            reinterpret_cast<const char*>(&checksum), (int)sizeof(checksum));
        wcm->dataBuf.ZeroFillLast();
        if ((int)KFS_CHUNK_HEADER_SIZE < wcm->dataBuf.BytesConsumable()) {
            die("invalid io buffer size");
        }
    }
    if (wcm->renameFlag) {
        mRenamesInFlight++;
        assert(mRenamesInFlight > 0);
    }
    if (mWriteMetaOpsTail) {
        assert(mWriteMetaOpsHead);
        while (mWriteMetaOpsTail->next) {
            mWriteMetaOpsTail = mWriteMetaOpsTail->next;
        }
        mWriteMetaOpsTail->next = wcm;
        mWriteMetaOpsTail = wcm;
        return 0;
    }
    assert(! mWriteMetaOpsHead);
    mWriteMetaOpsHead = wcm;
    mWriteMetaOpsTail = wcm;
    if (mWritesInFlight > 0) {
        mWaitForWritesInFlightFlag = true;
        return 0;
    }
    const int res = wcm->Start(this);
    if (res < 0) {
        mWriteMetaOpsHead = 0;
        mWriteMetaOpsTail = 0;
        delete wcm;
    }
    return (res >= 0 ? 0 : res);
}

int
ChunkInfoHandle::HandleChunkMetaWriteDone(int codeIn, void *dataIn)
{
    const bool prevInDoneHandlerFlag = mInDoneHandlerFlag;
    mInDoneHandlerFlag = true;
    int64_t res;
    int     err;
    int     code = codeIn;
    void*   data = dataIn;
    // Do not rely on compiler to unroll tail recursion, use loop.
    for (; ;) {
        assert(mWriteMetaOpsHead);
        int status = data ? *reinterpret_cast<const int*>(data) : -1;
        if (code == EVENT_DISK_ERROR && status >= 0) {
            status = -1;
        }
        if ((! mDeleteFlag && ! IsStale()) && status < 0) {
            KFS_LOG_STREAM_ERROR << mWriteMetaOpsHead->Show() <<
                " failed: status: " << status <<
                " op: status: "     << mWriteMetaOpsHead->status <<
                " msg: "            << mWriteMetaOpsHead->statusMsg <<
            KFS_LOG_EOM;
            if (! isBeingReplicated) {
                gChunkManager.ChunkIOFailed(this, status);
            }
        }
        if (mWriteMetaOpsHead->status >= 0) {
            mWriteMetaOpsHead->status = status;
        }
        if (mWriteMetaOpsHead->renameFlag) {
            assert(mRenamesInFlight > 0);
            mRenamesInFlight--;
            if (mWriteMetaOpsHead->status == 0) {
                if (code != EVENT_DISK_RENAME_DONE) {
                    ostringstream os;
                    os << "chunk meta write completion:"
                        " unexpected event code: " << code;
                    die(os.str());
                }
                mStableFlag = mWriteMetaOpsHead->stableFlag;
                chunkInfo.chunkVersion = mWriteMetaOpsHead->targetVersion;
                if (mStableFlag) {
                    mWriteAppenderOwnsFlag = false;
                    // LruUpdate below will add it back to the lru list.
                }
            }
        }
        WriteChunkMetaOp* const cur = mWriteMetaOpsHead;
        mWriteMetaOpsHead = cur->next;
        const bool doneFlag = ! mWriteMetaOpsHead;
        if (doneFlag) {
            mWriteMetaOpsTail = 0;
        }
        cur->HandleEvent(code, data);
        if (doneFlag) {
            break;
        }
        if (mWriteMetaOpsHead->IsWaiting()) {
            // Call the completion, this op was waiting for the one that
            // just completed.
            continue;
        }
        if (mWritesInFlight > 0) {
            mWaitForWritesInFlightFlag = true;
            break;
        }
        if (mWriteMetaOpsHead->renameFlag &&
                ! mWriteMetaOpsHead->IsRenameNeeded(this)) {
            res = 0;
            data = &res;
            code = EVENT_DISK_RENAME_DONE;
            continue;
        }
        if (mDeleteFlag || IsStale()) {
            err = -EBADF;
        } else if ((err = mWriteMetaOpsHead->Start(this)) >= 0) {
            break;
        }
        data = &err;
        code = EVENT_DISK_ERROR;
    }
    mInDoneHandlerFlag = prevInDoneHandlerFlag;
    UpdateState();
    return 0;
}

static int
GetMaxOpenFds()
{
    struct rlimit rlim;
    int maxOpenFds = 0;

    if (getrlimit(RLIMIT_NOFILE, &rlim) == 0) {
        maxOpenFds = rlim.rlim_cur;
        // bump the soft limit to the hard limit
        rlim.rlim_cur = rlim.rlim_max;
        if (setrlimit(RLIMIT_NOFILE, &rlim) == 0) {
            maxOpenFds = rlim.rlim_cur;
        }
    }
    KFS_LOG_STREAM_INFO <<
        "max # of open files: " << maxOpenFds <<
    KFS_LOG_EOM;
    return maxOpenFds;
}

// Chunk manager implementation.
ChunkManager::ChunkManager()
    : mMaxPendingWriteLruSecs(300),
      mCheckpointIntervalSecs(120),
      mTotalSpace(int64_t(1) << 62),
      mUsedSpace(0),
      mMinFsAvailableSpace((int64_t)(CHUNKSIZE + KFS_CHUNK_HEADER_SIZE)),
      mMaxSpaceUtilizationThreshold(0.05),
      mNextCheckpointTime(0),
      mMaxOpenChunkFiles((64 << 10) - 8),
      mMaxOpenFds(1 << 10),
      mFdsPerChunk(1),
      mChunkDirs(),
      mWriteId(GetRandomSeq()), // Seed write id.
      mPendingWrites(),
      mChunkTable(),
      mMaxIORequestSize(4 << 20),
      mNextChunkDirsCheckTime(globalNetManager().Now() - 1),
      mChunkDirsCheckIntervalSecs(120),
      mNextGetFsSpaceAvailableTime(globalNetManager().Now() - 1),
      mGetFsSpaceAvailableIntervalSecs(25),
      mInactiveFdsCleanupIntervalSecs(300),
      mNextInactiveFdCleanupTime(0),
      mReadChecksumMismatchMaxRetryCount(0),
      mAbortOnChecksumMismatchFlag(false),
      mRequireChunkHeaderChecksumFlag(false),
      mForceDeleteStaleChunksFlag(false),
      mKeepEvacuatedChunksFlag(false),
      mStaleChunkCompletion(*this),
      mStaleChunkOpsInFlight(0),
      mMaxStaleChunkOpsInFlight(4),
      mMaxDirCheckDiskTimeouts(4),
      mChunkPlacementPendingReadWeight(0),
      mChunkPlacementPendingWriteWeight(0),
      mMaxPlacementSpaceRatio(0.2),
      mMinPendingIoThreshold(8 << 20),
      mAllowSparseChunksFlag(true),
      mBufferedIoFlag(false),
      mNullBlockChecksum(0),
      mCounters(),
      mDirChecker(),
      mCleanupChunkDirsFlag(true),
      mStaleChunksDir("lost+found"),
      mDirtyChunksDir("dirty"),
      mEvacuateFileName("evacuate"),
      mEvacuateDoneFileName(mEvacuateFileName + ".done"),
      mChunkDirLockName("lock"),
      mEvacuationInactivityTimeout(300),
      mMetaHeartbeatTime(globalNetManager().Now() - 365 * 24 * 60 * 60),
      mMetaEvacuateCount(-1),
      mMaxEvacuateIoErrors(2),
      mChunkHeaderBuffer(reinterpret_cast<char*>(&mChunkHeaderBufferAlloc))
{
    mDirChecker.SetInterval(180);
    srand48((long)globalNetManager().Now());
    for (int i = 0; i < kChunkInfoListCount; i++) {
        ChunkList::Init(mChunkInfoLists[i]);
    }
    globalNetManager().SetMaxAcceptsPerRead(4096);
}

ChunkManager::~ChunkManager()
{
    assert(mChunkTable.IsEmpty());
    globalNetManager().UnRegisterTimeoutHandler(this);
}

void
ChunkManager::Shutdown()
{
    mDirChecker.Stop();
    // Run delete queue before removing chunk table entries.
    RunStaleChunksQueue();
    for (int i = 0; ;) {
        const bool completionFlag = DiskIo::RunIoCompletion();
        if (mStaleChunkOpsInFlight <= 0) {
            break;
        }
        if (completionFlag) {
            continue;
        }
        if (++i > 1000) {
            KFS_LOG_STREAM_ERROR <<
                "ChunkManager::Shutdown pending delete timeout exceeded" <<
            KFS_LOG_EOM;
            ChunkList::Iterator it(mChunkInfoLists[kChunkStaleList]);
            ChunkInfoHandle* cih;
            while ((cih = it.Next())) {
                Delete(*cih);
            }
            break;
        }
        usleep(10000);
    }

    ScavengePendingWrites(time(0) + 2 * mMaxPendingWriteLruSecs);
    CMap tmp;
    const CMapEntry* p;
    mChunkTable.First();
    while ((p = mChunkTable.Next())) {
        ChunkInfoHandle* const cih = p->GetVal();
        if (cih->IsFileInUse()) {
            cih->SetWriteAppenderOwns(mChunkInfoLists, false);
            bool newEntryFlag = true;
            tmp.Insert(p->GetKey(), cih, newEntryFlag);
            continue;
        }
        Release(*cih);
        Delete(*cih);
    }
    mChunkTable.Clear();
    mChunkTable.Swap(tmp);
    gAtomicRecordAppendManager.Shutdown();
    for (int i = 0; ;) {
        mChunkTable.First();
        while ((p = mChunkTable.Next())) {
            ChunkInfoHandle* const cih = p->GetVal();
            if (! cih) {
                mChunkTable.Erase(p->GetKey());
                continue;
            }
            if (cih->IsFileInUse()) {
                break;
            }
            mChunkTable.Erase(p->GetKey());
            Release(*cih);
            Delete(*cih);
        }
        const bool completionFlag = DiskIo::RunIoCompletion();
        if (mChunkTable.IsEmpty()) {
            break;
        }
        if (completionFlag) {
            continue;
        }
        if (++i > 1000) {
            KFS_LOG_STREAM_ERROR <<
                "ChunkManager::Shutdown timeout exceeded" <<
            KFS_LOG_EOM;
            break;
        }
        usleep(10000);
    }
    globalNetManager().UnRegisterTimeoutHandler(this);
    string errMsg;
    if (! DiskIo::Shutdown(&errMsg)) {
        KFS_LOG_STREAM_INFO <<
            "DiskIo::Shutdown failure: " << errMsg <<
        KFS_LOG_EOM;
    }
}

bool
ChunkManager::IsWriteAppenderOwns(kfsChunkId_t chunkId) const
{
    ChunkInfoHandle** const ci = mChunkTable.Find(chunkId);
    return (ci && (*ci)->IsWriteAppenderOwns());
}

void
ChunkManager::SetParameters(const Properties& prop)
{
    mInactiveFdsCleanupIntervalSecs = prop.getValue(
        "chunkServer.inactiveFdsCleanupIntervalSecs",
        mInactiveFdsCleanupIntervalSecs);
    mMaxPendingWriteLruSecs = max(1, prop.getValue(
        "chunkServer.maxPendingWriteLruSecs",
        mMaxPendingWriteLruSecs));
    mCheckpointIntervalSecs = max(1, prop.getValue(
        "chunkServer.checkpointIntervalSecs",
        mCheckpointIntervalSecs));
    mChunkDirsCheckIntervalSecs = max(1, prop.getValue(
        "chunkServer.chunkDirsCheckIntervalSecs",
        mChunkDirsCheckIntervalSecs));
    mGetFsSpaceAvailableIntervalSecs = max(1, prop.getValue(
        "chunkServer.getFsSpaceAvailableIntervalSecs",
        mGetFsSpaceAvailableIntervalSecs));
    mAbortOnChecksumMismatchFlag = prop.getValue(
        "chunkServer.abortOnChecksumMismatchFlag",
        mAbortOnChecksumMismatchFlag ? 1 : 0) != 0;
    mReadChecksumMismatchMaxRetryCount = prop.getValue(
        "chunkServer.readChecksumMismatchMaxRetryCount",
        mReadChecksumMismatchMaxRetryCount);
    mRequireChunkHeaderChecksumFlag = prop.getValue(
        "chunkServer.requireChunkHeaderChecksum",
        mRequireChunkHeaderChecksumFlag ? 1 : 0) != 0;
    mForceDeleteStaleChunksFlag = prop.getValue(
        "chunkServer.forceDeleteStaleChunks",
        mForceDeleteStaleChunksFlag ? 1 : 0) != 0;
    mKeepEvacuatedChunksFlag = prop.getValue(
        "chunkServer.keepEvacuatedChunksFlag",
        mKeepEvacuatedChunksFlag ? 1 : 0) != 0;
    mMaxStaleChunkOpsInFlight = prop.getValue(
        "chunkServer.maxStaleChunkOpsInFlight",
        mMaxStaleChunkOpsInFlight);
    mMaxDirCheckDiskTimeouts = prop.getValue(
        "chunkServer.maxDirCheckDiskTimeouts",
        mMaxDirCheckDiskTimeouts);
    mTotalSpace = prop.getValue(
        "chunkServer.totalSpace",
        mTotalSpace);
    mMinFsAvailableSpace = max(int64_t(CHUNKSIZE + KFS_CHUNK_HEADER_SIZE),
        prop.getValue(
            "chunkServer.minFsAvailableSpace",
            mMinFsAvailableSpace));
    mMaxSpaceUtilizationThreshold = prop.getValue(
        "chunkServer.maxSpaceUtilizationThreshold",
        mMaxSpaceUtilizationThreshold);
    mChunkPlacementPendingReadWeight = prop.getValue(
        "chunkServer.chunkPlacementPendingReadWeight",
        mChunkPlacementPendingReadWeight);
    mChunkPlacementPendingWriteWeight = prop.getValue(
        "chunkServer.chunkPlacementPendingWriteWeight",
        mChunkPlacementPendingWriteWeight);
    mMinPendingIoThreshold = prop.getValue(
        "chunkServer.minPendingIoThreshold",
        mMinPendingIoThreshold);
    mMaxPlacementSpaceRatio = prop.getValue(
        "chunkServer.maxPlacementSpaceRatio",
        mMaxPlacementSpaceRatio);
    mAllowSparseChunksFlag = prop.getValue(
        "chunkServer.allowSparseChunks",
        mAllowSparseChunksFlag ? 1 : 0) != 0;
    mBufferedIoFlag = prop.getValue(
        "chunkServer.bufferedIo",
        mBufferedIoFlag ? 1 : 0) != 0;
    mEvacuateFileName = prop.getValue(
        "chunkServer.evacuateFileName",
        mEvacuateFileName);
    mEvacuateDoneFileName = prop.getValue(
        "chunkServer.evacuateDoneFileName",
        mEvacuateDoneFileName);
    mEvacuationInactivityTimeout = prop.getValue(
        "chunkServer.evacuationInactivityTimeout",
        mEvacuationInactivityTimeout);
    mDirChecker.SetInterval(prop.getValue(
        "chunkServer.dirRecheckInterval",
        mDirChecker.GetInterval() / 1000) * 1000);
    mCleanupChunkDirsFlag = prop.getValue(
        "chunkServer.cleanupChunkDirs",
        mCleanupChunkDirsFlag);
    mDirChecker.SetRemoveFilesFlag(mCleanupChunkDirsFlag);

    TcpSocket::SetDefaultRecvBufSize(prop.getValue(
        "chunkServer.tcpSocket.recvBufSize",
        TcpSocket::GetDefaultRecvBufSize()));
    TcpSocket::SetDefaultSendBufSize(prop.getValue(
        "chunkServer.tcpSocket.sendBufSize",
        TcpSocket::GetDefaultSendBufSize()));

    globalNetManager().SetMaxAcceptsPerRead(prop.getValue(
        "chunkServer.net.maxAcceptsPerRead",
        globalNetManager().GetMaxAcceptsPerRead()));

    DiskIo::SetParameters(prop);
    Replicator::SetParameters(prop);

    gClientManager.SetTimeouts(
        prop.getValue("chunkServer.client.ioTimeoutSec",    5 * 60),
        prop.getValue("chunkServer.client.idleTimeoutSec", 10 * 60)
    );
    RemoteSyncSM::SetResponseTimeoutSec(
        prop.getValue("chunkServer.remoteSync.responseTimeoutSec",
            RemoteSyncSM::GetResponseTimeoutSec())
    );
    RemoteSyncSM::SetTraceRequestResponse(
        prop.getValue("chunkServer.remoteSync.traceRequestResponse", false)
    );
    mMaxEvacuateIoErrors = max(1, prop.getValue(
        "chunkServer.maxEvacuateIoErrors",
        mMaxEvacuateIoErrors
    ));

    DirChecker::FileNames excludes;
    excludes.insert(mEvacuateDoneFileName);
    mDirChecker.SetDontUseIfExist(excludes);
    gAtomicRecordAppendManager.SetParameters(prop);

    const time_t now = globalNetManager().Now();
    mNextGetFsSpaceAvailableTime = min(mNextGetFsSpaceAvailableTime,
        now + mGetFsSpaceAvailableIntervalSecs);
    mNextChunkDirsCheckTime = min(mNextChunkDirsCheckTime,
        now + mChunkDirsCheckIntervalSecs);
}

static string AddTrailingPathSeparator(const string& dir)
{
    return ((! dir.empty() && dir[dir.length() - 1] != '/') ?
        dir + "/" : dir);
}

struct EqualPrefixStr : public binary_function<string, string, bool>
{
    bool operator()(const string& x, const string& y) const
    {
        return x.compare(0, min(x.length(), y.length()), y) == 0;
    }
};

bool
ChunkManager::Init(const vector<string>& chunkDirs, const Properties& prop)
{
    if (chunkDirs.empty()) {
        KFS_LOG_STREAM_ERROR <<
            "no chunk directories specified" <<
        KFS_LOG_EOM;
        return false;
    }

    // allow to change dir names only before io starts.
    mStaleChunksDir = prop.getValue(
        "chunkServer.staleChunksDir",
        mStaleChunksDir);
    mDirtyChunksDir = prop.getValue(
        "chunkServer.dirtyChunksDir",
        mDirtyChunksDir);
    mChunkDirLockName = prop.getValue(
        "chunkServer.dirLockFileName",
        mChunkDirLockName);
    if (mStaleChunksDir.empty()) {
        KFS_LOG_STREAM_ERROR <<
            "invalid stale chunks dir name: " << mStaleChunksDir <<
        KFS_LOG_EOM;
        return false;
    }
    if (mDirtyChunksDir.empty()) {
        KFS_LOG_STREAM_ERROR <<
            "invalid stale chunks dir name: " << mDirtyChunksDir <<
        KFS_LOG_EOM;
        return false;
    }
    mStaleChunksDir = AddTrailingPathSeparator(mStaleChunksDir);
    mDirtyChunksDir = AddTrailingPathSeparator(mDirtyChunksDir);

    SetParameters(prop);

    // Normalize tailing /, and keep only longest prefixes:
    // only leave leaf directories.
    vector<string> dirs;
    dirs.reserve(chunkDirs.size());
    for (vector<string>::const_iterator it = chunkDirs.begin();
            it < chunkDirs.end();
            ++it) {
        if (it->empty()) {
            continue;
        }
        string dir = *it;
        size_t pos = dir.length();
        while (pos > 1 && dir[pos - 1] == '/') {
            --pos;
        }
        if (++pos < dir.length()) {
            dir.erase(pos);
        }
        dirs.push_back(AddTrailingPathSeparator(dir));
    }
    sort(dirs.begin(), dirs.end(), greater<string>());
    size_t cnt = unique(dirs.begin(), dirs.end(), EqualPrefixStr()) -
        dirs.begin();
    mChunkDirs.Allocate(cnt);
    vector<string>::const_iterator di = dirs.begin();
    for (ChunkDirs::iterator it = mChunkDirs.begin();
            it < mChunkDirs.end();
            ++it, ++di) {
        it->dirname = *di;
    }

    string errMsg;
    if (! DiskIo::Init(prop, &errMsg)) {
        KFS_LOG_STREAM_ERROR <<
            "DiskIo::Init failure: " << errMsg <<
        KFS_LOG_EOM;
        return false;
    }
    const int kMinOpenFds = 32;
    mMaxOpenFds = GetMaxOpenFds();
    if (mMaxOpenFds < kMinOpenFds) {
        KFS_LOG_STREAM_ERROR <<
            "file descriptor limit too small: " << mMaxOpenFds <<
        KFS_LOG_EOM;
        return false;
    }
    mFdsPerChunk = DiskIo::GetFdCountPerFile();
    if (mFdsPerChunk < 1) {
        KFS_LOG_STREAM_ERROR <<
            "invalid fd count per chunk: " << mFdsPerChunk <<
        KFS_LOG_EOM;
        return false;
    }
    mMaxOpenChunkFiles = min((mMaxOpenFds - kMinOpenFds / 2) / mFdsPerChunk,
        prop.getValue("chunkServer.maxOpenChunkFiles", mMaxOpenChunkFiles));
    if (mMaxOpenChunkFiles < kMinOpenFds / 2) {
        KFS_LOG_STREAM_ERROR <<
            "open chunks limit too small: " << mMaxOpenChunkFiles <<
        KFS_LOG_EOM;
        return false;
    }
    {
        IOBuffer buf;
        buf.ZeroFill((int)CHECKSUM_BLOCKSIZE);
        mNullBlockChecksum = ComputeBlockChecksum(&buf, buf.BytesConsumable());
    }
    // force a stat of the dirs and update space usage counts
    return StartDiskIo();
}

int
ChunkManager::AllocChunk(
    kfsFileId_t       fileId,
    kfsChunkId_t      chunkId,
    kfsSeq_t          chunkVersion,
    bool              isBeingReplicated,
    ChunkInfoHandle** outCih,
    bool              mustExistFlag /* = false */)
{
    ChunkInfoHandle** const cie = mChunkTable.Find(chunkId);
    if (cie) {
        if (isBeingReplicated) {
            return -EINVAL;
        }
        ChunkInfoHandle* const cih = *cie;
        if (cih->isBeingReplicated || cih->IsStable() ||
                cih->IsWriteAppenderOwns() ||
                cih->chunkInfo.chunkVersion != chunkVersion) {
            return -EINVAL;
        }
        if (outCih) {
            *outCih = cih;
        }
        return 0;
    } else if (mustExistFlag) {
        return -EBADF;
    }

    // Find the directory to use
    ChunkDirInfo* const chunkdir = GetDirForChunk();
    if (! chunkdir) {
        KFS_LOG_STREAM_INFO <<
            "no directory has space to host chunk " << chunkId <<
        KFS_LOG_EOM;
        return -ENOSPC;
    }

    // Chunks are dirty until they are made stable: A chunk becomes
    // stable when the write lease on the chunk expires and the
    // metaserver says the chunk is now stable.  Dirty chunks are
    // stored in a "dirty" dir; chunks in this dir will get nuked
    // on a chunkserver restart.  This provides a very simple failure
    // handling model.

    CleanupInactiveFds();

    const bool stableFlag = false;
    ChunkInfoHandle* const cih = new ChunkInfoHandle(*chunkdir, stableFlag);
    cih->chunkInfo.Init(fileId, chunkId, chunkVersion);
    cih->isBeingReplicated = isBeingReplicated;
    cih->SetMetaDirty();
    bool newEntryFlag = false;
    if (! mChunkTable.Insert(chunkId, cih, newEntryFlag) || ! newEntryFlag) {
        die("chunk insertion failure");
        cih->Delete(mChunkInfoLists);
        return -EFAULT;
    }
    KFS_LOG_STREAM_INFO << "Creating chunk: " << MakeChunkPathname(cih) <<
    KFS_LOG_EOM;
    int ret = OpenChunk(cih, O_RDWR | O_CREAT);
    if (ret < 0) {
        // open chunk failed: the entry in the chunk table is cleared and
        // Delete(*cih) is also called in OpenChunk().  Return the
        // error code
        return ret;
    }
    if (outCih) {
        *outCih = cih;
    }
    return ret;
}

void
ChunkManager::AllocChunkForAppend(
    AllocChunkOp* op, int replicationPos, ServerLocation peerLoc)
{
    if (IsWritePending(op->chunkId)) {
        op->statusMsg = "random write in progress";
        op->status = -EINVAL;
    }
    ChunkInfoHandle *cih = 0;
    op->status = AllocChunk(
        op->fileId, op->chunkId, op->chunkVersion, false, &cih,
        op->mustExistFlag);
    if (op->status != 0) {
        return;
    }
    assert(cih);
    gAtomicRecordAppendManager.AllocateChunk(
        op, replicationPos, peerLoc, cih->dataFH);
    if (op->status == 0) {
        cih->SetWriteAppenderOwns(mChunkInfoLists, true);
    }
}

bool
ChunkManager::IsChunkStable(const ChunkInfoHandle* cih) const
{
    return (
        cih->IsStable() &&
        (! cih->IsWriteAppenderOwns() ||
            gAtomicRecordAppendManager.IsChunkStable(cih->chunkInfo.chunkId)) &&
        ! IsWritePending(cih->chunkInfo.chunkId) &&
        ! cih->isBeingReplicated
    );
}

bool
ChunkManager::IsChunkStable(kfsChunkId_t chunkId) const
{
    ChunkInfoHandle** const ci = mChunkTable.Find(chunkId);
    return (! ci || IsChunkStable(*ci));
}

bool
ChunkManager::IsChunkReadable(kfsChunkId_t chunkId) const
{
    ChunkInfoHandle** const ci = mChunkTable.Find(chunkId);
    return (! ci || (IsChunkStable(*ci) && (*ci)->IsChunkReadable()));
}

bool
ChunkManager::IsChunkStable(MakeChunkStableOp* op)
{
    if (op->hasChecksum) {
        return false; // Have to run make stable to compare the checksum.
    }
    ChunkInfoHandle** const ci = mChunkTable.Find(op->chunkId);
    if (! ci) {
        op->statusMsg = "no such chunk";
        op->status    = -EBADF;
        return true;
    }
    // See if it have to wait until the chunk becomes readable.
    ChunkInfoHandle* const cih = *ci;
    return (op->chunkVersion == cih->chunkInfo.chunkVersion &&
        IsChunkStable(cih) && cih->IsChunkReadable());
}

int
ChunkManager::MakeChunkStable(kfsChunkId_t chunkId, kfsSeq_t chunkVersion,
    bool appendFlag, KfsCallbackObj* cb, string& statusMsg)
{
    ChunkInfoHandle** const ci = mChunkTable.Find(chunkId);
    if (! ci) {
        statusMsg = "no such chunk";
        return -EBADF;
    }
    ChunkInfoHandle* const cih = *ci;
    assert(cih);
    bool stableFlag = false;
    if (cih->IsRenameInFlight()) {
        if (chunkVersion != cih->GetTargetStateAndVersion(stableFlag)) {
            statusMsg = (stableFlag ? "" : "not ");
            statusMsg += "stable target version mismatch";
            return -EINVAL;
        }
    } else if (chunkVersion != cih->chunkInfo.chunkVersion) {
        statusMsg = "version mismatch";
        return -EINVAL;
    }
    if (cih->isBeingReplicated) {
        statusMsg = "chunk replication is in progress";
        return -EINVAL;
    }
    if (! cih->chunkInfo.chunkBlockChecksum) {
        statusMsg = "checksum are not loaded";
        return -EAGAIN;
    }
    if ((appendFlag ?
            ! cih->IsWriteAppenderOwns() :
            (cih->IsWriteAppenderOwns() &&
                ! gAtomicRecordAppendManager.IsChunkStable(chunkId)))) {
        ostringstream os;
        os << "make stable invalid state: "
            " chunk: "        << chunkId <<
            " version: " << cih->chunkInfo.chunkVersion <<
                "/" << chunkVersion <<
            " append: "       << appendFlag <<
            " appender owns:" << cih->IsWriteAppenderOwns()
        ;
        die(os.str());
    }
    if (! mPendingWrites.Delete(chunkId, cih->chunkInfo.chunkVersion)) {
        ostringstream os;
        os << "make stable failed to cleanup pending writes: "
            " chunk: "   << chunkId <<
            " version: " << cih->chunkInfo.chunkVersion
        ;
        die(os.str());
    }
    stableFlag = true;
    const bool renameFlag = true;
    const int  res        = cih->WriteChunkMetadata(
        cb, renameFlag, stableFlag, cih->chunkInfo.chunkVersion);
    if (res < 0) {
        statusMsg = "failed to start chunk meta data write";
    }
    return res;
}

int
ChunkManager::DeleteChunk(kfsChunkId_t chunkId)
{
    ChunkInfoHandle** const ci = mChunkTable.Find(chunkId);
    if (! ci) {
        return -EBADF;
    }
    KFS_LOG_STREAM_INFO << "deleting chunk: " << chunkId <<
    KFS_LOG_EOM;
    const bool forceDeleteFlag = true;
    return StaleChunk(*ci, forceDeleteFlag);
}

void
ChunkManager::DumpChunkMap()
{
    ofstream ofs;
    ofs.open("chunkdump.txt");
    if (ofs) {
        DumpChunkMap(ofs);
    }
    ofs.flush();
    ofs.close();
}

void
ChunkManager::DumpChunkMap(ostream &ofs)
{
   // Dump chunk map in the format of
   // chunkID fileID chunkSize
    mChunkTable.First();
    const CMapEntry* p;
    while ((p = mChunkTable.Next())) {
        ChunkInfoHandle* const cih = p->GetVal();
        ofs << cih->chunkInfo.chunkId <<
            " " << cih->chunkInfo.fileId <<
            " " << cih->chunkInfo.chunkSize <<
        "\n";
   }
}

int
ChunkManager::WriteChunkMetadata(
    kfsChunkId_t chunkId, KfsCallbackObj* cb, bool forceFlag /* = false */)
{
    ChunkInfoHandle** const ci = mChunkTable.Find(chunkId);
    if (! ci) {
        return -EBADF;
    }
    if (forceFlag) {
        (*ci)->SetMetaDirty();
    }
    return (*ci)->WriteChunkMetadata(cb);
}

int
ChunkManager::ReadChunkMetadata(kfsChunkId_t chunkId, KfsOp* cb)
{
    ChunkInfoHandle** const ci = mChunkTable.Find(chunkId);
    if (! ci) {
        return -EBADF;
    }
    ChunkInfoHandle* const cih = *ci;
    if (cih->isBeingReplicated) {
        KFS_LOG_STREAM_ERROR <<
            "denied meta data read for chunk: " << chunkId <<
            " replication is in flight" <<
        KFS_LOG_EOM;
        return -EBADF;
    }

    LruUpdate(*cih);
    if (cih->chunkInfo.AreChecksumsLoaded()) {
        int res = 0;
        cb->HandleEvent(EVENT_CMD_DONE, &res);
        return 0;
    }

    if (cih->readChunkMetaOp) {
        // if we have issued a read request for this chunk's metadata,
        // don't submit another one; otherwise, we will simply drive
        // up memory usage for useless IO's
        cih->readChunkMetaOp->AddWaiter(cb);
        return 0;
    }

    ReadChunkMetaOp* const rcm = new ReadChunkMetaOp(chunkId, cb);
    DiskIo*          const d   = SetupDiskIo(cih, rcm);
    if (! d) {
        delete rcm;
        return -ESERVERBUSY;
    }
    rcm->diskIo.reset(d);

    const int res = rcm->diskIo->Read(0, KFS_CHUNK_HEADER_SIZE);
    if (res < 0) {
        ReportIOFailure(cih, res);
        delete rcm;
        return res;
    }
    cih->readChunkMetaOp = rcm;
    return 0;
}

void
ChunkManager::ReadChunkMetadataDone(ReadChunkMetaOp* op, IOBuffer* dataBuf)
{
    ChunkInfoHandle** const ci = mChunkTable.Find(op->chunkId);
    if (! ci) {
        if (op->status == 0) {
            op->status    = -EBADF;
            op->statusMsg = "no such chunk";
            KFS_LOG_STREAM_ERROR <<
                "chunk meta data read completion: " <<
                    op->statusMsg  << " " << op->Show() <<
            KFS_LOG_EOM;
        }
        return;
    }
    ChunkInfoHandle* const cih = *ci;
    if (op != cih->readChunkMetaOp) {
        if (op->status >= 0) {
            op->status    = -EAGAIN;
            op->statusMsg = "stale meta data read";
        }
        KFS_LOG_STREAM_ERROR <<
            "chunk meta data read completion: " <<
                op->statusMsg  << " " << op->Show() <<
        KFS_LOG_EOM;
        return;
    }
    int res;
    if (! dataBuf ||
            dataBuf->BytesConsumable() < (int)KFS_CHUNK_HEADER_SIZE ||
            dataBuf->CopyOut(mChunkHeaderBuffer, kChunkHeaderBufferSize) !=
                kChunkHeaderBufferSize) {
        if (op->status != -ETIMEDOUT) {
            op->status    = -EIO;
            op->statusMsg = "short chunk meta data read";
        } else {
            op->statusMsg = "read timed out";
        }
        KFS_LOG_STREAM_ERROR <<
            "chunk meta data read completion: " << op->statusMsg  <<
            " " << (dataBuf ? dataBuf->BytesConsumable() : 0) <<
            " " << op->Show() <<
        KFS_LOG_EOM;
    } else {
        const DiskChunkInfo_t&  dci     =
            *reinterpret_cast<const DiskChunkInfo_t*>(mChunkHeaderBuffer);
        const uint64_t&        checksum =
            *reinterpret_cast<const uint64_t*>(&dci + 1);
        uint32_t               headerChecksum = 0;
        if ((checksum != 0 || mRequireChunkHeaderChecksumFlag) &&
                (headerChecksum = ComputeBlockChecksum(
                    mChunkHeaderBuffer, sizeof(dci))) != checksum) {
            op->status    = -EBADCKSUM;
            op->statusMsg = "chunk header checksum mismatch";
            ostringstream os;
            os << "chunk meta data read completion: " << op->statusMsg  <<
                " expected: " << checksum <<
                " computed: " << headerChecksum  <<
                " " << op->Show()
            ;
            const string str = os.str();
            KFS_LOG_STREAM_ERROR << str << KFS_LOG_EOM;
            if (mAbortOnChecksumMismatchFlag) {
                die(str);
            }
        } else if ((res = dci.Validate(op->chunkId, cih->IsStable() ?
                cih->chunkInfo.chunkVersion : kfsSeq_t(0))) < 0) {
            op->status    = res;
            op->statusMsg = "chunk metadata validation mismatch";
            KFS_LOG_STREAM_ERROR <<
                "chunk meta data read completion: " << op->statusMsg  <<
                " " << op->Show() <<
            KFS_LOG_EOM;
        } else {
            cih->chunkInfo.SetChecksums(dci.chunkBlockChecksum);
            if (cih->chunkInfo.chunkSize > (int64_t)dci.chunkSize) {
                const int64_t extra = cih->chunkInfo.chunkSize - dci.chunkSize;
                mUsedSpace -= extra;
                UpdateDirSpace(cih, -extra);
                cih->chunkInfo.chunkSize = dci.chunkSize;
            } else if (cih->chunkInfo.chunkSize != (int64_t)dci.chunkSize) {
                op->status    = res;
                op->statusMsg = "chunk metadata size mismatch";
                KFS_LOG_STREAM_ERROR <<
                    "chunk meta data read completion: " << op->statusMsg  <<
                    " file: " << cih->chunkInfo.chunkSize <<
                    " meta: " << dci.chunkSize <<
                    " " << op->Show() <<
                KFS_LOG_EOM;
            }
        }
    }
    LruUpdate(*cih);
    cih->readChunkMetaOp = 0;
    if (op->status < 0 && op->status != -ETIMEDOUT) {
        mCounters.mBadChunkHeaderErrorCount++;
        ChunkIOFailed(cih, op->status);
    }
}

bool
ChunkManager::IsChunkMetadataLoaded(kfsChunkId_t chunkId)
{
    ChunkInfoHandle *cih = 0;
    return (
        GetChunkInfoHandle(chunkId, &cih) >= 0 &&
        cih->chunkInfo.AreChecksumsLoaded()
    );
}

ChunkInfo_t*
ChunkManager::GetChunkInfo(kfsChunkId_t chunkId)
{
    ChunkInfoHandle** const ci = mChunkTable.Find(chunkId);
    return (ci ? &((*ci)->chunkInfo) : 0);
}

int
ChunkManager::MarkChunkStale(ChunkInfoHandle* cih, KfsCallbackObj* cb)
{
    const string s                  = MakeChunkPathname(cih);
    const string staleChunkPathname = MakeStaleChunkPathname(cih);
    string err;
    const int ret = DiskIo::Rename(
        s.c_str(), staleChunkPathname.c_str(), cb, &err) ? 0 : -1;
    KFS_LOG_STREAM_INFO <<
        "Moving chunk " << cih->chunkInfo.chunkId <<
        " to staleChunks dir " << staleChunkPathname <<
        (ret == 0 ? " ok" : " error:") << err <<
    KFS_LOG_EOM;
    return ret;
}

int
ChunkManager::StaleChunk(kfsChunkId_t chunkId,
    bool forceDeleteFlag, bool evacuatedFlag)
{
    ChunkInfoHandle** const ci = mChunkTable.Find(chunkId);
    if (! ci) {
        return -EBADF;
    }
    return StaleChunk(*ci, forceDeleteFlag, evacuatedFlag);
}

int
ChunkManager::StaleChunk(ChunkInfoHandle* cih,
    bool forceDeleteFlag, bool evacuatedFlag)
{
    assert(cih);
    if (mChunkTable.Erase(cih->chunkInfo.chunkId) <= 0) {
        return -EBADF;
    }
    gLeaseClerk.UnRegisterLease(cih->chunkInfo.chunkId);
    if (! cih->IsStale() && ! mPendingWrites.Delete(
            cih->chunkInfo.chunkId, cih->chunkInfo.chunkVersion)) {
        ostringstream os;
        os << "make stale failed to cleanup pending writes: "
            " chunk: "   << cih->chunkInfo.chunkId <<
            " version: " << cih->chunkInfo.chunkVersion
        ;
        die(os.str());
    }

    cih->MakeStale(mChunkInfoLists,
        (! forceDeleteFlag && ! mForceDeleteStaleChunksFlag) ||
        (evacuatedFlag && mKeepEvacuatedChunksFlag)
    );
    assert(! cih->HasWritesInFlight());
    RunStaleChunksQueue();
    return 0;
}

int
ChunkManager::TruncateChunk(kfsChunkId_t chunkId, int64_t chunkSize)
{
    // the truncated size should not exceed chunk size.
    if (chunkSize > (int64_t)CHUNKSIZE) {
        return -EINVAL;
    }
    ChunkInfoHandle** const ci = mChunkTable.Find(chunkId);
    if (! ci) {
        return -EBADF;
    }
    ChunkInfoHandle* const cih = *ci;
    string const chunkPathname = MakeChunkPathname(cih);

    // Cnunk close will truncate it to the cih->chunkInfo.chunkSize

    UpdateDirSpace(cih, -cih->chunkInfo.chunkSize);

    mUsedSpace -= cih->chunkInfo.chunkSize;
    mUsedSpace += chunkSize;
    cih->chunkInfo.chunkSize = chunkSize;

    UpdateDirSpace(cih, cih->chunkInfo.chunkSize);

    uint32_t const lastChecksumBlock = OffsetToChecksumBlockNum(chunkSize);

    // XXX: Could do better; recompute the checksum for this last block
    cih->chunkInfo.chunkBlockChecksum[lastChecksumBlock] = 0;
    cih->SetMetaDirty();

    return 0;
}

int
ChunkManager::ChangeChunkVers(ChangeChunkVersOp* op)
{
    ChunkInfoHandle** const ci = mChunkTable.Find(op->chunkId);
    if (! ci) {
        return -EBADF;
    }
    ChunkInfoHandle* const cih = *ci;
    bool stableFlag = cih->IsStable();
    if (cih->IsRenameInFlight()) {
        if (op->fromChunkVersion != cih->GetTargetStateAndVersion(stableFlag)) {
            op->statusMsg = (stableFlag ? "" : "not ");
            op->statusMsg += "stable target version mismatch";
            op->status    = -EINVAL;
            return op->status;
        }
    } else if (op->fromChunkVersion != cih->chunkInfo.chunkVersion) {
        op->statusMsg = "version mismatch";
        op->status    = -EINVAL;
        return op->status;
    }
    if (cih->HasWritesInFlight()) {
        op->statusMsg = "writes in flight";
        op->status    = -EINVAL;
        return op->status;
    }
    const int ret = ChangeChunkVers(
        cih, op->chunkVersion, op->makeStableFlag || stableFlag, op);
    if (ret < 0) {
        op->status = ret;
    }
    return ret;
}

int
ChunkManager::ChangeChunkVers(
    kfsChunkId_t    chunkId,
    int64_t         chunkVersion,
    bool            stableFlag,
    KfsCallbackObj* cb)
{
    ChunkInfoHandle** const ci = mChunkTable.Find(chunkId);
    if (! ci) {
        return -EBADF;
    }
    return ChangeChunkVers(*ci, chunkVersion, stableFlag, cb);
}

int
ChunkManager::ChangeChunkVers(
    ChunkInfoHandle* cih,
    int64_t          chunkVersion,
    bool             stableFlag,
    KfsCallbackObj*  cb)
{
    if (! cih->chunkInfo.chunkBlockChecksum) {
        KFS_LOG_STREAM_ERROR <<
            "attempt to change version on chunk: " <<
                cih->chunkInfo.chunkId << " denied: checksums are not loaded" <<
        KFS_LOG_EOM;
        return -EINVAL;
    }
    if (cih->IsWriteAppenderOwns() && ! IsChunkStable(cih)) {
        KFS_LOG_STREAM_WARN <<
            "attempt to change version on unstable chunk: " <<
                cih->chunkInfo.chunkId << " owned by write appender denied" <<
        KFS_LOG_EOM;
        return -EINVAL;
    }

    KFS_LOG_STREAM_INFO <<
        "Chunk " << MakeChunkPathname(cih) <<
        " already exists; changing version #" <<
        " from " << cih->chunkInfo.chunkVersion << " to " << chunkVersion <<
        " stable: " << cih->IsStable() << "=>" << stableFlag <<
    KFS_LOG_EOM;

    if (! mPendingWrites.Delete(
            cih->chunkInfo.chunkId, cih->chunkInfo.chunkVersion)) {
        ostringstream os;
        os << "change version failed to cleanup pending writes: "
            " chunk: "   << cih->chunkInfo.chunkId <<
            " version: " << cih->chunkInfo.chunkVersion
        ;
        die(os.str());
    }
    const bool renameFlag = true;
    return cih->WriteChunkMetadata(cb, renameFlag, stableFlag, chunkVersion);
}

void
ChunkManager::ReplicationDone(kfsChunkId_t chunkId, int status)
{
    ChunkInfoHandle** const ci = mChunkTable.Find(chunkId);
    if (! ci) {
        return;
    }
    ChunkInfoHandle* const cih = *ci;
    if (! cih->isBeingReplicated) {
        KFS_LOG_STREAM_DEBUG <<
            "irnored stale replication completion for"
                " chunk: "  << chunkId <<
                " status: " << status <<
        KFS_LOG_EOM;
        return;
    }

    KFS_LOG_STREAM_DEBUG <<
        "Replication for chunk: " << chunkId <<
        " status: " << status <<
        " " << MakeChunkPathname(cih) <<
    KFS_LOG_EOM;
    if (status < 0) {
        const bool forceDeleteFlag = true;
        StaleChunk(cih, forceDeleteFlag);
        return;
    }

    cih->isBeingReplicated = false;
    LruUpdate(*cih); // Add it to lru.
    if (cih->IsFileOpen() && cih->IsStable() &&
            ! cih->IsFileInUse() && ! cih->SyncMeta()) {
        Release(*cih);
    }
}

void
ChunkManager::Start()
{
    globalNetManager().RegisterTimeoutHandler(this);
}

void
ChunkManager::UpdateDirSpace(ChunkInfoHandle* cih, int64_t nbytes)
{
    ChunkDirInfo& dir = cih->GetDirInfo();
    dir.usedSpace += nbytes;
    if (dir.usedSpace < 0) {
        dir.usedSpace = 0;
    }
}

ChunkManager::ChunkDirInfo*
ChunkManager::GetDirForChunk()
{
    // do weighted random, so that we can fill all drives
    ChunkDirs::iterator dirToUse          = mChunkDirs.end();
    int64_t             totalFreeSpace    = 0;
    int64_t             totalPendingRead  = 0;
    int64_t             totalPendingWrite = 0;
    int64_t             maxFreeSpace      = 0;
    int                 dirCount          = 0;
    for (ChunkDirs::iterator it = mChunkDirs.begin();
            it < mChunkDirs.end();
            ++it) {
        it->placementSkipFlag = true;
        if (it->evacuateStartedFlag) {
            continue;
        }
        const int64_t space = it->availableSpace;
        if (space < mMinFsAvailableSpace ||
                space <= it->totalSpace * mMaxSpaceUtilizationThreshold) {
            continue;
        }
        dirCount++;
        totalFreeSpace += space;
        if (dirToUse == mChunkDirs.end()) {
            dirToUse = it;
        }
        if (maxFreeSpace < space) {
            maxFreeSpace = space;
        }
        it->placementSkipFlag = false;
        if (mChunkPlacementPendingReadWeight <= 0 &&
                mChunkPlacementPendingWriteWeight <= 0) {
            it->pendingReadBytes  = 0;
            it->pendingWriteBytes = 0;
            continue;
        }
        int     freeRequestCount;
        int     requestCount;
        int64_t readBlockCount;
        int64_t writeBlockCount;
        int     blockSize;
        if (! DiskIo::GetDiskQueuePendingCount(
                it->diskQueue,
                freeRequestCount,
                requestCount,
                readBlockCount,
                writeBlockCount,
                blockSize)) {
            die(it->dirname + ": get pending io count failed");
        }
        it->pendingReadBytes  = readBlockCount  * blockSize;
        it->pendingWriteBytes = writeBlockCount * blockSize;
        totalPendingRead  += it->pendingReadBytes;
        totalPendingWrite += it->pendingWriteBytes;
    }
    if (dirCount <= 0 || totalFreeSpace <= 0) {
        return 0;
    }
    if (dirCount == 1) {
        return &(*dirToUse);
    }
    if (mChunkPlacementPendingReadWeight > 0 ||
            mChunkPlacementPendingWriteWeight > 0) {
        // Exclude directories / drives that exceed "max io pending".
        const int64_t maxPendingIo = max(mMinPendingIoThreshold, (int64_t)
            (totalPendingRead * mChunkPlacementPendingReadWeight +
            totalPendingWrite * mChunkPlacementPendingReadWeight) / dirCount);
        ChunkDirs::iterator minIoPendingDir = mChunkDirs.end();
        for (ChunkDirs::iterator it = dirToUse;
                it < mChunkDirs.end();
                ++it) {
            if (it->placementSkipFlag) {
                continue;
            }
            if (it->pendingReadBytes + it->pendingWriteBytes >
                    maxPendingIo) {
                if (minIoPendingDir == mChunkDirs.end() ||
                        it->pendingReadBytes + it->pendingWriteBytes <
                        minIoPendingDir->pendingReadBytes +
                            minIoPendingDir->pendingWriteBytes) {
                    minIoPendingDir = it;
                }
                if (--dirCount <= 0) {
                    return &(*minIoPendingDir);
                }
                it->placementSkipFlag = true;
                if (it->availableSpace == maxFreeSpace) {
                    maxFreeSpace = -1; // Force update.
                }
                totalFreeSpace -= it->availableSpace;
                if (it == dirToUse) {
                    dirToUse = mChunkDirs.end();
                }
            } else if (dirToUse == mChunkDirs.end()) {
                dirToUse = it;
            }
        }
    }
    assert(totalFreeSpace > 0);
    int64_t minAvail = 0;
    if (mMaxPlacementSpaceRatio > 0) {
        if (maxFreeSpace < 0) {
            maxFreeSpace = 0;
            for (ChunkDirs::iterator it = dirToUse;
                    it < mChunkDirs.end();
                    ++it) {
                if (it->placementSkipFlag) {
                    continue;
                }
                if (maxFreeSpace < it->availableSpace) {
                    maxFreeSpace = it->availableSpace;
                }
            }
        }
        minAvail = (int64_t)(maxFreeSpace * mMaxPlacementSpaceRatio);
        for (ChunkDirs::iterator it = dirToUse;
                it < mChunkDirs.end();
                ++it) {
            if (it->placementSkipFlag) {
                continue;
            }
            if (minAvail <= it->availableSpace) {
                continue;
            }
            totalFreeSpace += minAvail - it->availableSpace;
        }
    }
    const double spaceWeight = double(1) / totalFreeSpace;
    const double randVal     = drand48();
    double       curVal      = 0;
    for (ChunkDirs::iterator it = dirToUse;
            it < mChunkDirs.end();
            ++it) {
        if (it->placementSkipFlag) {
            continue;
        }
        curVal += max(minAvail, it->availableSpace) * spaceWeight;
        if (randVal < curVal) {
            dirToUse = it;
            break;
        }
    }
    return (dirToUse == mChunkDirs.end() ? 0 : &(*dirToUse));
}

string
ChunkManager::MakeChunkPathname(ChunkInfoHandle *cih)
{
    return MakeChunkPathname(cih, cih->IsStable(), cih->chunkInfo.chunkVersion);
}

string
ChunkManager::MakeChunkPathname(ChunkInfoHandle *cih, bool stableFlag, kfsSeq_t targetVersion)
{
    return MakeChunkPathname(
        stableFlag ?
            cih->GetDirname() :
            cih->GetDirname() + mDirtyChunksDir,
        cih->chunkInfo.fileId,
        cih->chunkInfo.chunkId,
        stableFlag ? targetVersion : 0
    );
}

string
ChunkManager::MakeChunkPathname(const string &chunkdir, kfsFileId_t fid, kfsChunkId_t chunkId, kfsSeq_t chunkVersion)
{
    ostringstream os;

    os << chunkdir << fid << '.' << chunkId << '.' << chunkVersion;
    return os.str();
}

string
ChunkManager::MakeStaleChunkPathname(ChunkInfoHandle *cih)
{
    return MakeChunkPathname(
        cih->GetDirname() + mStaleChunksDir,
        cih->chunkInfo.fileId,
        cih->chunkInfo.chunkId,
        cih->chunkInfo.chunkVersion
    );
}

void
ChunkManager::AddMapping(ChunkManager::ChunkDirInfo& dir, const char* filename,
    int64_t infilesz)
{
    const int   kNumComponents = 3;
    long long   components[kNumComponents];
    const char* ptr    = filename;
    char*       end    = 0;
    int64_t     filesz = infilesz;
    int         i;

    for (i = 0; i < kNumComponents; i++) {
        components[i] = strtoll(ptr, &end, 10);
        if (components[i] < 0) {
            break;
        }
        if ((*end & 0xFF) != '.') {
            if (*end == 0) {
                i++;
            }
            break;
        }
        ptr = end + 1;
    }
    if (i != kNumComponents || *end) {
        KFS_LOG_STREAM_INFO <<
            "ignoring malformed chunk file name: " <<
                dir.dirname << filename <<
        KFS_LOG_EOM;
        return;
    }
    // Allow files bigger than chunk size. If file wasn't properly closed,
    // but was in the stable directory, its header needs to be read,
    // validated and proper size must be set.
    // The file might be bigger by one io buffer size, and io buffer size is
    // guaranteed to be less or equal to the KFS_CHUNK_HEADER_SIZE.
    const int64_t kMaxChunkFileSize = (int64_t)(KFS_CHUNK_HEADER_SIZE + CHUNKSIZE);
    if (filesz < (int64_t)KFS_CHUNK_HEADER_SIZE ||
            filesz > (int64_t)(kMaxChunkFileSize + KFS_CHUNK_HEADER_SIZE)) {
        KFS_LOG_STREAM_INFO <<
            "ignoring invalid chunk file: " << dir.dirname << filename <<
            " size: " << filesz <<
        KFS_LOG_EOM;
        return;
    }
    const chunkId_t chunkId   = components[1];
    const kfsSeq_t  chunkVers = components[2];
    if (filesz > kMaxChunkFileSize) {
        // Load and validate chunk header, and set proper file size.
        const string cf(dir.dirname + filename);
        const int    fd = open(cf.c_str(), O_RDONLY);
        if (fd < 0) {
            const int err = errno;
            KFS_LOG_STREAM_INFO <<
                "ignoring invalid chunk file: " << cf <<
                    " size: " << filesz <<
                    " :" << QCUtils::SysError(err) <<
            KFS_LOG_EOM;
            return;
        }
        const ssize_t rd = read(fd, mChunkHeaderBuffer, kChunkHeaderBufferSize);
        close(fd);
        if (rd != kChunkHeaderBufferSize) {
            const int err = rd < 0 ? errno : EINVAL;
            KFS_LOG_STREAM_INFO <<
                "ignoring invalid chunk file: " << cf <<
                    " size: "                   << filesz <<
                    " read: "                   << rd <<
                    " :"                        << QCUtils::SysError(err) <<
            KFS_LOG_EOM;
            return;
        }
        const DiskChunkInfo_t& dci      =
            *reinterpret_cast<const DiskChunkInfo_t*>(mChunkHeaderBuffer);
        const uint64_t         checksum =
            *reinterpret_cast<const uint64_t*>(&dci + 1);
        const int res = dci.Validate(chunkId, chunkVers);
        if (res < 0) {
            KFS_LOG_STREAM_INFO <<
                "ignoring invalid chunk file: " << cf <<
                    " size: "                   << filesz <<
                    " invalid chunk header"
                    " status: "                 << res <<
            KFS_LOG_EOM;
            return;
        }
        uint32_t hdrChecksum = 0;
        if ((checksum != 0 || mRequireChunkHeaderChecksumFlag) &&
                ((hdrChecksum = ComputeBlockChecksum(
                    mChunkHeaderBuffer, sizeof(dci))) != checksum)) {
            KFS_LOG_STREAM_INFO <<
                "ignoring invalid chunk file: "  << cf <<
                    " invalid header:"
                    " size: "                    << filesz <<
                    " chunk size: "              << dci.chunkSize <<
                    " checksum: "                << checksum <<
                    " expect: "                  << hdrChecksum <<
            KFS_LOG_EOM;
            return;
        }
        filesz = dci.chunkSize + KFS_CHUNK_HEADER_SIZE;
        if (truncate(cf.c_str(), filesz)) {
            const int err = errno;
            KFS_LOG_STREAM_ERROR <<
                "failed truncate chunk file: " << cf <<
                    " size: "                  << infilesz <<
                    " to: "                    << filesz <<
                    " :"                       << QCUtils::SysError(err) <<
            KFS_LOG_EOM;
        } else {
            KFS_LOG_STREAM_INFO <<
                "truncated chunk file: " << cf <<
                    " size: "            << infilesz <<
                    " to: "              << filesz <<
            KFS_LOG_EOM;
        }
    }
    ChunkInfoHandle* cih = 0;
    if (GetChunkInfoHandle(chunkId, &cih) == 0) {
        string const name(dir.dirname + filename);
        KFS_LOG_STREAM_INFO <<
            (mForceDeleteStaleChunksFlag ? "deleting" : "moving") <<
            " duplicate chunk: " << chunkId <<
            " file name: " << name <<
            " keeping: "   << MakeChunkPathname(cih) <<
        KFS_LOG_EOM;
        if (mForceDeleteStaleChunksFlag) {
            if (unlink(name.c_str())) {
                const int err = errno;
                KFS_LOG_STREAM_ERROR <<
                    "failed to remove " << name <<
                    " error: " << QCUtils::SysError(err) <<
                KFS_LOG_EOM;
            }
        } else {
            string const staleName(
                dir.dirname + mStaleChunksDir + filename);
            if (rename(name.c_str(), staleName.c_str())) {
                const int err = errno;
                KFS_LOG_STREAM_ERROR <<
                    "failed to rename " << name <<
                    " error: " << QCUtils::SysError(err) <<
                KFS_LOG_EOM;
            }
        }
        return;
    }
    cih = new ChunkInfoHandle(dir);
    cih->chunkInfo.fileId       = components[0];
    cih->chunkInfo.chunkId      = chunkId;
    cih->chunkInfo.chunkVersion = chunkVers;
    cih->chunkInfo.chunkSize    = filesz - KFS_CHUNK_HEADER_SIZE;
    AddMapping(cih);
}

int
ChunkManager::OpenChunk(kfsChunkId_t chunkId, int openFlags)
{
    ChunkInfoHandle** const ci = mChunkTable.Find(chunkId);
    if (! ci) {
        KFS_LOG_STREAM_DEBUG << "no such chunk: " << chunkId << KFS_LOG_EOM;
        return -EBADF;
    }
    return OpenChunk(*ci, openFlags);
}

int
ChunkManager::OpenChunk(ChunkInfoHandle* cih, int openFlags)
{
    if (cih->IsFileOpen()) {
        return 0;
    }
    if (! cih->dataFH) {
        cih->dataFH.reset(new DiskIo::File());
    }
    string errMsg;
    const bool kReserveFileSpace = true;
    const string fn = MakeChunkPathname(cih);
    bool tempFailureFlag = false;
    // Set reservation size larger than max chunk size in order to detect files
    // that weren't properly closed. + 1 here will make file one io block bigger
    // QCDiskQueue::OpenFile() makes EOF block size aligned.
    if (! cih->dataFH->Open(
            fn.c_str(),
            CHUNKSIZE + KFS_CHUNK_HEADER_SIZE + 1,
            (openFlags & (O_WRONLY | O_RDWR)) == 0,
            kReserveFileSpace,
            (openFlags & O_CREAT) != 0,
            &errMsg,
            &tempFailureFlag,
            mBufferedIoFlag)) {
        mCounters.mOpenErrorCount++;
        if ((openFlags & O_CREAT) != 0 || ! tempFailureFlag) {
            //
            // we are unable to open/create a file. notify the metaserver
            // of lost data so that it can re-replicate if needed.
            //
            NotifyMetaCorruptedChunk(cih, -EBADF);
            if (mChunkTable.Erase(cih->chunkInfo.chunkId) > 0) {
                const int64_t size = min(mUsedSpace, cih->chunkInfo.chunkSize);
                UpdateDirSpace(cih, -size);
                mUsedSpace -= size;
            }
            Delete(*cih);
        }
        KFS_LOG_STREAM_ERROR <<
            "failed to " << (((openFlags & O_CREAT) == 0) ? "open" : "create") <<
            " chunk file: " << fn << " :" << errMsg <<
        KFS_LOG_EOM;
        return (tempFailureFlag ? -EAGAIN : -EBADF);
    }
    globals().ctrOpenDiskFds.Update(1);
    LruUpdate(*cih);

    // the checksums will be loaded async
    return 0;
}

int
ChunkManager::CloseChunk(kfsChunkId_t chunkId)
{
    ChunkInfoHandle** const ci = mChunkTable.Find(chunkId);
    if (! ci) {
        return -EBADF;
    }
    return CloseChunk(*ci);
}

bool
ChunkManager::CloseChunkIfReadable(kfsChunkId_t chunkId)
{
    ChunkInfoHandle** const ci = mChunkTable.Find(chunkId);
    if (! ci) {
        return -EBADF;
    }
    return (
        IsChunkStable(*ci) &&
        (*ci)->IsChunkReadable() &&
        CloseChunk(*ci) == 0
    );
}

int
ChunkManager::CloseChunk(ChunkInfoHandle* cih)
{
    if (cih->IsWriteAppenderOwns()) {
        KFS_LOG_STREAM_INFO <<
            "Ignoring close chunk on chunk: " << cih->chunkInfo.chunkId <<
            " open for append " <<
        KFS_LOG_EOM;
        return -EINVAL;
    }

    // Close file if not in use.
    if (cih->IsFileOpen() && ! cih->IsFileInUse() &&
            ! cih->isBeingReplicated && ! cih->SyncMeta()) {
        Release(*cih);
    } else {
        KFS_LOG_STREAM_INFO <<
            "Didn't release chunk " << cih->chunkInfo.chunkId <<
            " on close;  might give up lease" <<
        KFS_LOG_EOM;
        gLeaseClerk.RelinquishLease(
            cih->chunkInfo.chunkId, cih->chunkInfo.chunkSize);
    }
    return 0;
}

void
ChunkManager::ChunkSize(SizeOp* op)
{
    ChunkInfoHandle* cih;
    if (GetChunkInfoHandle(op->chunkId, &cih) < 0) {
        op->status    = -EBADF;
        op->statusMsg = "no such chunk";
        return;
    }
    if (cih->isBeingReplicated) {
        op->status    = -EAGAIN;
        op->statusMsg = "chunk replication in progress";
        return;
    }
    if (op->chunkVersion >= 0 &&
            op->chunkVersion != cih->chunkInfo.chunkVersion) {
        op->status    = -EBADVERS;
        op->statusMsg = "chunk version mismatch";
        return;
    }
    if (cih->IsWriteAppenderOwns() &&
            ! gAtomicRecordAppendManager.IsChunkStable(op->chunkId)) {
        op->statusMsg = "write append in progress, returning max chunk size";
        op->size      = CHUNKSIZE;
        KFS_LOG_STREAM_DEBUG <<
            op->statusMsg <<
            " chunk: " << op->chunkId <<
            " file: "  << op->fileId  <<
            " size: "  << op->size    <<
        KFS_LOG_EOM;
        return;
    }
    op->size = cih->chunkInfo.chunkSize;
}

void
ChunkManager::GetDriveName(ReadOp *op)
{
    ChunkInfoHandle *cih;

    if (GetChunkInfoHandle(op->chunkId, &cih) < 0) {
        return;
    }
    // provide the path to the client for telemetry
    op->driveName = cih->GetDirname();
}

int
ChunkManager::ReadChunk(ReadOp *op)
{
    ChunkInfoHandle* cih = 0;
    if (GetChunkInfoHandle(op->chunkId, &cih) < 0) {
        return -EBADF;
    }
    // provide the path to the client for telemetry
    op->driveName = cih->GetDirname();

    // the checksums should be loaded...
    cih->chunkInfo.VerifyChecksumsLoaded();

    if (op->chunkVersion != cih->chunkInfo.chunkVersion) {
        KFS_LOG_STREAM_INFO << "Version # mismatch (have=" <<
            cih->chunkInfo.chunkVersion << " vs asked=" << op->chunkVersion <<
            ")...failing a read" <<
        KFS_LOG_EOM;
        return -EBADVERS;
    }
    DiskIo* const d = SetupDiskIo(cih, op);
    if (! d) {
        return -ESERVERBUSY;
    }

    op->diskIo.reset(d);

    // schedule a read based on the chunk size
    if (op->offset >= cih->chunkInfo.chunkSize) {
        op->numBytesIO = 0;
    } else if ((int64_t) (op->offset + op->numBytes) > cih->chunkInfo.chunkSize) {
        op->numBytesIO = cih->chunkInfo.chunkSize - op->offset;
    } else {
        op->numBytesIO = op->numBytes;
    }

    if (op->numBytesIO == 0) {
        return -EIO;
    }
    // for checksumming to work right, reads should be in terms of
    // checksum-blocks.
    const int64_t offset = OffsetToChecksumBlockStart(op->offset);

    size_t numBytesIO = OffsetToChecksumBlockEnd(op->offset + op->numBytesIO - 1) - offset;

    // Make sure we don't try to read past EOF; the checksumming will
    // do the necessary zero-padding.
    if ((int64_t) (offset + numBytesIO) > cih->chunkInfo.chunkSize)
        numBytesIO = cih->chunkInfo.chunkSize - offset;

    const int ret = op->diskIo->Read(offset + KFS_CHUNK_HEADER_SIZE, numBytesIO);
    if (ret < 0) {
        ReportIOFailure(cih, ret);
        return ret;
    }
    // read was successfully scheduled
    return 0;
}

int
ChunkManager::WriteChunk(WriteOp *op)
{
    ChunkInfoHandle* cih = 0;
    if (GetChunkInfoHandle(op->chunkId, &cih) < 0) {
        return -EBADF;
    }
    // the checksums should be loaded...
    cih->chunkInfo.VerifyChecksumsLoaded();

    // schedule a write based on the chunk size.  Make sure that a
    // write doesn't overflow the size of a chunk.
    op->numBytesIO = min((size_t) (CHUNKSIZE - op->offset), op->numBytes);

    if (op->numBytesIO <= 0 || op->offset < 0)
        return -EINVAL;

    const int64_t addedBytes(op->offset + op->numBytesIO - cih->chunkInfo.chunkSize);
    if (addedBytes > 0 && mUsedSpace + addedBytes >= mTotalSpace) {
        KFS_LOG_STREAM_ERROR <<
            "out of disk space: " << mUsedSpace << " + " << addedBytes <<
            " = " << (mUsedSpace + addedBytes) << " >= " << mTotalSpace <<
        KFS_LOG_EOM;
	return -ENOSPC;
    }

    int64_t offset     = op->offset;
    ssize_t numBytesIO = op->numBytesIO;
    if ((OffsetToChecksumBlockStart(offset) == offset) &&
            ((size_t) numBytesIO >= (size_t) CHECKSUM_BLOCKSIZE)) {
        if (numBytesIO % CHECKSUM_BLOCKSIZE != 0) {
            return -EINVAL;
        }
        if (op->wpop && !op->isFromReReplication &&
                op->checksums.size() == size_t(numBytesIO / CHECKSUM_BLOCKSIZE)) {
            assert(op->checksums[0] == op->wpop->checksum || op->checksums.size() > 1);
        } else {
            op->checksums = ComputeChecksums(op->dataBuf, numBytesIO);
        }
    } else {
        if ((size_t) numBytesIO >= (size_t) CHECKSUM_BLOCKSIZE) {
            assert((size_t) numBytesIO < (size_t) CHECKSUM_BLOCKSIZE);
            return -EINVAL;
        }
        int            off     = (int)(offset % CHECKSUM_BLOCKSIZE);
        const uint32_t blkSize = (size_t(off + numBytesIO) > CHECKSUM_BLOCKSIZE) ?
            2 * CHECKSUM_BLOCKSIZE : CHECKSUM_BLOCKSIZE;

        op->checksums.clear();
        // The checksum block we are after is beyond the current
        // end-of-chunk.  So, treat that as a 0-block and splice in.
        if (offset - off >= cih->chunkInfo.chunkSize) {
            IOBuffer data;
            data.ReplaceKeepBuffersFull(op->dataBuf, off, numBytesIO);
            data.ZeroFill(blkSize - (off + numBytesIO));
            op->dataBuf->Move(&data);
        } else {
            // Need to read the data block over which the checksum is
            // computed.
            if (op->rop == NULL) {
                // issue a read
                ReadOp *rop = new ReadOp(op, offset - off, blkSize);
                KFS_LOG_STREAM_DEBUG <<
                    "write triggered a read for offset=" << offset <<
                KFS_LOG_EOM;
                op->rop = rop;
                rop->Execute();
                // It is possible that the both read and write ops are complete
                // at this point. This normally happens in the case of errors.
                // In such cases all error handlers are already invoked.
                // If not then the write op will be restarted once read op
                // completes.
                // Return now.
                return 0;
            }
            // If the read failed, cleanup and bail
            if (op->rop->status < 0) {
                op->status = op->rop->status;
                op->rop->wop = NULL;
                delete op->rop;
                op->rop = NULL;
                return op->HandleDone(EVENT_DISK_ERROR, NULL);
            }

            // All is good.  So, get on with checksumming
            op->rop->dataBuf->ReplaceKeepBuffersFull(op->dataBuf, off, numBytesIO);

            delete op->dataBuf;
            op->dataBuf = op->rop->dataBuf;
            op->rop->dataBuf = NULL;
            // If the buffer doesn't have a full CHECKSUM_BLOCKSIZE worth
            // of data, zero-pad the end.  We don't need to zero-pad the
            // front because the underlying filesystem will zero-fill when
            // we read a hole.
            ZeroPad(op->dataBuf);
        }

        assert(op->dataBuf->BytesConsumable() == (int) blkSize);
        op->checksums = ComputeChecksums(op->dataBuf, blkSize);

        // Trim data at the buffer boundary from the beginning, to make write
        // offset close to where we were asked from.
        int numBytes(numBytesIO);
        offset -= off;
        op->dataBuf->TrimAtBufferBoundaryLeaveOnly(off, numBytes);
        offset += off;
        numBytesIO = numBytes;
    }

    DiskIo* const d = SetupDiskIo(cih, op);
    if (! d) {
        return -ESERVERBUSY;
    }
    op->diskIo.reset(d);

    /*
    KFS_LOG_STREAM_DEBUG <<
        "Checksum for chunk: " << op->chunkId << ", offset=" << op->offset <<
        ", bytes=" << op->numBytesIO << ", # of cksums=" << op->checksums.size() <<
    KFS_LOG_EOM;
    */

    int res = op->diskIo->Write(
        offset + KFS_CHUNK_HEADER_SIZE, numBytesIO, op->dataBuf);
    if (res >= 0) {
        UpdateChecksums(cih, op);
        assert(res <= numBytesIO);
        res = min(res, int(op->numBytesIO));
        op->numBytesIO = numBytesIO;
        cih->StartWrite(op);
    } else {
        op->diskIo.reset();
        ReportIOFailure(cih, res);
    }
    return res;
}

void
ChunkManager::UpdateChecksums(ChunkInfoHandle *cih, WriteOp *op)
{
    int64_t endOffset = op->offset + op->numBytesIO;

    // the checksums should be loaded...
    cih->chunkInfo.VerifyChecksumsLoaded();

    for (vector<uint32_t>::size_type i = 0; i < op->checksums.size(); i++) {
        int64_t  offset = op->offset + i * CHECKSUM_BLOCKSIZE;
        uint32_t checksumBlock = OffsetToChecksumBlockNum(offset);

        cih->chunkInfo.chunkBlockChecksum[checksumBlock] = op->checksums[i];
    }

    if (cih->chunkInfo.chunkSize < endOffset) {

        UpdateDirSpace(cih, endOffset - cih->chunkInfo.chunkSize);

	mUsedSpace += endOffset - cih->chunkInfo.chunkSize;
        cih->chunkInfo.chunkSize = endOffset;

    }
    assert(0 <= mUsedSpace && mUsedSpace <= mTotalSpace);
}

void
ChunkManager::WriteDone(WriteOp* op)
{
    ChunkInfoHandle* cih = 0;
    if (GetChunkInfoHandle(op->chunkId, &cih) < 0) {
        return;
    }
    if (! cih->IsFileEquals(op->diskIo)) {
        KFS_LOG_STREAM_DEBUG <<
            "ignoring stale write completion: " << op->Show() <<
            " disk io: " << reinterpret_cast<const void*>(op->diskIo.get()) <<
        KFS_LOG_EOM;
        return;
    }
    cih->WriteDone(op);
}

bool
ChunkManager::ReadChunkDone(ReadOp *op)
{
    ChunkInfoHandle *cih = NULL;

    bool staleRead = false;
    if ((GetChunkInfoHandle(op->chunkId, &cih) < 0) ||
            (op->chunkVersion != cih->chunkInfo.chunkVersion) ||
            (staleRead = ! cih->IsFileEquals(op->diskIo))) {
        if (op->dataBuf) {
            op->dataBuf->Clear();
        }
        if (cih) {
            KFS_LOG_STREAM_INFO << "Version # mismatch (have=" <<
                cih->chunkInfo.chunkVersion <<
                " vs asked=" << op->chunkVersion << ")" <<
                (staleRead ? " stale read" : "") <<
            KFS_LOG_EOM;
        }
        op->status = -EBADVERS;
        return true;
    }

    const int readLen = op->dataBuf->BytesConsumable();
    if (readLen <= 0) {
        KFS_LOG_STREAM_ERROR << "Short read for" <<
            " chunk: "  << cih->chunkInfo.chunkId  <<
            " size: "   << cih->chunkInfo.chunkSize <<
            " read:"
            " offset: " << op->offset <<
            " len: "    << readLen <<
        KFS_LOG_EOM;
        if (cih->chunkInfo.chunkSize > op->offset + readLen) {
            op->status = -EIO;
            ChunkIOFailed(cih, op->status);
        } else {
            // Size has decreased while read was in flight.
            // Possible race with truncation, which could be considered valid.
            // Another possibility that read and write completed out of order,
            // which is really a bug, especially if this really is read modify
            // write.
            assert(! op->wop);
            op->status = -EAGAIN;
        }
        return true;
    }

    ZeroPad(op->dataBuf);

    assert(op->dataBuf->BytesConsumable() >= (int) CHECKSUM_BLOCKSIZE);

    // either nothing to verify or it better match

    bool mismatch = false;

    // figure out the block we are starting from and grab all the checksums
    vector<uint32_t>::size_type i, checksumBlock = OffsetToChecksumBlockNum(op->offset);
    op->checksum = ComputeChecksums(op->dataBuf, op->dataBuf->BytesConsumable());

    // the checksums should be loaded...
    if (!cih->chunkInfo.AreChecksumsLoaded()) {
        // the read took too long; the checksums got paged out.  ask the client to retry
        KFS_LOG_STREAM_INFO << "Checksums for chunk " <<
            cih->chunkInfo.chunkId  <<
            " got paged out; returning EAGAIN to client" <<
        KFS_LOG_EOM;
        op->status = -EAGAIN;
        return true;
    }

    cih->chunkInfo.VerifyChecksumsLoaded();

    for (i = 0;
            i < op->checksum.size() &&
                checksumBlock < MAX_CHUNK_CHECKSUM_BLOCKS;
            checksumBlock++, i++) {
        const uint32_t checksum =
            cih->chunkInfo.chunkBlockChecksum[checksumBlock];
        if (checksum == 0 && op->checksum[i] == mNullBlockChecksum &&
                mAllowSparseChunksFlag) {
            KFS_LOG_STREAM_INFO <<
                " chunk: "      << cih->chunkInfo.chunkId <<
                " block: "      << checksumBlock <<
                " no checksum " <<
                " read: "       << op->checksum[i] <<
            KFS_LOG_EOM;
            continue;
        }
        if (op->checksum[i] != checksum) {
            mismatch = true;
            break;
        }
    }

    if (!mismatch) {
        // for checksums to verify, we did reads in multiples of
        // checksum block sizes.  so, get rid of the extra
        AdjustDataRead(op);
        return true;
    }
    const bool retry = op->retryCnt++ < mReadChecksumMismatchMaxRetryCount;
    op->status = -EBADCKSUM;

    ostringstream os;
    os <<
        "Checksum mismatch for chunk=" << op->chunkId <<
        " offset="    << op->offset <<
        " bytes="     << op->numBytesIO <<
        ": expect: "  << cih->chunkInfo.chunkBlockChecksum[checksumBlock] <<
        " computed: " << op->checksum[i] <<
        " try: "      << op->retryCnt <<
        ((mAbortOnChecksumMismatchFlag && ! retry) ? " abort" : "")
    ;
    const string str = os.str();
    KFS_LOG_STREAM_ERROR << str << KFS_LOG_EOM;
    if (retry) {
        op->dataBuf->Clear();
        if (ReadChunk(op) == 0) {
            return false;
        }
    }
    if (mAbortOnChecksumMismatchFlag) {
        die(str);
    }
    op->dataBuf->Clear();

    // Notify the metaserver that the chunk we have is "bad"; the
    // metaserver will re-replicate this chunk.
    mCounters.mReadChecksumErrorCount++;
    ChunkIOFailed(cih, op->status);
    return true;
}

void
ChunkManager::NotifyMetaCorruptedChunk(ChunkInfoHandle* cih, int err)
{
    assert(cih);
    if (err == 0) {
        mCounters.mLostChunksCount++;
        cih->GetDirInfo().corruptedChunksCount++;
    } else {
        mCounters.mCorruptedChunksCount++;
    }

    KFS_LOG_STREAM_ERROR <<
        (err == 0 ? "lost" : "corrupted") <<
        " chunk: "     << cih->chunkInfo.chunkId <<
        " file: "      << cih->chunkInfo.fileId <<
        " error: "     << err <<
        (err ? string() : QCUtils::SysError(-err, " ")) <<
        " dir: "       << cih->GetDirname() <<
        " total:"
        " lost: "      << mCounters.mLostChunksCount <<
        " corrupted: " << mCounters.mCorruptedChunksCount <<
    KFS_LOG_EOM;

    // This op will get deleted when we get an ack from the metaserver
    CorruptChunkOp* const op = new CorruptChunkOp(
        0, cih->chunkInfo.fileId, cih->chunkInfo.chunkId);
    op->isChunkLost = err == 0;
    gMetaServerSM.EnqueueOp(op);
    // Meta server automatically cleans up leases for corrupted chunks.
    gLeaseClerk.UnRegisterLease(cih->chunkInfo.chunkId);
}

void
ChunkManager::ChunkIOFailed(kfsChunkId_t chunkId, int err, const DiskIo::File* file)
{
    ChunkInfoHandle* cih;
    if (GetChunkInfoHandle(chunkId, &cih) < 0) {
        KFS_LOG_STREAM_ERROR <<
            "corrupt chunk: " << chunkId << " not in table" <<
        KFS_LOG_EOM;
        return;
    }
    if (! cih->IsFileEquals(file)) {
        KFS_LOG_STREAM_DEBUG <<
            "ignoring stale io failure notification: " << chunkId <<
            " file: " << reinterpret_cast<const void*>(file) <<
        KFS_LOG_EOM;
        return;
    }
    ChunkIOFailed(cih, err);
}

void
ChunkManager::ReportIOFailure(ChunkInfoHandle* cih, int err)
{
    if (err == -EAGAIN || err == -ENOMEM || err == -ETIMEDOUT) {
        KFS_LOG_STREAM_ERROR <<
            "assuming temporary io failure chunk: " << cih->chunkInfo.chunkId <<
            " dir: " << cih->GetDirname() <<
            " " << QCUtils::SysError(-err) <<
        KFS_LOG_EOM;
        return;
    }
    ChunkIOFailed(cih, err);
}

void
ChunkManager::ChunkIOFailed(ChunkInfoHandle* cih, int err)
{
    NotifyMetaCorruptedChunk(cih, err);
    StaleChunk(cih);
}

void
ChunkManager::ChunkIOFailed(kfsChunkId_t chunkId, int err, const DiskIo* diskIo)
{
    ChunkIOFailed(chunkId, err, diskIo ? diskIo->GetFilePtr().get() : 0);
}

//
// directory with dirname is unaccessable; maybe drive failed.  so,
// notify metaserver of lost blocks.  the metaserver will then
// re-replicate.
//
void
ChunkManager::NotifyMetaChunksLost(ChunkManager::ChunkDirInfo& dir)
{
    KFS_LOG_STREAM(dir.evacuateDoneFlag ?
            MsgLogger::kLogLevelWARN : MsgLogger::kLogLevelERROR) <<
        (dir.evacuateDoneFlag ? "evacuate done: " : "lost") <<
        " chunk directory: " << dir.dirname <<
    KFS_LOG_EOM;
    CorruptChunkOp* op    = 0;
    const string*   dname = &(dir.dirname);
    for (int i = 0; i < ChunkDirInfo::kChunkDirListCount; i++) {
        ChunkDirInfo::ChunkLists& list = dir.chunkLists[i];
        ChunkInfoHandle* cih;
        while ((cih = ChunkDirList::Front(list))) {
            const kfsChunkId_t chunkId = cih->chunkInfo.chunkId;
            const kfsFileId_t  fileId  = cih->chunkInfo.fileId;
            // get rid of chunkid from our list
            const bool staleFlag = cih->IsStale();
            ChunkInfoHandle** const ci = mChunkTable.Find(chunkId);
            if (ci && *ci == cih) {
                if (mChunkTable.Erase(chunkId) <= 0) {
                    die("corrupted chunk table");
                }
            }
            const int64_t size = min(mUsedSpace, cih->chunkInfo.chunkSize);
            UpdateDirSpace(cih, -size);
            mUsedSpace -= size;
            Delete(*cih);
            if (staleFlag) {
                continue;
            }
            KFS_LOG_STREAM_INFO <<
                "lost chunk: " << chunkId <<
                " file: " << fileId <<
            KFS_LOG_EOM;
            mCounters.mDirLostChunkCount++;
            if (! gMetaServerSM.IsConnected()) {
                // If no connection exists then the meta server assumes that
                // the chunks are lost anyway, and the inventory synchronization
                // in the meta hello is sufficient on re-connect.
                continue;
            }
            if (! op) {
                op = new CorruptChunkOp(0, fileId, chunkId, dname);
                // Do not count as corrupt.
                op->isChunkLost = true;
                dname = 0;
            } else {
                op->fid     = fileId;
                op->chunkId = chunkId;
                op->chunkDir.clear();
            }
            const int ref = op->Ref();
            gMetaServerSM.EnqueueOp(op);
            assert(op->GetRef() >= ref);
            if (op->GetRef() > ref) {
                // Op in flight / queued allocate a new one.
                op->UnRef();
                op = 0;
            }
        }
    }
    if (op) {
        op->UnRef();
    }
    if (! dir.evacuateDoneFlag) {
        mCounters.mChunkDirLostCount++;
    }
    const bool updateFlag = dir.countFsSpaceAvailableFlag;
    dir.Stop();
    if (updateFlag) {
        UpdateCountFsSpaceAvailableFlags();
    }
    mDirChecker.Add(dir.dirname, dir.dirLock);
}

int
ChunkManager::UpdateCountFsSpaceAvailableFlags()
{
    int ret = 0;
    for (ChunkDirs::iterator it = mChunkDirs.begin();
            it != mChunkDirs.end();
            ++it) {
        if (it->availableSpace < 0 || it->evacuateStartedFlag) {
            it->countFsSpaceAvailableFlag = false;
            continue;
        }
        ChunkDirs::const_iterator cit;
        for (cit = mChunkDirs.begin();
                cit != it &&
                    (cit->availableSpace < 0 ||
                        ! cit->countFsSpaceAvailableFlag ||
                        cit->deviceId != it->deviceId);
                ++cit)
            {}
        it->countFsSpaceAvailableFlag = cit == it;
        if (it->countFsSpaceAvailableFlag) {
            ret++;
        }
    }
    return ret;
}

void
ChunkManager::ZeroPad(IOBuffer *buffer)
{
    const int bytesFilled = buffer->BytesConsumable();
    if ((bytesFilled % CHECKSUM_BLOCKSIZE) == 0) {
        return;
    }
    const int numToZero = CHECKSUM_BLOCKSIZE - (bytesFilled % CHECKSUM_BLOCKSIZE);
    if (numToZero > 0) {
        // pad with 0's
        buffer->ZeroFill(numToZero);
    }
}

void
ChunkManager::AdjustDataRead(ReadOp *op)
{
    op->dataBuf->Consume(
        op->offset - OffsetToChecksumBlockStart(op->offset));
    op->dataBuf->Trim(op->numBytesIO);
}

uint32_t
ChunkManager::GetChecksum(kfsChunkId_t chunkId, int64_t offset)
{
    ChunkInfoHandle *cih;

    if (offset < 0 || GetChunkInfoHandle(chunkId, &cih) < 0)
        return 0;

    const uint32_t checksumBlock = OffsetToChecksumBlockNum(offset);
    // the checksums should be loaded...
    cih->chunkInfo.VerifyChecksumsLoaded();

    assert(checksumBlock < MAX_CHUNK_CHECKSUM_BLOCKS);

    return cih->chunkInfo.chunkBlockChecksum[
        min(MAX_CHUNK_CHECKSUM_BLOCKS - 1, checksumBlock)];
}

vector<uint32_t>
ChunkManager::GetChecksums(kfsChunkId_t chunkId, int64_t offset, size_t numBytes)
{
    ChunkInfoHandle** const ci = mChunkTable.Find(chunkId);

    if (offset < 0 || ! ci) {
        return vector<uint32_t>();
    }

    const ChunkInfoHandle * const cih = *ci;
    // the checksums should be loaded...
    cih->chunkInfo.VerifyChecksumsLoaded();

    return (vector<uint32_t>(
        cih->chunkInfo.chunkBlockChecksum +
            OffsetToChecksumBlockNum(offset),
        cih->chunkInfo.chunkBlockChecksum +
            min(MAX_CHUNK_CHECKSUM_BLOCKS,
                OffsetToChecksumBlockNum(
                    offset + numBytes + CHECKSUM_BLOCKSIZE - 1))
    ));
}

DiskIo*
ChunkManager::SetupDiskIo(ChunkInfoHandle *cih, KfsCallbackObj *op)
{
    if (! cih->IsFileOpen()) {
        CleanupInactiveFds();
        if (OpenChunk(cih, O_RDWR) < 0) {
            return 0;
        }
    }
    LruUpdate(*cih);
    return new DiskIo(cih->dataFH, op);
}

int
ChunkManager::Restart()
{
    if (gLogger.GetVersionFromCkpt() != gLogger.GetLoggerVersionNum()) {
        KFS_LOG_STREAM_FATAL <<
            "Unsupported log version. Copy out the data and copy it back in." <<
        KFS_LOG_EOM;
        return -1;
    }
    Restore();
    return 0;
}

//
// On a restart, whatever chunks were dirty need to be nuked: we may
// have had writes pending to them and we never flushed them to disk.
//
void
ChunkManager::RemoveDirtyChunks()
{
    for (ChunkDirs::iterator it = mChunkDirs.begin();
            it != mChunkDirs.end();
            ++it) {
        if (it->availableSpace < 0) {
            continue;
        }
        const string dir = it->dirname + mDirtyChunksDir;
        DIR* const dirStream = opendir(dir.c_str());
        if (! dirStream) {
            const int err = errno;
            KFS_LOG_STREAM_ERROR <<
                "unable to open " << dir <<
                " error: " << QCUtils::SysError(err) <<
                KFS_LOG_EOM;
            continue;
        }
        struct dirent const* dent;
        while ((dent = readdir(dirStream))) {
            const string name = dir + dent->d_name;
            struct stat buf;
            if (stat(name.c_str(), &buf) || ! S_ISREG(buf.st_mode)) {
                continue;
            }
            KFS_LOG_STREAM_INFO <<
                "Cleaning out dirty chunk: " << name <<
            KFS_LOG_EOM;
            if (unlink(name.c_str())) {
                const int err = errno;
                KFS_LOG_STREAM_ERROR <<
                    "unable to remove " << name <<
                    " error: " << QCUtils::SysError(err) <<
                KFS_LOG_EOM;
            }
        }
        closedir(dirStream);
    }
}

void
ChunkManager::Restore()
{
    RemoveDirtyChunks();
    bool scheduleEvacuateFlag = false;
    for (ChunkDirs::iterator it = mChunkDirs.begin();
            it != mChunkDirs.end();
            ++it) {
        if (it->availableSpace < 0) {
            continue;
        }
        const string& dir = it->dirname;
        if (! mEvacuateDoneFileName.empty()) {
            const string name(dir + mEvacuateDoneFileName);
            struct stat buf;
            if (stat(name.c_str(), &buf) == 0) {
                KFS_LOG_STREAM_INFO <<
                    "ignoring directory: " << dir <<
                    " file: " << mEvacuateDoneFileName << " exists" <<
                KFS_LOG_EOM;
                it->availableSpace = -1;
                continue;
            }
            const int err = errno;
            if (err != ENOENT) {
                KFS_LOG_STREAM_INFO <<
                    "ignoring directory: " << dir <<
                    " file: " << mEvacuateDoneFileName <<
                    " error: " << QCUtils::SysError(err) <<
                KFS_LOG_EOM;
                it->availableSpace = -1;
                continue;
            }
        }
        DIR* const dirStream = opendir(dir.c_str());
        if (! dirStream) {
            const int err = errno;
            KFS_LOG_STREAM_ERROR <<
                "unable to open directory: " << dir <<
                " error: " << QCUtils::SysError(err) <<
            KFS_LOG_EOM;
            it->availableSpace = -1;
            continue;
        }
        struct dirent const* dent;
        while ((dent = readdir(dirStream))) {
            if (dent->d_name == mEvacuateFileName) {
                KFS_LOG_STREAM_INFO <<
                    "evacuate directory: " << dir <<
                    " file: " << mEvacuateFileName << " exists" <<
                KFS_LOG_EOM;
                it->evacuateFlag = true;
                scheduleEvacuateFlag = true;
            }
            if (dent->d_name == mChunkDirLockName) {
                continue;
            }
            string const name(dir + dent->d_name);
            struct stat buf;
            if (stat(name.c_str(), &buf)) {
                const int err = errno;
                KFS_LOG_STREAM_INFO <<
                    "ignoring directory entry: " << name <<
                    " error: " << QCUtils::SysError(err) <<
                KFS_LOG_EOM;
            } else if (S_ISREG(buf.st_mode)) {
                AddMapping(*it, dent->d_name, buf.st_size);
            }
        }
        closedir(dirStream);
    }
    if (scheduleEvacuateFlag) {
        UpdateCountFsSpaceAvailableFlags();
        for (ChunkDirs::iterator it = mChunkDirs.begin();
                it != mChunkDirs.end(); ++it) {
            if (it->evacuateFlag) {
                it->ScheduleEvacuate();
            }
        }
    }
    mDirChecker.SetRemoveFilesFlag(mCleanupChunkDirsFlag);
}

void
ChunkManager::AddMapping(ChunkInfoHandle *cih)
{
    bool newEntryFlag = false;
    ChunkInfoHandle** const ci = mChunkTable.Insert(
        cih->chunkInfo.chunkId, cih, newEntryFlag);
    if (! ci) {
        die("add mapping failure");
    }
    if (! newEntryFlag) {
        *ci = cih;
    }
    mUsedSpace += cih->chunkInfo.chunkSize;
    UpdateDirSpace(cih, cih->chunkInfo.chunkSize);
}

void
ChunkManager::GetHostedChunks(
    vector<ChunkInfo_t> &stable,
    vector<ChunkInfo_t> &notStable,
    vector<ChunkInfo_t> &notStableAppend)
{
    // walk thru the table and pick up the chunk-ids
    mChunkTable.First();
    const CMapEntry* p;
    while ((p = mChunkTable.Next())) {
        const ChunkInfoHandle* const cih = p->GetVal();
        if (cih->isBeingReplicated) {
            // Do not report replicated chunks, replications should be canceled
            // on reconnect.
            continue;
        }
        if (cih->IsRenameInFlight()) {
            // Tell meta server the target version. It comes here when the
            // meta server connection breaks while make stable or version change
            // is in flight.
            // Report the target version and status, otherwise meta server might
            // think that this is stale chunk copy, and delete it.
            // This creates time gap with the client: the chunk still might be
            // transitioning when the read comes. In such case the chunk will
            // not be "readable" and the client will be asked to come back later.
            bool stableFlag = false;
            const kfsSeq_t vers = cih->GetTargetStateAndVersion(stableFlag);
            vector<ChunkInfo_t>& dest = stableFlag ? stable :
                (cih->IsWriteAppenderOwns() ? notStableAppend : notStable);
            dest.push_back(cih->chunkInfo);
            dest.back().chunkVersion = vers;
        } else {
            (IsChunkStable(cih) ? stable :
            (cih->IsWriteAppenderOwns() ?
                notStableAppend : notStable
            )).push_back(cih->chunkInfo);
        }
    }
}

int
ChunkManager::GetChunkInfoHandle(kfsChunkId_t chunkId, ChunkInfoHandle **cih)
{
    ChunkInfoHandle** const ci = mChunkTable.Find(chunkId);
    if (! ci) {
        *cih = 0;
        return -EBADF;
    }
    *cih = *ci;
    return 0;
}

int
ChunkManager::AllocateWriteId(WriteIdAllocOp *wi, int replicationPos, ServerLocation peerLoc)
{
    ChunkInfoHandle *cih = 0;

    if (GetChunkInfoHandle(wi->chunkId, &cih) < 0) {
        wi->statusMsg = "no such chunk";
        wi->status = -EBADF;
    } else if (wi->chunkVersion != cih->chunkInfo.chunkVersion) {
        wi->statusMsg = "chunk version mismatch";
        wi->status = -EINVAL;
    } else if (wi->isForRecordAppend && IsWritePending(wi->chunkId)) {
        wi->statusMsg = "random write in progress";
        wi->status = -EINVAL;
    } else if (wi->isForRecordAppend && ! IsWriteAppenderOwns(wi->chunkId)) {
        wi->statusMsg = "not open for append";
        wi->status = -EINVAL;
    } else if (! wi->isForRecordAppend && cih->IsWriteAppenderOwns()) {
        wi->statusMsg = "write append in progress";
        wi->status = -EINVAL;
    } else {
        mWriteId++;
        wi->writeId = mWriteId;
        if (wi->isForRecordAppend) {
            gAtomicRecordAppendManager.AllocateWriteId(
                wi, replicationPos, peerLoc, cih->dataFH);
        } else if (cih->IsStable()) {
            wi->statusMsg = "chunk stable";
            wi->status = -EINVAL;
        } else if (cih->IsRenameInFlight()) {
            wi->statusMsg = "chunk state transition is in progress";
            wi->status = -EAGAIN;
        } else {
            WriteOp* const op = new WriteOp(
                wi->seq, wi->chunkId, wi->chunkVersion,
                wi->offset, wi->numBytes, NULL, mWriteId
            );
            op->enqueueTime     = globalNetManager().Now();
            op->isWriteIdHolder = true;
            mPendingWrites.push_back(op);
        }
    }
    if (wi->status != 0) {
        KFS_LOG_STREAM_ERROR <<
            "failed: " << wi->Show() <<
        KFS_LOG_EOM;
    }
    return wi->status;
}

int64_t
ChunkManager::GetChunkVersion(kfsChunkId_t c)
{
    ChunkInfoHandle *cih;

    if (GetChunkInfoHandle(c, &cih) < 0)
        return -1;

    return cih->chunkInfo.chunkVersion;
}

WriteOp *
ChunkManager::CloneWriteOp(int64_t writeId)
{
    WriteOp* const other = mPendingWrites.find(writeId);
    if (! other || other->status < 0) {
        // if the write is "bad" already, don't add more data to it
        if (other) {
            KFS_LOG_STREAM_ERROR <<
                "clone write op failed due to status: " << other->status <<
            KFS_LOG_EOM;
        }
        return 0;
    }

    // Since we are cloning, "touch" the time
    other->enqueueTime = globalNetManager().Now();
    // offset/size/buffer are to be filled in
    return new WriteOp(other->seq, other->chunkId, other->chunkVersion,
                     0, 0, NULL, other->writeId);
}

void
ChunkManager::SetWriteStatus(int64_t writeId, int status)
{
    WriteOp* const op = mPendingWrites.find(writeId);
    if (! op) {
        return;
    }
    op->status = status;

    KFS_LOG_STREAM_INFO <<
        "setting the status of writeid: " << writeId << " to " << status <<
    KFS_LOG_EOM;
}

int
ChunkManager::GetWriteStatus(int64_t writeId)
{
    const WriteOp* const op = mPendingWrites.find(writeId);
    return (op ? op->status : -EINVAL);
}

void
ChunkManager::RunStaleChunksQueue(bool completionFlag)
{
    if (completionFlag) {
        assert(mStaleChunkOpsInFlight > 0);
        mStaleChunkOpsInFlight--;
    }
    ChunkList::Iterator it(mChunkInfoLists[kChunkStaleList]);
    ChunkInfoHandle* cih;
    while (mStaleChunkOpsInFlight < mMaxStaleChunkOpsInFlight &&
            (cih = it.Next())) {
        // If the chunk with target version already exists, then do not issue
        // delete.
        // If the existing chunk is already stable but the chunk to delete has
        // the same version but it is not stable, then the file is likely have
        // already been deleted , when the existing chunk transitioned into
        // stable version. If not then unstable chunk will be cleaned up on the
        // next restart.
        ChunkInfoHandle** const ci = mChunkTable.Find(cih->chunkInfo.chunkId);
        if (! ci ||
                ! (*ci)->CanHaveVersion(cih->chunkInfo.chunkVersion)) {
            if (cih->IsKeep()) {
                if (MarkChunkStale(cih, &mStaleChunkCompletion) == 0) {
                    mStaleChunkOpsInFlight++;
                }
            } else {
                const string fileName = MakeChunkPathname(cih);
                string err;
                const bool ok = DiskIo::Delete(
                    fileName.c_str(), &mStaleChunkCompletion, &err);
                if (ok) {
                    mStaleChunkOpsInFlight++;
                }
                KFS_LOG_STREAM(ok ?
                        MsgLogger::kLogLevelINFO :
                        MsgLogger::kLogLevelERROR) <<
                    "deleting stale chunk: " << fileName <<
                    (ok ? " ok" : " error: ") << err <<
                    " in flight: " << mStaleChunkOpsInFlight <<
                KFS_LOG_EOM;
            }
        }
        const int64_t size = min(mUsedSpace, cih->chunkInfo.chunkSize);
        UpdateDirSpace(cih, -size);
        mUsedSpace -= size;
        Delete(*cih);
    }
}

void
ChunkManager::Timeout()
{
    const time_t now = globalNetManager().Now();

    if (now >= mNextCheckpointTime) {
        mNextCheckpointTime = globalNetManager().Now() + mCheckpointIntervalSecs;
        // if any writes have been around for "too" long, remove them
        // and reclaim memory
        ScavengePendingWrites(now);
        // cleanup inactive fd's and thereby free up fd's
        CleanupInactiveFds(now);
    }
    if (mNextChunkDirsCheckTime < now) {
        // once in a while check that the drives hosting the chunks are good.
        CheckChunkDirs();
        mNextChunkDirsCheckTime = now + mChunkDirsCheckIntervalSecs;
    }
    if (mNextGetFsSpaceAvailableTime < now) {
        GetFsSpaceAvailable();
        mNextGetFsSpaceAvailableTime = now + mGetFsSpaceAvailableIntervalSecs;
    }
    gLeaseClerk.Timeout();
    gAtomicRecordAppendManager.Timeout();
}

void
ChunkManager::ScavengePendingWrites(time_t now)
{
    const time_t opExpireTime = now - mMaxPendingWriteLruSecs;

    while (! mPendingWrites.empty()) {
        WriteOp* const op = mPendingWrites.front();
        // The list is sorted by enqueue time
        if (opExpireTime < op->enqueueTime) {
            break;
        }
        // if it exceeds 5 mins, retire the op
        KFS_LOG_STREAM_DEBUG <<
            "Retiring write with id=" << op->writeId <<
            " as it has been too long" <<
        KFS_LOG_EOM;
        mPendingWrites.pop_front();

        ChunkInfoHandle *cih;
        if (GetChunkInfoHandle(op->chunkId, &cih) == 0) {
            if (now - cih->lastIOTime >= mInactiveFdsCleanupIntervalSecs) {
                // close the chunk only if it is inactive
                CloseChunk(cih);
                // CloseChunk never deletes cih
            }
            if (cih->IsFileOpen() &&
                    ! ChunkLru::IsInList(mChunkInfoLists[kChunkLruList], *cih)) {
                LruUpdate(*cih);
            }
        }
        delete op;
    }
}

int
ChunkManager::Sync(WriteOp *op)
{
    if (!op->diskIo) {
        return -1;
    }
    return op->diskIo->Sync(op->waitForSyncDone);
}

void
ChunkManager::CleanupInactiveFds(time_t now)
{
    const bool periodic = now > 0;
    // if we haven't cleaned up in 5 mins or if we too many fd's that
    // are open, clean up.
    if (periodic) {
        if (now < mNextInactiveFdCleanupTime) {
            return;
        }
    } else {
        const uint64_t openChunkCnt = globals().ctrOpenDiskFds.GetValue();
        if (openChunkCnt < (uint64_t)mMaxOpenChunkFiles &&
                    openChunkCnt * mFdsPerChunk +
                        globals().ctrOpenNetFds.GetValue() <
                    (uint64_t)mMaxOpenFds) {
            return;
        }
    }

    const time_t cur = periodic ? now : globalNetManager().Now();
    // either we are periodic cleaning or we have too many FDs open
    // shorten the interval if we're out of fd.
    const time_t expireTime = cur - (periodic ?
        mInactiveFdsCleanupIntervalSecs :
        (mInactiveFdsCleanupIntervalSecs + 2) / 3);
    ChunkLru::Iterator it(mChunkInfoLists[kChunkLruList]);
    ChunkInfoHandle* cih;
    while ((cih = it.Next()) && cih->lastIOTime < expireTime) {
        if (! cih->IsFileOpen() || cih->isBeingReplicated) {
            // Doesn't belong here, if / when io completes it will be added back.
            ChunkLru::Remove(mChunkInfoLists[kChunkLruList], *cih);
            continue;
        }
        bool inUse;
        bool hasLease = false;
        if ((inUse = cih->IsFileInUse()) ||
                (hasLease = gLeaseClerk.IsLeaseValid(cih->chunkInfo.chunkId)) ||
                IsWritePending(cih->chunkInfo.chunkId)) {
            KFS_LOG_STREAM_DEBUG << "cleanup: stale entry in chunk lru:"
                " fileid: "   << (const void*)cih->dataFH.get() <<
                " chunk: "    << cih->chunkInfo.chunkId <<
                " last io: " << (now - cih->lastIOTime) << " sec. ago" <<
                (inUse ?    " file in use" : "") <<
                (hasLease ? " has lease"   : "") <<
            KFS_LOG_EOM;
            continue;
        }
        if (cih->SyncMeta()) {
            continue;
        }
        // we have a valid file-id and it has been over 5 mins since we last did
        // I/O on it.
        KFS_LOG_STREAM_DEBUG << "cleanup: closing"
            " fileid: "  << (const void*)cih->dataFH.get() <<
            " chunk: "   << cih->chunkInfo.chunkId <<
            " last io: " << (now - cih->lastIOTime) << " sec. ago" <<
        KFS_LOG_EOM;
        Release(*cih);
    }
    cih = ChunkLru::Front(mChunkInfoLists[kChunkLruList]);
    mNextInactiveFdCleanupTime = mInactiveFdsCleanupIntervalSecs +
        ((cih && cih->lastIOTime > expireTime) ? cih->lastIOTime : cur);
}

bool
ChunkManager::StartDiskIo()
{
    if ((int)KFS_CHUNK_HEADER_SIZE < IOBufferData::GetDefaultBufferSize()) {
        KFS_LOG_STREAM_INFO <<
            "invalid io buffer size: " <<
                IOBufferData::GetDefaultBufferSize() <<
            " exceeds chunk header size: " << KFS_CHUNK_HEADER_SIZE <<
        KFS_LOG_EOM;
        return false;
    }
    mDirChecker.SetLockFileName(mChunkDirLockName);
    mDirChecker.SetRemoveFilesFlag(false);
    for (ChunkDirs::iterator it = mChunkDirs.begin();
            it < mChunkDirs.end(); ++it) {
        mDirChecker.Add(it->dirname);
    }
    mDirChecker.SetInterval(mChunkDirsCheckIntervalSecs * 1000);
    mDirChecker.AddSubDir(mStaleChunksDir);
    mDirChecker.AddSubDir(mDirtyChunksDir);
    DirChecker::DirsAvailable dirs;
    mDirChecker.Start(dirs);
    for (ChunkDirs::iterator it = mChunkDirs.begin();
            it != mChunkDirs.end();
            ++it) {
        DirChecker::DirsAvailable::const_iterator const dit =
            dirs.find(it->dirname);
        if (dit == dirs.end()) {
            KFS_LOG_STREAM_INFO << it->dirname <<
                ": not using" <<
                KFS_LOG_EOM;
            it->availableSpace = -1;
            NotifyMetaChunksLost(*it);
            continue;
        }
        // UpdateCountFsSpaceAvailableFlags() below will set the following flag.
        it->countFsSpaceAvailableFlag = false;
        it->deviceId                  = dit->second.first;
        it->dirLock                   = dit->second.second;
        it->availableSpace            = 0;
        it->totalSpace                = it->usedSpace;
        string errMsg;
        if (! DiskIo::StartIoQueue(
                it->dirname.c_str(),
                it->deviceId,
                mMaxOpenChunkFiles,
                &errMsg)) {
            KFS_LOG_STREAM_ERROR <<
                "Failed to start disk queue for: " << it->dirname <<
                " dev: << " << it->deviceId << " :" << errMsg <<
            KFS_LOG_EOM;
            DiskIo::Shutdown();
            return false;
        }
        if (! (it->diskQueue = DiskIo::FindDiskQueue(it->dirname.c_str()))) {
            die(it->dirname + ": failed to find disk queue");
        }
        KFS_LOG_STREAM_INFO <<
            "chunk directory: " << it->dirname <<
            " devId: "          << it->deviceId <<
            " space:"
            " available: "      << it->availableSpace <<
            " used: "           << it->usedSpace <<
        KFS_LOG_EOM;
    }
    mMaxIORequestSize = min(CHUNKSIZE, DiskIo::GetMaxRequestSize());
    UpdateCountFsSpaceAvailableFlags();
    GetFsSpaceAvailable();
    return true;
}

int64_t
ChunkManager::GetTotalSpace(int64_t& totalFsSpace, int& chunkDirs,
    int& evacuateInFlightCount, int& writableDirs,
    int& evacuateChunks, int64_t& evacuateByteCount,
    int* evacuateDoneChunkCount, int64_t* evacuateDoneByteCount,
    HelloMetaOp::LostChunkDirs* lostChunkDirs)
{
    totalFsSpace           = 0;
    chunkDirs              = 0;
    writableDirs           = 0;
    evacuateInFlightCount  = 0;
    evacuateChunks         = 0;
    evacuateByteCount      = 0;
    int     evacuateDoneChunks     = 0;
    int64_t evacuateDoneBytes      = 0;
    int64_t totalFsAvailableSpace  = 0;
    int64_t usedSpace              = 0;
    for (ChunkDirs::const_iterator it = mChunkDirs.begin();
            it < mChunkDirs.end(); ++it) {
        if (it->availableSpace < 0) {
            if (lostChunkDirs) {
                lostChunkDirs->insert(lostChunkDirs->end(), it->dirname);
            }
            continue;
        }
        if (it->evacuateFlag) {
            // Never send evacuate count to the meta server <= 0 while
            // evacuation is in progress -- the meta server clears evacuation
            // queue when counter is 0.
            // The counter can be sent on heartbeat, while evacuation response
            // in flight, so the two can potentially get out of sync.
            evacuateInFlightCount  += max(1, it->evacuateInFlightCount);
            evacuateChunks         += it->chunkCount;
            evacuateByteCount      += it->usedSpace;
            evacuateDoneChunks     += it->GetEvacuateDoneChunkCount();
            evacuateDoneBytes      += it->GetEvacuateDoneByteCount();
        } else {
            if (it->availableSpace > mMinFsAvailableSpace &&
                    it->availableSpace >
                        it->totalSpace * mMaxSpaceUtilizationThreshold) {
                writableDirs++;
            }
        }
        chunkDirs++;
        if (it->countFsSpaceAvailableFlag) {
            totalFsSpace += it->totalSpace;
            if (it->availableSpace > mMinFsAvailableSpace) {
                totalFsAvailableSpace +=
                    it->availableSpace - mMinFsAvailableSpace;
            }
        }
        usedSpace += it->usedSpace;
        KFS_LOG_STREAM_DEBUG <<
            "chunk directory: " << it->dirname <<
            " has space "       << it->availableSpace <<
            " total: "          << totalFsAvailableSpace <<
            " used: "           << usedSpace <<
            " limit: "          << mTotalSpace <<
        KFS_LOG_EOM;
    }
    if (evacuateDoneChunkCount) {
        *evacuateDoneChunkCount = evacuateDoneChunks;
    }
    if (evacuateDoneByteCount) {
        *evacuateDoneByteCount = evacuateDoneBytes;
    }
    return (min(totalFsAvailableSpace, mTotalSpace) + mUsedSpace);
}

int
ChunkManager::ChunkDirInfo::CheckDirReadableDone(int code, void* data)
{
    if ((code != EVENT_DISK_CHECK_DIR_READABLE_DONE &&
            code != EVENT_DISK_ERROR) || ! checkDirReadableFlightFlag) {
        die("CheckDirReadableDone invalid completion");
    }

    checkDirReadableFlightFlag = false;
    if (availableSpace < 0) {
        return 0; // Ignore, already marked not in use.
    }

    if (code == EVENT_DISK_ERROR) {
        DiskError(*reinterpret_cast<int*>(data));
    } else {
        KFS_LOG_STREAM_DEBUG <<
            "chunk directory: " << dirname << " is readable"
            " space: " << availableSpace <<
            " used: "  << usedSpace <<
            " dev: "   << deviceId <<
            " queue: " << (const void*)diskQueue <<
        KFS_LOG_EOM;
        diskTimeoutCount = 0;
    }
    return 0;
}

int
ChunkManager::ChunkDirInfo::FsSpaceAvailDone(int code, void* data)
{
    if ((code != EVENT_DISK_GET_FS_SPACE_AVAIL_DONE &&
            code != EVENT_DISK_ERROR) || ! fsSpaceAvailInFlightFlag) {
        die("FsSpaceAvailDone invalid completion");
    }

    fsSpaceAvailInFlightFlag = false;
    if (availableSpace < 0) {
        return 0; // Ignore, already marked not in use.
    }

    if (code == EVENT_DISK_ERROR) {
        DiskError(*reinterpret_cast<int*>(data));
    } else {
        if (availableSpace >= 0) {
            const int64_t* const ret =
                reinterpret_cast<const int64_t*>(data);
            const int64_t fsAvail = ret[0];
            const int64_t fsTotal = ret[1];
            KFS_LOG_STREAM_DEBUG <<
                "chunk directory: " << dirname <<
                " available: "      << availableSpace <<
                " => "              << fsAvail <<
                " total: "          << totalSpace <<
                " => "              << fsTotal <<
                " used: "           << usedSpace <<
            KFS_LOG_EOM;
            availableSpace = max(int64_t(0), fsAvail);
            totalSpace     = max(int64_t(0), fsTotal);
        }
        diskTimeoutCount = 0;
    }
    return 0;
}

void
ChunkManager::ChunkDirInfo::DiskError(int sysErr)
{
    if (availableSpace < 0) {
        return; // Ignore, already marked not in use.
    }
    KFS_LOG_STREAM_ERROR <<
        "chunk directory: " << dirname <<
        " error: "          << QCUtils::SysError(-sysErr) <<
        " space:"
        " available: "      << availableSpace <<
        " used: "           << usedSpace <<
    KFS_LOG_EOM;
    if ((sysErr != -EMFILE && sysErr != -ENFILE) &&
            (sysErr != -ETIMEDOUT || ++diskTimeoutCount >
            gChunkManager.GetMaxDirCheckDiskTimeouts())) {
        gChunkManager.NotifyMetaChunksLost(*this);
    }
}

int
ChunkManager::ChunkDirInfo::CheckEvacuateFileDone(int code, void* data)
{
    if ((code != EVENT_DISK_GET_FS_SPACE_AVAIL_DONE &&
            code != EVENT_DISK_ERROR) || ! checkEvacuateFileInFlightFlag) {
        die("CheckEvacuateFileDone invalid completion");
    }

    checkEvacuateFileInFlightFlag = false;
    if (availableSpace < 0) {
        return 0; // Ignore, already marked not in use.
    }

    if (code == EVENT_DISK_ERROR) {
        const int sysErr = *reinterpret_cast<int*>(data);
        KFS_LOG_STREAM(sysErr == -ENOENT ?
                MsgLogger::kLogLevelDEBUG :
                MsgLogger::kLogLevelERROR) <<
            "chunk directory: " << dirname <<
            " \"evacuate\""
            " error: " << QCUtils::SysError(-sysErr) <<
            " space: " << availableSpace <<
            " used: "  << usedSpace <<
            " dev: "   << deviceId <<
            " queue: " << (const void*)diskQueue <<
        KFS_LOG_EOM;
        if (sysErr == -EIO) {
            if (++evacuateCheckIoErrorsCount >=
                    gChunkManager.GetMaxEvacuateIoErrors()) {
                DiskError(sysErr);
            }
        } else {
            evacuateCheckIoErrorsCount = 0;
        }
    } else if (! evacuateFlag) {
        KFS_LOG_STREAM_INFO <<
            "chunk directory: " << dirname <<
            " \"evacuate\""
            " space: " << availableSpace <<
            " used: "  << usedSpace <<
            " dev: "   << deviceId <<
            " queue: " << (const void*)diskQueue <<
        KFS_LOG_EOM;
        diskTimeoutCount = 0;
        evacuateFlag     = true;
        ScheduleEvacuate();
    }
    return 0;
}

int
ChunkManager::ChunkDirInfo::EvacuateChunksDone(int code, void* data)
{
    if (code != EVENT_CMD_DONE || data != &evacuateChunksOp ||
            ! evacuateChunksOpInFlightFlag) {
        die("EvacuateChunksDone invalid completion");
    }

    evacuateChunksOpInFlightFlag = false;
    if (availableSpace < 0) {
        return 0; // Ignore, already marked not in use.
    }

    if (! evacuateFlag) {
        return 0;
    }
    UpdateLastEvacuationActivityTime();
    if (evacuateChunksOp.status != 0) {
        if (! evacuateStartedFlag && evacuateChunksOp.status == -EAGAIN) {
            SetEvacuateStarted();
        }
        if (! evacuateStartedFlag || (evacuateInFlightCount <= 0 &&
                (evacuateChunksOp.status != -EAGAIN ||
                    evacuateChunksOp.numChunks <= 1))) {
            // Restart from the evacuate file check, in order to try again with
            // a delay.
            if (! ChunkDirList::IsEmpty(chunkLists[kChunkDirEvacuateList])) {
                die("non empty evacuate list");
            }
            evacuateStartedFlag = false;
            evacuateFlag        = false;
            KFS_LOG_STREAM_WARN <<
                "evacuate: " << dirname <<
                " status: "  << evacuateChunksOp.status <<
                " restarting from evacuation file check" <<
            KFS_LOG_EOM;
        }
        if (evacuateStartedFlag == countFsSpaceAvailableFlag) {
            gChunkManager.UpdateCountFsSpaceAvailableFlags();
        }
        rescheduleEvacuateThreshold = max(0,
            evacuateInFlightCount - max(0, evacuateChunksOp.numChunks));
        if (evacuateInFlightCount <= 0 && evacuateStartedFlag) {
            // Do one chunk at a time if we get -EAGAIN and no
            // evacuations are in flight at the moment.
            ScheduleEvacuate(1);
        }
        return 0;
    }

    SetEvacuateStarted();
    if (countFsSpaceAvailableFlag) {
        gChunkManager.UpdateCountFsSpaceAvailableFlags();
    }
    // Minor optimization: try to traverse the chunk list first, it likely
    // that all chunks that were scheduled for evacuation are still in the list
    // in the same order that they were scheduled.
    ChunkDirList::Iterator it(chunkLists[kChunkDirList]);
    int i;
    for (i = 0; i < evacuateChunksOp.numChunks; i++) {
        ChunkInfoHandle* const cih = it.Next();
        if (! cih || cih->chunkInfo.chunkId != evacuateChunksOp.chunkIds[i]) {
            break;
        }
        cih->SetEvacuate(true);
    }
    for ( ; i < evacuateChunksOp.numChunks; i++) {
        ChunkInfoHandle* cih;
        if (gChunkManager.GetChunkInfoHandle(
                evacuateChunksOp.chunkIds[i], &cih) == 0 &&
                &(cih->GetDirInfo()) == this) {
            cih->SetEvacuate(true);
        }
    }
    ScheduleEvacuate();
    return 0;
}

int
ChunkManager::ChunkDirInfo::RenameEvacuateFileDone(int code, void* data)
{
    if ((code != EVENT_DISK_RENAME_DONE &&
            code != EVENT_DISK_ERROR) || ! evacuateFileRenameInFlightFlag) {
        die("RenameEvacuateFileDone invalid completion");
    }

    evacuateFileRenameInFlightFlag = false;
    if (availableSpace < 0) {
        return 0; // Ignore, already marked not in use.
    }

    if (code == EVENT_DISK_ERROR) {
        DiskError(*reinterpret_cast<int*>(data));
    } else {
        KFS_LOG_STREAM_DEBUG <<
            "chunk directory: " << dirname << " evacuation done"
            " space: " << availableSpace <<
            " used: "  << usedSpace <<
            " dev: "   << deviceId <<
            " queue: " << (const void*)diskQueue <<
        KFS_LOG_EOM;
        diskTimeoutCount = 0;
        evacuateDoneFlag = true;
        gChunkManager.NotifyMetaChunksLost(*this);
    }
    return 0;
}

void
ChunkManager::ChunkDirInfo::ScheduleEvacuate(
    int maxChunkCount)
{
    if (availableSpace < 0) {
        return; // Ignore, already marked not in use.
    }

    if (evacuateChunksOpInFlightFlag || ! evacuateFlag ||
            ! globalNetManager().IsRunning()) {
        return;
    }
    if (evacuateStartedFlag &&
            ChunkDirList::IsEmpty(chunkLists[kChunkDirList])) {
        if (evacuateInFlightCount > 0 ||
                ! ChunkDirList::IsEmpty(chunkLists[kChunkDirEvacuateList])) {
            return;
        }
        if (evacuateDoneFlag || evacuateFileRenameInFlightFlag) {
            return;
        }
        if (gChunkManager.GetEvacuateFileName().empty() ||
                gChunkManager.GetEvacuateDoneFileName().empty()) {
            evacuateDoneFlag = true;
            return;
        }
        const string src = dirname + gChunkManager.GetEvacuateFileName();
        const string dst = dirname + gChunkManager.GetEvacuateDoneFileName();
        string       statusMsg;
        evacuateFileRenameInFlightFlag = true;
        if (! DiskIo::Rename(
                    src.c_str(),
                    dst.c_str(),
                    &renameEvacuateFileCb,
                    &statusMsg)) {
            KFS_LOG_STREAM_ERROR <<
               "evacuate done rename " <<
               src << " to " << dst <<
               " " << statusMsg <<
            KFS_LOG_EOM;
            evacuateFileRenameInFlightFlag = false; // Retry later
        }
        return;
    }
    if (evacuateStartedFlag) {
        evacuateChunksOp.totalSpace            = -1;
        evacuateChunksOp.totalFsSpace          = -1;
        evacuateChunksOp.usedSpace             = -1;
        evacuateChunksOp.chunkDirs             = -1;
        evacuateChunksOp.writableChunkDirs     = -1;
        evacuateChunksOp.evacuateInFlightCount = -1;
        evacuateChunksOp.numChunks             = 0;
        evacuateChunksOp.evacuateChunks        = -1;
        evacuateChunksOp.evacuateByteCount     = -1;
        const int maxCnt = maxChunkCount > 0 ?
            min(int(EvacuateChunksOp::kMaxChunkIds), maxChunkCount) :
            EvacuateChunksOp::kMaxChunkIds;
        ChunkDirList::Iterator it(chunkLists[kChunkDirList]);
        ChunkInfoHandle*       cih;
        while (evacuateChunksOp.numChunks < maxCnt && (cih = it.Next())) {
            evacuateChunksOp.chunkIds[evacuateChunksOp.numChunks++] =
                cih->chunkInfo.chunkId;
        }
    } else {
        KFS_LOG_STREAM_WARN <<
            "evacuate: " << dirname <<
            " starting" <<
        KFS_LOG_EOM;
        // On the first evacuate update the meta server space, in order to
        // to prevent chunk allocation failures.
        // When the response comes back the evacuate started flag is set to
        // true.
        const bool updateFlag = countFsSpaceAvailableFlag;
        SetEvacuateStarted();
        if (updateFlag) {
            gChunkManager.UpdateCountFsSpaceAvailableFlags();
        }
        evacuateChunksOp.totalSpace = gChunkManager.GetTotalSpace(
            evacuateChunksOp.totalFsSpace,
            evacuateChunksOp.chunkDirs,
            evacuateChunksOp.evacuateInFlightCount,
            evacuateChunksOp.writableChunkDirs,
            evacuateChunksOp.evacuateChunks,
            evacuateChunksOp.evacuateByteCount
        );
        evacuateChunksOp.usedSpace = gChunkManager.GetUsedSpace();
        evacuateStartedFlag = false;
        if (updateFlag) {
            gChunkManager.UpdateCountFsSpaceAvailableFlags();
        }
    }
    UpdateLastEvacuationActivityTime();
    // Submit op even if the chunk list is empty in order to update meta
    // server's free space counters.
    evacuateChunksOpInFlightFlag = true;
    evacuateChunksOp.status = 0;
    gMetaServerSM.EnqueueOp(&evacuateChunksOp);
}

void
ChunkManager::ChunkDirInfo::RestartEvacuation()
{
    if (availableSpace < 0) {
        return; // Ignore, already marked not in use.
    }
    if (! evacuateStartedFlag) {
        return;
    }
    KFS_LOG_STREAM_WARN <<
        "evacuate: " << dirname <<
        " restarting"
        " in flight: " << evacuateInFlightCount <<
    KFS_LOG_EOM;
    ChunkDirInfo::ChunkLists& list = chunkLists[kChunkDirEvacuateList];
    ChunkInfoHandle*          cih;
    while ((cih = ChunkDirList::Front(list))) {
        cih->SetEvacuate(false);
    }
    ScheduleEvacuate();
}

void
ChunkManager::MetaServerConnectionLost()
{
    mMetaEvacuateCount = -1;
    for (ChunkDirs::iterator it = mChunkDirs.begin();
            it < mChunkDirs.end(); ++it) {
        if (it->availableSpace < 0 || ! it->evacuateFlag) {
            continue;
        }
        // Take directory out of allocation now. Hello will update the
        // meta server's free space parameters used in chunk placement.
        it->SetEvacuateStarted();
        if (it->countFsSpaceAvailableFlag) {
            UpdateCountFsSpaceAvailableFlags();
        }
        it->RestartEvacuation();
    }
}

long
ChunkManager::GetNumWritableChunks() const
{
    return (long)mPendingWrites.GetChunkIdCount();
}

void
ChunkManager::CheckChunkDirs()
{
    KFS_LOG_STREAM_DEBUG << "Checking chunk dirs" << KFS_LOG_EOM;

    DirChecker::DirsAvailable dirs;
    mDirChecker.GetNewlyAvailable(dirs);
    bool getFsSpaceAvailFlag = false;
    for (ChunkDirs::iterator it = mChunkDirs.begin();
            it < mChunkDirs.end(); ++it) {
        if (it->availableSpace < 0 || it->checkDirReadableFlightFlag) {
            DirChecker::DirsAvailable::const_iterator const dit =
                dirs.find(it->dirname);
            if (dit == dirs.end()) {
                continue;
            }
            if (it->checkDirReadableFlightFlag) {
                // Add it back, and wait in flight op completion.
                mDirChecker.Add(it->dirname);
                continue;
            }
            string errMsg;
            if (DiskIo::StartIoQueue(
                    it->dirname.c_str(),
                    dit->second.first,
                    mMaxOpenChunkFiles,
                    &errMsg)) {
                if (! (it->diskQueue = DiskIo::FindDiskQueue(
                        it->dirname.c_str()))) {
                    die(it->dirname + ": failed to find disk queue");
                }
                it->availableSpace             = 0;
                it->deviceId                   = dit->second.first;
                it->dirLock                    = dit->second.second;
                it->corruptedChunksCount       = 0;
                it->evacuateCheckIoErrorsCount = 0;
                ChunkDirs::const_iterator cit;
                for (cit = mChunkDirs.begin(); cit != mChunkDirs.end(); ++cit) {
                    if (cit == it || cit->availableSpace < 0) {
                        continue;
                    }
                    if (it->deviceId == cit->deviceId &&
                            it->countFsSpaceAvailableFlag) {
                        break;
                    }
                }
                it->countFsSpaceAvailableFlag = cit == mChunkDirs.end();
                KFS_LOG_STREAM_INFO <<
                    "chunk directory: "  << it->dirname <<
                    " devId: "           << it->deviceId <<
                    " space:"
                    " used: "            << it->usedSpace <<
                    " countAvail: "      << it->countFsSpaceAvailableFlag <<
                KFS_LOG_EOM;
                getFsSpaceAvailFlag = true;
                // Notify meta serve that directory is now in use.
                gMetaServerSM.EnqueueOp(
                    new CorruptChunkOp(0, -1, -1, &(it->dirname), true));
                continue;
            }
            KFS_LOG_STREAM_ERROR <<
                "failed to start disk queue for: " << it->dirname <<
                " dev: << " << it->deviceId << " :" << errMsg <<
            KFS_LOG_EOM;
            // For now do not keep trying.
            // mDirChecker.Add(it->dirname);
            continue;
        }
        string err;
        it->checkDirReadableFlightFlag = true;
        if (! DiskIo::CheckDirReadable(
                it->dirname.c_str(), &(it->checkDirReadableCb), &err)) {
            it->checkDirReadableFlightFlag = false;
            KFS_LOG_STREAM_ERROR << "failed to queue"
                " check dir readable request for: " << it->dirname <<
                " : " << err <<
            KFS_LOG_EOM;
            // Do not declare directory unusable on req. queueing failure.
            // DiskIo can be temp. out of requests.
        }
    }
    if (getFsSpaceAvailFlag) {
        GetFsSpaceAvailable();
    }
}

void
ChunkManager::GetFsSpaceAvailable()
{
    for (ChunkDirs::iterator it = mChunkDirs.begin();
            it < mChunkDirs.end(); ++it) {
        if (it->availableSpace < 0) {
            continue;
        }
        string err;
        if (! it->evacuateFlag && ! it->checkEvacuateFileInFlightFlag) {
            const string fn = it->dirname + mEvacuateFileName;
            it->checkEvacuateFileInFlightFlag = true;
            if (! DiskIo::GetFsSpaceAvailable(
                    fn.c_str(), &(it->checkEvacuateFileCb), &err)) {
                it->checkEvacuateFileInFlightFlag = false;
                KFS_LOG_STREAM_ERROR << "failed to queue "
                    "fs space available request for: " << fn <<
                    " : " << err <<
                KFS_LOG_EOM;
                // Do not declare directory unusable on req. queueing failure.
                // DiskIo can be temp. out of requests.
                continue;
            }
        }
        if (it->evacuateStartedFlag &&
                mEvacuationInactivityTimeout > 0 &&
                mMetaEvacuateCount == 0 &&
                ! it->evacuateChunksOpInFlightFlag &&
                it->evacuateInFlightCount > 0 &&
                it->lastEvacuationActivityTime + mEvacuationInactivityTimeout <
                    mMetaHeartbeatTime) {
            it->RestartEvacuation();
        }
        if (it->fsSpaceAvailInFlightFlag) {
            continue;
        }
        it->fsSpaceAvailInFlightFlag = true;
        if (! DiskIo::GetFsSpaceAvailable(
                it->dirname.c_str(), &(it->fsSpaceAvailCb), &err)) {
            it->fsSpaceAvailInFlightFlag = 0;
            KFS_LOG_STREAM_ERROR << "failed to queue "
                "fs space available request for: " << it->dirname <<
                " : " << err <<
            KFS_LOG_EOM;
            // Do not declare directory unusable on req. queueing failure.
            // DiskIo can be temp. out of requests.
        }
    }
}

void
ChunkManager::MetaHeartbeat(HeartbeatOp& op)
{
    mMetaHeartbeatTime = globalNetManager().Now();
    mMetaEvacuateCount = op.metaEvacuateCount;
}

} // namespace KFS
