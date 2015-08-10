//---------------------------------------------------------- -*- Mode: C++ -*-
//subrata mitra
// \brief Code to deal with distribued recovery.
//----------------------------------------------------------------------------

#ifndef CHUNKSERVER_DISTRIBUTEDREPAIR_H
#define CHUNKSERVER_DISTRIBUTEDREPAIR_H

#include "common/kfstypes.h"
#include <stdint.h>
#if 0
namespace KFS
{

struct ReplicateChunkOp;
struct DistributedRepairChunkOp;
class Properties;
class NetManager;

class DistributedRepair
{
public:
    static int Run(DistributedRepairChunkOp* dRCOp);
    #if 0
    struct Counters
    {
        typedef int64_t Counter;

        Counter mReplicationCount;
        Counter mReplicationErrorCount;
        Counter mReplicationCanceledCount;
        Counter mRecoveryCount;
        Counter mRecoveryErrorCount;
        Counter mRecoveryCanceledCount;
        Counter mReplicatorCount;
        Counter mReadCount;
        Counter mWriteCount;
        Counter mReadByteCount;
        Counter mWriteByteCount;
        Counters()
            : mReplicationCount(0),
              mReplicationErrorCount(0),
              mReplicationCanceledCount(0),
              mRecoveryCount(0),
              mRecoveryErrorCount(0),
              mRecoveryCanceledCount(0),
              mReplicatorCount(0),
              mReadCount(0),
              mWriteCount(0),
              mReadByteCount(0),
              mWriteByteCount(0)
            {}
        void Reset()
            { *this = Counters(); }
    };
    static void Run(ReplicateChunkOp* op);
    static int GetNumReplications();
    static void CancelAll();
    static bool Cancel(kfsChunkId_t chunkId, kfsSeq_t targeVersion);
    static void SetParameters(const Properties& props);
    static void GetCounters(Counters& counters);
    static void Shutdown();
};

class ClientThread;
class RSReplicatorEntry
{
protected:
    RSReplicatorEntry(
        ClientThread* inThreadPtr)
        : mClientThreadPtr(inThreadPtr),
          mNextPtr(0)
        {}
    virtual ~RSReplicatorEntry();
    virtual void Handle() = 0;
    void Enqueue();
    bool IsPending() const
        { return (mNextPtr != 0); }

    ClientThread* const mClientThreadPtr;
private:
    RSReplicatorEntry*  mNextPtr;

private:
    RSReplicatorEntry(
        const RSReplicatorEntry& inEntry);
    RSReplicatorEntry& operator=(
        const RSReplicatorEntry& inEntry);
friend class ClientThreadImpl;
#endif
};

}

#endif // CHUNKSERVER_DISTRIBUTEDREPAIR_H
#endif
