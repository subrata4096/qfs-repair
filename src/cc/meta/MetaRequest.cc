/*!
 * $Id$
 *
 * \file MetaRequest.cc
 * \brief Meta server request handlers.
 * \author Blake Lewis and Sriram Rao
 *         Mike Ovsiannikov
 *
 * Copyright 2008-2012 Quantcast Corp.
 * Copyright 2006-2008 Kosmix Corp.
 *
 * This file is part of Kosmos File System (KFS).
 *
 * Licensed under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include "kfstree.h"
#include "MetaRequest.h"
#include "Logger.h"
#include "Checkpoint.h"
#include "util.h"
#include "LayoutManager.h"
#include "ChildProcessTracker.h"
#include "NetDispatch.h"
#include "Restorer.h"
#include "AuditLog.h"
#include "ClientSM.h"

#include "kfsio/Globals.h"
#include "kfsio/checksum.h"
#include "kfsio/IOBufferWriter.h"
#include "kfsio/DelegationToken.h"
#include "kfsio/ChunkAccessToken.h"
#include "common/MsgLogger.h"
#include "common/RequestParser.h"
#include "common/IntToString.h"
#include "qcdio/QCUtils.h"
#include "qcdio/qcstutils.h"
#include "common/time.h"
#include "common/kfserrno.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>

#include <map>
#include <iomanip>
#include <sstream>
#include <limits>

namespace KFS {

using std::map;
using std::string;
using std::istringstream;
using std::ifstream;
using std::min;
using std::max;
using std::make_pair;
using std::numeric_limits;
using KFS::libkfsio::globals;

static bool    gWormMode = false;
static string  gChunkmapDumpDir(".");
static const char* const ftypes[] = { "empty", "file", "dir" };

static ostringstream&
GetTmpOStringStream()
{
    static ostringstream ret;
    ret.str(string());
    resetOStream(ret);
    return ret;
}

static bool
CanAccessFile(const MetaFattr* fa, MetaRequest& op)
{
    if (! fa) {
        op.status    = -ENOENT;
        op.statusMsg = "no such file";
        return false;
    }
    if (fa->IsStriped() && op.clientProtoVers <
            KFS_CLIENT_MIN_STRIPED_FILE_SUPPORT_PROTO_VERS) {
        op.status    = -EINVAL;
        op.statusMsg = "striped file, client upgrade required";
        return false;
    }
    return true;
}

/*!
 * Specially named files (such as, those that end with ".tmp") can be
 * mutated by remove/rename.  Otherwise, in WORM no deletes/renames are allowed.
 */
static inline bool
IsWormMutationAllowed(const string &pathname)
{
    return (pathname.length() >= 4 &&
        pathname.compare(pathname.length() - 4, 4, ".tmp") == 0);
}

static inline bool
startsWith(const string& str, const string& prefix)
{
    const size_t len = prefix.length();
    return (str.length() >= len && str.compare(0, len, prefix) == 0);
}

/*
 * Set WORM mode. In WORM mode, deletes are disabled.
 */
void
setWORMMode(bool value)
{
    gWormMode = value;
}

void
setChunkmapDumpDir(string d)
{
    gChunkmapDumpDir = d;
}

inline static bool
OkHeader(const MetaRequest* op, ostream &os, bool checkStatus = true)
{
    os <<
        "OK\r\n"
        "Cseq: " << op->opSeqno
    ;
    if (op->status == 0 && op->statusMsg.empty()) {
        os <<
            "\r\n"
            "Status: 0\r\n"
        ;
        return true;
    }
    os <<
        "\r\n"
        "Status: " << (op->status >= 0 ? op->status :
            -SysToKfsErrno(-op->status)) << "\r\n"
    ;
    if (! op->statusMsg.empty()) {
        const size_t p = op->statusMsg.find('\r');
        assert(
            string::npos == p &&
            op->statusMsg.find('\n') == string::npos
        );
        os << "Status-message: " <<
            (p == string::npos ?
                op->statusMsg :
                op->statusMsg.substr(0, p)) <<
        "\r\n";
    }
    if (checkStatus && op->status < 0) {
        os << "\r\n";
    }
    return (op->status >= 0);
}

inline static ostream&
PutHeader(const MetaRequest* op, ostream &os)
{
    OkHeader(op, os, false);
    return os;
}

inline static bool
IsValidUser(kfsUid_t user)
{
    return (user != kKfsUserNone);
}

inline static bool
IsValidGroup(kfsGid_t group)
{
    return (group != kKfsGroupNone);
}

inline static bool
IsValidMode(kfsMode_t mode, bool dirFlag)
{
    return (mode != kKfsModeUndef &&
        (mode & ~(dirFlag ?
            kfsMode_t(Permissions::kDirModeMask) :
            kfsMode_t(Permissions::kFileModeMask))) == 0
    );
}

inline static bool
IsGroupMember(kfsUid_t user, kfsGid_t group)
{
    return IsValidGroup(group); // The client lib does group validation
}

inline static void
SetEUserAndEGroup(MetaRequest& req)
{
    gLayoutManager.SetEUserAndEGroup(req);
}

inline static const UserAndGroupNames*
GetUserAndGroupNames(const MetaRequest& req)
{
    return (
        (req.authUid != kKfsUserNone && req.fromClientSMFlag && req.clnt) ?
        &static_cast<const ClientSM*>(req.clnt
            )->GetAuthContext().GetUserAndGroupNames() : 0
    );
}

inline static void
SetPermissionDeniedStatus(MetaRequest& op)
{
    if (0 <= op.status) {
        op.status   = -EPERM;
        if (op.statusMsg.empty()) {
            op.statusMsg = "permission denied";
        }
    }
}

inline static bool
HasMetaServerAdminAccess(MetaRequest& op)
{
    if (! gLayoutManager.HasMetaServerAdminAccess(op)) {
        SetPermissionDeniedStatus(op);
        return false;
    }
    return true;
}

inline static bool
HasMetaServerStatsAccess(MetaRequest& op)
{
    if (! gLayoutManager.HasMetaServerStatsAccess(op)) {
        SetPermissionDeniedStatus(op);
        return false;
    }
    return true;
}

template<typename T> inline bool
SetUserAndGroup(T& req)
{
    if (req.authUid != kKfsUserNone) {
        if (! req.ownerName.empty()) {
            req.user = gLayoutManager.GetUserAndGroup().GetUserId(
                req.ownerName);
            if (req.user == kKfsUserNone) {
                req.status    = -EINVAL;
                req.statusMsg = "no such user";
                return false;
            }
        }
        if (! req.groupName.empty()) {
            if (! gLayoutManager.GetUserAndGroup().GetGroupId(
                    req.groupName, req.group)) {
                req.status    = -EINVAL;
                req.statusMsg = "no such group";
                return false;
            }
        }
    }
    SetEUserAndEGroup(req);
    if (req.user != kKfsUserNone || req.group != kKfsGroupNone) {
        gLayoutManager.SetUserAndGroup(req, req.user, req.group);
    }
    return true;
}

inline static void
FattrReply(const MetaFattr* fa, MFattr& ofa)
{
    if (! fa) {
        return;
    }
    ofa = *fa;
    // Keep the following to handle purely theoretical case: "re-open" file
    // for append. Normal append now does not attempt to calculate "the actual"
    // file size as append files are sparse anyway, and always sets file size
    // equal to the begining of the "next" chunk.
    if (fa->filesize < 0 &&
            fa->type == KFS_FILE &&
            fa->chunkcount() > 0 &&
            fa->nextChunkOffset() >= (chunkOff_t)CHUNKSIZE &&
            ! fa->IsStriped()) {
        MetaChunkInfo* ci = 0;
        if (metatree.getalloc(fa->id(),
                fa->nextChunkOffset() - CHUNKSIZE, &ci) == 0 &&
                ci &&
                gLayoutManager.HasWriteAppendLease(ci->chunkId)) {
            // Reduce getlayout calls, return the same value to the
            // client as in the case of getlayout followed by
            // getsize on unstable write append chunk.
            // Chunk servers always return CHUNKSIZE in such case
            // to allow reading file opened for append while the
            // file is being written into.
            ofa.filesize = fa->nextChunkOffset();
        }
    }
}

inline static ostream&
UserAndGroupNamesReply(
    ostream&                 os,
    const UserAndGroupNames* ugn,
    kfsUid_t                 user,
    kfsGid_t                 group,
    const char* const        prefix = "")
{
    if (ugn) {
        const string* name = ugn->GetUserName(user);
        os << prefix << "UName: ";
        if (name && ! name->empty()) {
            os << *name;
        } else {
            os << user;
        }
        os << "\r\n"
            << prefix << "GName: ";
        name = ugn->GetGroupName(group);
        if (name && ! name->empty()) {
            os << *name;
        } else {
            os << group;
        }
        os << "\r\n";
    }
    return os;
}

inline static ostream&
FattrReply(ostream& os, const MFattr& fa, const UserAndGroupNames* ugn)
{
    os <<
    "File-handle: " << fa.id()         << "\r\n"
    "Type: "        << ftypes[fa.type] << "\r\n"
    "File-size: "   << fa.filesize     << "\r\n"
    "Replication: " << fa.numReplicas  << "\r\n";
    if (fa.type == KFS_FILE) {
        os << "Chunk-count: " << fa.chunkcount() << "\r\n";
    } else if (fa.type == KFS_DIR) {
        os <<
        "File-count: " << fa.fileCount() << "\r\n"
        "Dir-count: "  << fa.dirCount()  << "\r\n";
    }
    sendtime(os, "M-Time: ",  fa.mtime,  "\r\n");
    sendtime(os, "C-Time: ",  fa.ctime,  "\r\n");
    sendtime(os, "CR-Time: ", fa.crtime, "\r\n");
    if (fa.IsStriped()) {
        os <<
        "Striper-type: "         << int32_t(fa.striperType) << "\r\n"
        "Num-stripes: "          << fa.numStripes           << "\r\n"
        "Num-recovery-stripes: " << fa.numRecoveryStripes   << "\r\n"
        "Stripe-size: "          << fa.stripeSize           << "\r\n";
    }
    os <<
    "User: "  << fa.user  << "\r\n"
    "Group: " << fa.group << "\r\n"
    "Mode: "  << fa.mode  << "\r\n";
    if (fa.minSTier < kKfsSTierMax) {
        os <<
        "Min-tier: " << (int)fa.minSTier << "\r\n"
        "Max-tier: " << (int)fa.maxSTier << "\r\n";
    }
    return UserAndGroupNamesReply(os, ugn, fa.user, fa.group);
}

template<typename CondT>
class RequestWaitQueue : public ITimeout
{
public:
    RequestWaitQueue(
        NetManager& netManager,
        CondT&      cond,
        bool        ownsCondFlag = false)
        : ITimeout(),
          mCond(cond),
          mFront(0),
          mBack(0),
          mCur(0),
          mDeletedFlag(0),
          mNetManager(netManager),
          mOwnsCondFlag(ownsCondFlag),
          mMaxOpsPerLoop(2)
        {}
    virtual ~RequestWaitQueue()
    {
        if (mFront) {
            mNetManager.UnRegisterTimeoutHandler(this);
            // leave requests suspended
        }
        if (mOwnsCondFlag) {
            delete &mCond;
        }
        if (mDeletedFlag) {
            *mDeletedFlag = true;
        }
    }
    virtual void Timeout()
    {
        int  opsCount    = 0;
        bool deletedFlag = false;
        mDeletedFlag = &deletedFlag;
        while (mFront && mCond(*mFront)) {
            mCur = mFront;
            mFront = mCur->next;
            if (! mFront) {
                assert(mCur == mBack);
                mBack = 0;
            }
            mCur->next = 0;
            mCur->suspended = false;
            submit_request(mCur);
            if (deletedFlag) {
                return;
            }
            mCur = 0;
            if (mMaxOpsPerLoop <= ++opsCount) {
                if (mFront && mCond(*mFront)) {
                    mNetManager.Wakeup();
                }
                break;
            }
        }
        mDeletedFlag = 0;
        if (! mFront) {
            mNetManager.UnRegisterTimeoutHandler(this);
        }
    }
    bool SuspendIfNeeded(MetaRequest& req)
    {
        if (mCur == &req || (! HasPendingRequests() && mCond(req))) {
            return false;
        }
        Add(&req);
        return true;
    }
    void Add(MetaRequest* req)
    {
        if (! req || req->next || req == mFront || req == mBack) {
            panic("request is null "
                "or already in this or another queue", false);
            return;
        }
        req->suspended = true;
        if (mBack) {
            assert(mFront);
            mBack->next = req;
            mBack = req;
        } else {
            assert(! mFront);
            mFront = req;
            mBack  = req;
            mNetManager.RegisterTimeoutHandler(this);
        }
    }
    bool HasPendingRequests() const
        { return (mFront != 0); }
    void Wakeup()
    {
        if (! HasPendingRequests()) {
            return;
        }
        mNetManager.Wakeup();
    }
    void SetParameters(const Properties& props, const char* prefix)
    {
        mMaxOpsPerLoop = props.getValue(
            (prefix ? prefix : "") + string("maxOpsPerLoop"),
            mMaxOpsPerLoop
        );
    }
private:
    CondT&       mCond;
    MetaRequest* mFront;
    MetaRequest* mBack;
    MetaRequest* mCur;
    bool*        mDeletedFlag;
    NetManager&  mNetManager;
    bool         mOwnsCondFlag;
    int          mMaxOpsPerLoop;

    RequestWaitQueue(const RequestWaitQueue&);
    RequestWaitQueue& operator=(const RequestWaitQueue&);
};

class EnoughBuffersCond
{
public:
    bool operator() (MetaRequest& req)
    {
        return gLayoutManager.HasEnoughFreeBuffers(&req);
    }
};

static EnoughBuffersCond&
GetEnoughBuffersCond()
{
    static EnoughBuffersCond sEnoughBuffersCond;
    return sEnoughBuffersCond;
}

typedef RequestWaitQueue<EnoughBuffersCond> BuffersWaitQueue;
static BuffersWaitQueue sBuffersWaitQueue(
    globalNetManager(), GetEnoughBuffersCond());

void
CheckIfIoBuffersAvailable()
{
    if (sBuffersWaitQueue.HasPendingRequests() &&
            gLayoutManager.HasEnoughFreeBuffers()) {
        sBuffersWaitQueue.Wakeup();
    }
}

void
SetRequestParameters(const Properties& props)
{
    sBuffersWaitQueue.SetParameters(props, "metaServer.buffersWaitQueue.");
}

static bool
HasEnoughIoBuffersForResponse(MetaRequest& req)
{
    return (! sBuffersWaitQueue.SuspendIfNeeded(req));
}

class ResponseWOStream : private IOBuffer::WOStream
{
public:
    ResponseWOStream()
        : IOBuffer::WOStream()
        {}
    ostream& Set(IOBuffer& buf)
    {
        return IOBuffer::WOStream::Set(
            buf, gLayoutManager.GetMaxResponseSize());
    }
    void Reset()
        { IOBuffer::WOStream::Reset(); }
};
static ResponseWOStream sWOStream;

/* virtual */ void
MetaLookup::handle()
{
    authType = kAuthenticationTypeUndef; // always reset if op gets here.
    SetEUserAndEGroup(*this);
    MetaFattr* fa = 0;
    if ((status = metatree.lookup(dir, name, euser, egroup, fa)) == 0) {
        FattrReply(fa, fattr);
    }
}

/* virtual */ bool
MetaLookup::dispatch(ClientSM& sm)
{
    return sm.Handle(*this);
}

/* virtual */ void
MetaLookupPath::handle()
{
    SetEUserAndEGroup(*this);
    MetaFattr* fa = 0;
    if ((status = metatree.lookupPath(
            root, path, euser, egroup, fa)) == 0) {
        FattrReply(fa, fattr);
    }
}

template<typename T> inline static bool
CheckUserAndGroup(T& req)
{
    if (! IsValidUser(req.user)) {
        req.status    = -EINVAL;
        req.statusMsg = "invalid user";
        return false;
    }
    if (! IsValidGroup(req.group)) {
        req.status    = -EINVAL;
        req.statusMsg = "invalid group";
        return false;
    }
    if (req.user != req.euser && req.euser != kKfsUserRoot) {
        req.status    = -EPERM;
        req.statusMsg = "user different from effective user";
        return false;
    }
    if (req.euser != kKfsUserRoot &&
            ! (req.authUid != kKfsUserNone ?
            gLayoutManager.GetUserAndGroup().IsGroupMember(req.user, req.group) :
            IsGroupMember(req.user, req.group))) {
        req.status    = -EPERM;
        req.statusMsg = "user is not a member of the group";
        return false;
    }
    return true;
}

template<typename T> inline static bool
CheckCreatePerms(T& req, bool dirFlag)
{
    if (req.authUid == kKfsUserNone) {
        if (req.euser == kKfsUserNone) {
            req.euser = gLayoutManager.GetDefaultUser();
        }
        if (req.egroup == kKfsGroupNone) {
            req.egroup = gLayoutManager.GetDefaultGroup();
        }
        if (req.user == kKfsUserNone) {
            req.user = req.euser;
        }
        if (req.group == kKfsGroupNone) {
            req.group = req.egroup;
        }
    } else {
        if (req.user == kKfsUserNone) {
            req.user = req.authUid;
        }
        if (req.group == kKfsGroupNone) {
            req.group = req.authGid;
        }
    }
    if (req.mode == kKfsModeUndef) {
        req.mode = req.op == META_MKDIR ?
            gLayoutManager.GetDefaultDirMode() :
            gLayoutManager.GetDefaultFileMode();
    }
    if (! IsValidMode(req.mode, dirFlag)) {
        req.status    = -EINVAL;
        req.statusMsg = "invalid mode";
        return false;
    }
    return CheckUserAndGroup(req);
}

const string kInvalidChunksPath("/proc/invalid_chunks");
const string kInvalidChunksPrefix(kInvalidChunksPath + "/");

class MetaRequestNull : public MetaRequest
{
protected:
    MetaRequestNull()
        : MetaRequest(META_NUM_OPS_COUNT, false)
        {}
    virtual ostream& ShowSelf(ostream& os) const
        { return os << "null"; }
    virtual int log(ostream& /* os */) const
        { return 0; }
    static const MetaRequest& Instance()
    {
        static const MetaRequestNull sInstance;
        return sInstance;
    }
    static const MetaRequest& sNullReq;
    friend struct MetaRequest;
};
// Force construction;
const MetaRequest& MetaRequestNull::sNullReq = MetaRequestNull::Instance();

/* static */ const MetaRequest&
MetaRequest::GetNullReq()
{
    return MetaRequestNull::Instance();
}

/* virtual */ void
MetaCreate::handle()
{
    if (! SetUserAndGroup(*this)) {
        return;
    }
    const bool invalChunkFlag = dir == ROOTFID &&
        startsWith(name, kInvalidChunksPrefix);
    bool rootUserFlag = false;
    if (invalChunkFlag) {
        name = name.substr(kInvalidChunksPrefix.length());
        const char* chunk = name.c_str();
        for (const char* p = chunk; ; p++) {
            const int sym = *p & 0xff;
            if ((sym < '0' || sym > '9') && sym != '.') {
                if (sym == 0 && p > chunk) {
                    break;
                }
                statusMsg = "invalid chunk id: " + name;
                status    = -EINVAL;
                return;
            }
        }
        const char* ptr     = name.data();
        const char* end     = ptr + name.size();
        chunkId_t   chunkId = -1;
        if (! DecIntParser::Parse(ptr, end - ptr, chunkId) || ptr != end) {
            statusMsg = "invalid chunk id: " + name;
            status    = -EINVAL;
            return;
        }
        fid_t            fid = -1;
        const MetaFattr* cfa = 0;
        if (! gLayoutManager.GetChunkFileId(chunkId, fid, 0, &cfa, 0) ||
                ! cfa) {
            status    = -ENOENT;
            statusMsg = "no such chunk";
            return;
        }
        string msg("detected invalid chunk: " + name);
        msg += " file ";
        AppendDecIntToString(msg, fid);
        msg += " eof: ";
        AppendDecIntToString(msg, cfa->filesize);
        KFS_LOG_STREAM_ERROR << msg << KFS_LOG_EOM;
        if (cfa->filesize <= 0) {
            // Ignore null size files for now, most likely it is abandoned file.
            return;
        }
        if (gLayoutManager.GetPanicOnInvalidChunkFlag()) {
            panic(msg);
        }
        MetaFattr* fa = 0;
        if ((status = metatree.lookupPath(ROOTFID, kInvalidChunksPath,
                    kKfsUserRoot, kKfsGroupRoot, fa)) != 0
                || ! fa || fa->type != KFS_DIR) {
            if (status == 0) {
                status    = -ENOENT;
                statusMsg = kInvalidChunksPath +
                    ": no such directory";
            }
            return;
        }
        dir = fa->id();
        if (user == kKfsUserNone && authUid == kKfsUserNone) {
            user  = euser  != kKfsUserNone  ? euser  : kKfsUserRoot;
            group = egroup != kKfsGroupNone ? egroup : kKfsGroupRoot;
        }
        mode         = 0;
        rootUserFlag = authUid == kKfsUserNone;
        minSTier     = kKfsSTierMax;
        maxSTier     = kKfsSTierMax;
    } else {
        const bool kDirFlag = false;
        if (! CheckCreatePerms(*this, kDirFlag)) {
            return;
        }
    }

    if (! invalChunkFlag && gWormMode && ! IsWormMutationAllowed(name)) {
        // Do not create a file that we can not write into.
        statusMsg = "worm mode";
        status    = -EPERM;
        return;
    }
    fid        = 0;
    todumpster = -1;
    if (striperType != KFS_STRIPED_FILE_TYPE_NONE && numRecoveryStripes > 0) {
        numReplicas = min(numReplicas,
            gLayoutManager.GetMaxReplicasPerRSFile());
    } else {
        numReplicas = min(numReplicas,
            gLayoutManager.GetMaxReplicasPerFile());
    }
    if (maxSTier < minSTier ||
            minSTier < kKfsSTierMin || minSTier > kKfsSTierMax ||
            maxSTier < kKfsSTierMin || maxSTier > kKfsSTierMax) {
        status    = -EINVAL;
        statusMsg = "invalid storage tier range";
        return;
    }
    if (! gLayoutManager.Validate(*this)) {
        if (0 <= status) {
            status = -EINVAL;
        }
        return;
    }
    MetaFattr* fa = 0;
    status = metatree.create(
        dir,
        name,
        &fid,
        numReplicas,
        exclusive,
        striperType,
        numStripes,
        numRecoveryStripes,
        stripeSize,
        todumpster,
        user,
        group,
        mode,
        rootUserFlag ? kKfsUserRoot : euser,
        egroup,
        &fa
    );
    if (status == 0) {
        if (minSTier < kKfsSTierMax) {
            fa->minSTier = minSTier;
            fa->maxSTier = maxSTier;
        } else {
            minSTier = fa->minSTier;
            maxSTier = fa->maxSTier;
        }
    }
}

/* virtual */ void
MetaMkdir::handle()
{
    if (! SetUserAndGroup(*this)) {
        return;
    }
    const bool kDirFlag = true;
    if (! CheckCreatePerms(*this, kDirFlag)) {
        return;
    }
    fid = 0;
    MetaFattr* fa = 0;
    status = metatree.mkdir(
        dir, name, user, group, mode, euser, egroup, &fid, &fa);
    if (status == 0 && fa) {
        minSTier = fa->minSTier;
        maxSTier = fa->maxSTier;
    }
}

static int
LookupAbsPath(fid_t& dir, string& name, kfsUid_t euser, kfsGid_t egroup)
{
    if (dir != ROOTFID || name.empty() ||
            *name.begin() != '/' || *name.rbegin() == '/') {
        return 0;
    }
    const size_t nameStart = name.rfind('/');
    size_t pos = nameStart;
    while (pos > 0 && name[pos-1] == '/') {
        pos--;
    }
    if (pos == 0) {
        name = name.substr(nameStart + 1);
    } else {
        const string parentDir = name.substr(0, pos);
        MetaFattr*   fa        = 0;
        const int status = metatree.lookupPath(ROOTFID, parentDir,
            euser, egroup, fa);
        if (status != 0) {
            return status;
        }
        if (fa && fa->type == KFS_DIR) {
            dir  = fa->id();
            name = name.substr(nameStart + 1);
        }
    }
    return 0;
}

/*!
 * \brief Remove a file in a directory.  Also, remove the chunks
 * associated with the file.  For removing chunks, we send off
 * RPCs to the appropriate chunkservers.
 */

/* virtual */ void
MetaRemove::handle()
{
    if (gWormMode && ! IsWormMutationAllowed(name)) {
        // deletes are disabled in WORM mode except for specially named
        // files
        statusMsg = "worm mode";
        status    = -EPERM;
        return;
    }
    todumpster = -1;
    SetEUserAndEGroup(*this);
    if ((status = LookupAbsPath(dir, name, euser, egroup)) != 0) {
        return;
    }
    status = metatree.remove(dir, name, pathname, todumpster,
        euser, egroup);
}

/* virtual */ void
MetaRmdir::handle()
{
    if (gWormMode && ! IsWormMutationAllowed(name)) {
        // deletes are disabled in WORM mode
        statusMsg = "worm mode";
        status    = -EPERM;
        return;
    }
    SetEUserAndEGroup(*this);
    if ((status = LookupAbsPath(dir, name, euser, egroup)) != 0) {
        return;
    }
    status = metatree.rmdir(dir, name, pathname, euser, egroup);
}

static vector<MetaDentry*>&
GetReadDirTmpVec()
{
    static vector<MetaDentry*> sReaddirRes;
    sReaddirRes.clear();
    sReaddirRes.reserve(1024);
    return sReaddirRes;
}

inline const MetaFattr*
GetDirAttr(fid_t dir, const vector<MetaDentry*>& v)
{
    const MetaFattr* fa = v.empty() ? 0 : (v.front()->getName() == ".." ?
        v.back()->getFattr() : v.front()->getFattr());
    if (fa && fa->id() != dir) {
        fa = fa->parent;
        if (fa && fa->id() != dir) {
            fa = 0;
        }
    }
    return (fa ? fa : metatree.getFattr(dir));
}

/* virtual */ void
MetaReaddir::handle()
{
    if (! HasEnoughIoBuffersForResponse(*this)) {
        return;
    }
    const bool oldFormatFlag = numEntries < 0;
    int maxEntries = gLayoutManager.GetReadDirLimit();
    if (numEntries > 0 &&
            (maxEntries <= 0 || numEntries < maxEntries)) {
        maxEntries = numEntries;
    }
    numEntries = 0;
    resp.Clear();
    vector<MetaDentry*>& v = GetReadDirTmpVec();
    if ((status = fnameStart.empty() ?
            metatree.readdir(dir, v,
                maxEntries, &hasMoreEntriesFlag) :
            metatree.readdir(dir, fnameStart, v,
                maxEntries, hasMoreEntriesFlag)
            ) != 0) {
        if (status == -ENOENT) {
            const MetaFattr* const fa = metatree.getFattr(dir);
            if (fa && fa->type != KFS_DIR) {
                status = -ENOTDIR;
            }
        }
        return;
    }
    SetEUserAndEGroup(*this);
    const MetaFattr* const fa = GetDirAttr(dir, v);
    if (! fa || ! fa->CanRead(euser, egroup)) {
        status = -EACCES;
        return;
    }
    if (oldFormatFlag && hasMoreEntriesFlag) {
        status     = -ENOMEM;
        statusMsg  = "response exceeds max. allowed number of entries"
            " consider updating kfs client lib";
        return;
    }
    const int extSize = IOBufferData::GetDefaultBufferSize() +
        int(MAX_FILE_NAME_LENGTH);
    int       maxSize = gLayoutManager.GetMaxResponseSize();
    if (! oldFormatFlag && extSize * 2 < maxSize) {
        maxSize -= extSize;
    }
    IOBufferWriter writer(resp);
    vector<MetaDentry*>::const_iterator it;
    for (it = v.begin();
            it != v.end() && writer.GetSize() <= maxSize;
            ++it) {
        const string& name = (*it)->getName();
        // Supress "/" dentry for "/".
        if (dir == ROOTFID && name == "/") {
            continue;
        }
        writer.Write(name);
        writer.Write("\n", 1);
        ++numEntries;
    }
    writer.Close();
    if (resp.BytesConsumable() > maxSize) {
        if (oldFormatFlag) {
            resp.Clear();
            numEntries = 0;
            status     = -ENOMEM;
            statusMsg  = "response exceeds max. size";
        } else {
            if (it != v.end()) {
                hasMoreEntriesFlag = true;
            }
        }
    }
}

class EnumerateLocations
{
    ServerLocations& v;
public:
    EnumerateLocations(ServerLocations& result)
        : v(result)
        {}
    void operator()(const ChunkServerPtr& c) const
    {
        v.push_back(c->GetServerLocation());
    }
};

class ListServerLocations
{
    ostream& os;
public:
    ListServerLocations(ostream &out)
        : os(out)
        {}
    void operator()(const ServerLocation& s) const
    {
        os << " " << s;
    }
};

template<bool ShortFormatFlag>
class ReaddirPlusWriter
{
public:
    typedef MetaReaddirPlus::DEntry   DEntry;
    typedef MetaReaddirPlus::DEntries DEntries;
    typedef MetaReaddirPlus::CInfos   CInfos;
private:
    enum { kNumBufSize = 64 };

    typedef LayoutManager::Servers     Servers;
    typedef PropertiesTokenizer::Token Token;

    class PropName : public Token
    {
    public:
        PropName(const char* shortName, const char* longName)
            : Token(ShortFormatFlag ? shortName : longName)
            {}
    };

    typedef LinearHash<
        KeyOnly<uint64_t>,
        KeyCompare<uint64_t>,
        DynamicArray<SingleLinkedList<KeyOnly<uint64_t> >*, 10>,
        StdFastAllocator<KeyOnly<uint64_t> >
    > IdSet;

    IOBufferWriter writer;
    const int      maxSize;
    const bool     getLastChunkInfoOnlyIfSizeUnknown;
    const bool     omitLastChunkInfoFlag;
    const bool     fileIdAndTypeOnlyFlag;
    char* const    nBufEnd;
    kfsUid_t       prevUid;
    kfsGid_t       prevGid;
    bool           firstEntryFlag;
    bool           insertPrevGidFlag;
    bool           insertPrevUidFlag;
    IdSet          ugids;
    char           nBuf[kNumBufSize];

    static const PropName kMtime;
    static const PropName kCtime;
    static const PropName kCrtime;
    static const PropName kBeginEntry;
    static const PropName kName;
    static const PropName kId;
    static const PropName kType;
    static const PropName kNL;
    static const PropName kSType;
    static const PropName kSCount;
    static const PropName kSRecov;
    static const PropName kSSize;
    static const PropName kCCnt;
    static const PropName kFSize;
    static const PropName kRepl;
    static const PropName kUser;
    static const PropName kGroup;
    static const PropName kMode;
    static const PropName kLOff;
    static const PropName kLId;
    static const PropName kLVers;
    static const PropName kLRCnt;
    static const PropName kLRepl;
    static const PropName kFileCount;
    static const PropName kDirCount;
    static const PropName kMinTier;
    static const PropName kMaxTier;
    static const PropName kSpace;
    static const PropName kUserName;
    static const PropName kGroupName;
    static const Token    kFileType[];

    template<typename T> static
    char* ToString(T val, char* bufEnd)
    {
        return (ShortFormatFlag ?
            IntToHexString(val, bufEnd) :
            IntToDecString(val, bufEnd)
        );
    }
    void Write(const Token& name)
    {
        Write(name.mPtr, name.mLen);
    }
    template<typename T>
    void WriteInt(T val)
    {
        const char* const b = ToString(val, nBufEnd);
        Write(b, nBufEnd - b);
    }
    void WriteTime(int64_t t)
    {
        if (ShortFormatFlag) {
            WriteInt(t);
            return;
        }
        const int64_t kMicroseconds = 1000 * 1000;
        char* p = ToString(t % kMicroseconds, nBufEnd);
        *--p = ' ';
        p = ToString(t / kMicroseconds, p);
        Write(p, nBufEnd - p);
    }
    void Write(const char* data, size_t len)
    {
        writer.Write(data, len);
    }
    void Write(const string& str)
    {
        writer.Write(str);
    }
    bool IsNewGroup(kfsGid_t gid)
    {
        if (prevGid == gid) {
            return false;
        }
        const uint64_t kGroupBit = uint64_t(1) << sizeof(kfsGid_t) * 8;
        bool           ret       = false;
        if (insertPrevGidFlag) {
            const uint64_t id = prevGid | kGroupBit;
            ugids.Insert(id, id, ret);
            ret = false;
            insertPrevGidFlag = false;
        }
        prevGid = gid;
        const uint64_t id = gid | kGroupBit;
        ugids.Insert(id, id, ret);
        return ret;
    }
    bool IsNewUser(kfsUid_t uid)
    {
        if (prevUid == uid) {
            return false;
        }
        bool ret = false;
        if (insertPrevUidFlag) {
            ugids.Insert(prevUid, prevUid, ret);
            ret = false;
            insertPrevUidFlag = false;
        }
        prevUid = uid;
        ugids.Insert(uid, uid, ret);
        return ret;
    }
    void Write(const DEntry& entry, CInfos::const_iterator& lci,
            bool noAttrsFlag, const UserAndGroupNames* ugn)
    {
        Write(kBeginEntry);
        Write(kName);
        Write(entry.name);
        if (noAttrsFlag) {
            Write(kNL);
            return;
        }
        Write(kId);
        WriteInt(entry.id());
        Write(kType);
        Write(kFileType[entry.type]);
        if (fileIdAndTypeOnlyFlag) {
            Write(kNL);
            return;
        }
        Write(kMtime);
        WriteTime(entry.mtime);
        if (! ShortFormatFlag || entry.ctime != entry.crtime) {
            Write(kCtime);
            WriteTime(entry.ctime);
        }
        Write(kCrtime);
        WriteTime(entry.crtime);
        Write(kUser);
        WriteInt(entry.user);
        Write(kGroup);
        WriteInt(entry.group);
        Write(kMode);
        WriteInt(entry.mode);
        if (ugn) {
            if (firstEntryFlag || IsNewUser(entry.user)) {
                const string* const name = ugn->GetUserName(entry.user);
                if (name) {
                    Write(kUserName);
                    Write(*name);
                }
            }
            if (firstEntryFlag || IsNewGroup(entry.group)) {
                const string* const name = ugn->GetGroupName(entry.group);
                if (name) {
                    Write(kGroupName);
                    Write(*name);
                }
            }
            if (firstEntryFlag) {
                firstEntryFlag = false;
                prevUid = entry.user;
                prevGid = entry.group;
                insertPrevUidFlag = true;
                insertPrevGidFlag = true;
            }
        }
        if (entry.type == KFS_DIR) {
            if (entry.filesize >= 0) {
                Write(kFSize);
                WriteInt(entry.filesize);
            }
            Write(kFileCount);
            WriteInt(entry.fileCount());
            Write(kDirCount);
            WriteInt(entry.dirCount());
            Write(kNL);
            return;
        }
        if (entry.IsStriped()) {
            Write(kSType);
            WriteInt(int(entry.striperType));
            Write(kSCount);
            WriteInt(entry.numStripes);
            Write(kSRecov);
            WriteInt(entry.numRecoveryStripes);
            Write(kSSize);
            WriteInt(entry.stripeSize);
        }
        Write(kCCnt);
        WriteInt(entry.chunkcount());
        Write(kFSize);
        WriteInt(entry.filesize);
        Write(kRepl);
        WriteInt(entry.numReplicas);
        if (entry.minSTier < kKfsSTierMax &&
                (entry.type == KFS_FILE || entry.type == KFS_DIR)) {
            Write(kMinTier);
            WriteInt(entry.minSTier);
            Write(kMaxTier);
            WriteInt(entry.maxSTier);
        }
        if (omitLastChunkInfoFlag ||
                entry.type == KFS_DIR || entry.IsStriped() ||
                (getLastChunkInfoOnlyIfSizeUnknown &&
                entry.filesize >= 0)) {
            Write(kNL);
            return;
        }
        const ChunkLayoutInfo& lc = *lci++;
        if (lc.chunkId <= 0) {
            Write(kNL);
            return;
        }
        // Tell the client the info about the last chunk of the
        // file. If the file size is not known then, client can use this
        // info to calculate the size. The most common case is when the
        // chunk is being written into, and the file is not striped.
        Write(kLOff);
        WriteInt(lc.offset);
        Write(kLId);
        WriteInt(lc.chunkId);
        Write(kLVers);
        WriteInt(lc.chunkVersion);
        Write(kLRCnt);
        WriteInt(lc.locations.size());
        Write(kLRepl);
        for (ServerLocations::const_iterator it = lc.locations.begin();
                it != lc.locations.end();
                ++it) {
            Write(kSpace);
            Write(it->hostname);
            Write(kSpace);
            WriteInt(it->port);
        }
        Write(kNL);
    }
    ReaddirPlusWriter(const ReaddirPlusWriter&);
    ReaddirPlusWriter& operator=(const ReaddirPlusWriter&);
public:
    ReaddirPlusWriter(IOBuffer& b, int ms, bool f, bool olcif, bool fidtof)
        : writer(b),
          maxSize(ms),
          getLastChunkInfoOnlyIfSizeUnknown(f),
          omitLastChunkInfoFlag(olcif),
          fileIdAndTypeOnlyFlag(fidtof),
          nBufEnd(nBuf + kNumBufSize - 1),
          prevUid(kKfsUserNone),
          prevGid(kKfsGroupNone),
          firstEntryFlag(true),
          insertPrevGidFlag(false),
          insertPrevUidFlag(false),
          ugids()
        {}
    size_t Write(const DEntries& entries, const CInfos& cinfos,
            bool noAttrsFlag, const UserAndGroupNames* ugn)
    {
        ugids.Clear();
        firstEntryFlag = true;
        CInfos::const_iterator   cit = cinfos.begin();
        DEntries::const_iterator it;
        for (it = entries.begin();
                it != entries.end() &&
                    writer.GetSize() <= maxSize;
                ++it) {
            Write(*it, cit, noAttrsFlag, ugn);
        }
        if (cinfos.end() < cit) {
            panic("dentry and last chunk info mismatch", false);
        }
        writer.Close();
        return (it - entries.begin());
    }
};

template<bool F> const typename ReaddirPlusWriter<F>::PropName
    ReaddirPlusWriter<F>::kMtime(
    "\nM:"  , "\r\nM-Time: ");
template<bool F> const typename ReaddirPlusWriter<F>::PropName
    ReaddirPlusWriter<F>::kCtime(
    "\nC:"  , "\r\nC-Time: ");
template<bool F> const typename ReaddirPlusWriter<F>::PropName
    ReaddirPlusWriter<F>::kCrtime(
    "\nCR:" , "\r\nCR-Time: ");
template<bool F> const typename ReaddirPlusWriter<F>::PropName
    ReaddirPlusWriter<F>::kBeginEntry(
    "B"  , "Begin-entry");
template<bool F> const typename ReaddirPlusWriter<F>::PropName
    ReaddirPlusWriter<F>::kName(
    "\nN:" , "\r\nName: ");
template<bool F> const typename ReaddirPlusWriter<F>::PropName
    ReaddirPlusWriter<F>::kId(
    "\nH:" , "\r\nFile-handle: ");
template<bool F> const typename ReaddirPlusWriter<F>::PropName
    ReaddirPlusWriter<F>::kType(
    "\nT:" , "\r\nType: ");
template<bool F> const typename ReaddirPlusWriter<F>::PropName
    ReaddirPlusWriter<F>::kNL(
    "\n" , "\r\n");
template<bool F> const typename ReaddirPlusWriter<F>::PropName
    ReaddirPlusWriter<F>::kSType(
    "\nST:" , "\r\nStriper-type: ");
template<bool F> const typename ReaddirPlusWriter<F>::PropName
    ReaddirPlusWriter<F>::kSCount(
    "\nSC:" , "\r\nNum-stripes: ");
template<bool F> const typename ReaddirPlusWriter<F>::PropName
    ReaddirPlusWriter<F>::kSRecov(
    "\nSR:" , "\r\nNum-recovery-stripes: ");
template<bool F> const typename ReaddirPlusWriter<F>::PropName
    ReaddirPlusWriter<F>::kSSize(
    "\nSS:" , "\r\nStripe-size: ");
template<bool F> const typename ReaddirPlusWriter<F>::PropName
    ReaddirPlusWriter<F>::kCCnt(
    "\nCC:" , "\r\nChunk-count: ");
template<bool F> const typename ReaddirPlusWriter<F>::PropName
    ReaddirPlusWriter<F>::kFSize(
    "\nS:"  , "\r\nFile-size: ");
template<bool F> const typename ReaddirPlusWriter<F>::PropName
    ReaddirPlusWriter<F>::kRepl(
    "\nR:"  , "\r\nReplication: ");
template<bool F> const typename ReaddirPlusWriter<F>::PropName
    ReaddirPlusWriter<F>::kUser(
    "\nU:"  , "\r\nUser: ");
template<bool F> const typename ReaddirPlusWriter<F>::PropName
    ReaddirPlusWriter<F>::kGroup(
    "\nG:"  , "\r\nGroup: ");
template<bool F> const typename ReaddirPlusWriter<F>::PropName
    ReaddirPlusWriter<F>::kMode(
    "\nA:"  , "\r\nMode: ");
template<bool F> const typename ReaddirPlusWriter<F>::PropName
    ReaddirPlusWriter<F>::kLOff(
    "\nLO:" , "\r\nChunk-offset: ");
template<bool F> const typename ReaddirPlusWriter<F>::PropName
    ReaddirPlusWriter<F>::kLId(
    "\nLH:" , "\r\nChunk-handle: ");
template<bool F> const typename ReaddirPlusWriter<F>::PropName
    ReaddirPlusWriter<F>::kLVers(
    "\nLV:" , "\r\nChunk-version: ");
template<bool F> const typename ReaddirPlusWriter<F>::PropName
    ReaddirPlusWriter<F>::kLRCnt(
    "\nLN:" , "\r\nNum-replicas: ");
template<bool F> const typename ReaddirPlusWriter<F>::PropName
    ReaddirPlusWriter<F>::kLRepl(
    "\nLR:" , "\r\nReplicas:");
template<bool F> const typename ReaddirPlusWriter<F>::PropName
    ReaddirPlusWriter<F>::kFileCount(
    "\nFC:" , "\r\nFile-count: ");
template<bool F> const typename ReaddirPlusWriter<F>::PropName
    ReaddirPlusWriter<F>::kDirCount(
    "\nDC:" , "\r\nDir-count: ");
template<bool F> const typename ReaddirPlusWriter<F>::PropName
    ReaddirPlusWriter<F>::kMinTier(
    "\nFT:" , "\r\nMin-tier: ");
template<bool F> const typename ReaddirPlusWriter<F>::PropName
    ReaddirPlusWriter<F>::kMaxTier(
    "\nLT:" , "\r\nMax-tier: ");
template<bool F> const typename ReaddirPlusWriter<F>::PropName
    ReaddirPlusWriter<F>::kSpace(
    " " , " ");
template<bool F> const typename ReaddirPlusWriter<F>::PropName
    ReaddirPlusWriter<F>::kUserName(
    "\nUN:" , "\r\nUser-name: ");
template<bool F> const typename ReaddirPlusWriter<F>::PropName
    ReaddirPlusWriter<F>::kGroupName(
    "\nGN:" , "\r\nGroup-name: ");
template<bool F> const typename ReaddirPlusWriter<F>::Token
    ReaddirPlusWriter<F>::kFileType[] = { "empty", "file", "dir" };

MetaReaddirPlus::~MetaReaddirPlus()
{
    if (ioBufPending > 0) {
        gLayoutManager.ChangeIoBufPending(-ioBufPending);
    }
}

/* virtual */ void
MetaReaddirPlus::handle()
{
    if (! HasEnoughIoBuffersForResponse(*this)) {
        return;
    }
    dentries.clear();
    lastChunkInfos.clear();
    if (ioBufPending > 0) {
        gLayoutManager.ChangeIoBufPending(-ioBufPending);
    }
    ioBufPending = 0;
    int maxEntries = gLayoutManager.GetReadDirLimit();
    if (numEntries > 0 &&
            (maxEntries <= 0 || numEntries < maxEntries)) {
        maxEntries = numEntries;
    }
    vector<MetaDentry*>& res = GetReadDirTmpVec();
    if ((status = fnameStart.empty() ?
            metatree.readdir(dir, res,
                maxEntries, &hasMoreEntriesFlag) :
            metatree.readdir(dir, fnameStart, res,
                maxEntries, hasMoreEntriesFlag)
            ) != 0) {
        if (status == -ENOENT) {
            MetaFattr * const fa = metatree.getFattr(dir);
            if (fa && fa->type != KFS_DIR) {
                status = -ENOTDIR;
            }
        }
        return;
    }
    const MetaFattr* const fa = GetDirAttr(dir, res);
    SetEUserAndEGroup(*this);
    if (! fa || ! fa->CanRead(euser, egroup)) {
        status = -EACCES;
        return;
    }
    noAttrsFlag      = ! fa->CanSearch(euser, egroup);
    if (numEntries < 0 && hasMoreEntriesFlag) {
        status     = -ENOMEM;
        statusMsg  = "response exceeds max. allowed number of entries"
            " consider updating kfs client lib";
        return;
    }
    maxRespSize = max(0, gLayoutManager.GetMaxResponseSize());
    const int    extSize = IOBufferData::GetDefaultBufferSize() +
        int(MAX_FILE_NAME_LENGTH);
    const size_t maxSize =
        (size_t)((numEntries >= 0 && extSize * 2 < maxRespSize) ?
        maxRespSize - extSize : maxRespSize);
    dentries.reserve(res.size());
    const size_t avgEntrySz[] = {
        148, 272, 64,
         64, 128, 24,
         36, 36,  64,
         28, 28,  24,
    };
    int idx = (fileIdAndTypeOnlyFlag ? 6 : 0) + (numEntries < 0 ? 0 : 3);
    const size_t avgDirExtraSize  = avgEntrySz[idx++];
    const size_t avgFileExtraSize = avgEntrySz[idx++];
    const size_t avgChunkInfoSize = avgEntrySz[idx++];
    const size_t avgLocationSize  = 22;
    size_t responseSize = 0;
    vector<MetaDentry*>::const_iterator it;
    for (it = res.begin();
            it != res.end() && responseSize <= maxSize;
            ++it) {
        const MetaDentry* const entry = *it;
        const MetaFattr*  const fa    = metatree.getFattr(entry);
        if (! fa) {
            continue;
        }
        // Supress "/" dentry for "/".
        const string& name = entry->getName();
        if (fa->id() == ROOTFID && name == "/") {
            continue;
        }
        responseSize += name.length() + (fa->type == KFS_DIR ?
            avgDirExtraSize : avgFileExtraSize);
        dentries.push_back(DEntry(*fa, name));
        if (omitLastChunkInfoFlag || fileIdAndTypeOnlyFlag ||
                noAttrsFlag || fa->type == KFS_DIR || fa->IsStriped() ||
                (getLastChunkInfoOnlyIfSizeUnknown &&
                fa->filesize >= 0)) {
            continue;
        }
        responseSize += avgChunkInfoSize;
        lastChunkInfos.push_back(ChunkLayoutInfo());
        ChunkLayoutInfo& lc = lastChunkInfos.back();
        // for a file, get the layout and provide location of last chunk
        // so that the client can compute filesize
        MetaChunkInfo* lastChunk = 0;
        MetaFattr*     cfa       = 0;
        if (metatree.getLastChunkInfo(
                fa->id(), false, cfa, lastChunk) != 0 ||
                ! lastChunk) {
            lc.offset       = -1;
            lc.chunkId      = -1;
            lc.chunkVersion = -1;
            continue;
        }
        if (fa != cfa) {
            panic("readdirplus: file attribute mismatch", false);
        }
        lc.offset       = lastChunk->offset;
        lc.chunkId      = lastChunk->chunkId;
        lc.chunkVersion = lastChunk->chunkVersion;
        Servers    c;
        MetaFattr* lfa = 0;
        if (gLayoutManager.GetChunkToServerMapping(
                *lastChunk, c, lfa) != 0) {
            // All the servers hosting the chunk are down.
            continue;
        }
        lc.locations.reserve(c.size());
        for_each(c.begin(), c.end(), EnumerateLocations(lc.locations));
        responseSize += lc.locations.size() * avgLocationSize;
    }
    if (maxSize < responseSize) {
        if (numEntries < 0) {
            status    = -ENOMEM;
            statusMsg = "response exceeds max. size";
            lastChunkInfos.clear();
            dentries.clear();
            responseSize = 0;
        } else if (it != res.end()) {
            hasMoreEntriesFlag = true;
        }
    }
    ioBufPending = (int64_t)responseSize;
    if (ioBufPending > 0) {
        gLayoutManager.ChangeIoBufPending(ioBufPending);
        maxRespSize = max(maxRespSize, (int)ioBufPending +
            IOBufferData::GetDefaultBufferSize());
    }
}

/*!
 * \brief Get the allocation information for a specific chunk in a file.
 */
/* virtual */ void
MetaGetalloc::handle()
{
    if (offset < 0) {
        status    = -EINVAL;
        statusMsg = "negative offset";
        return;
    }
    MetaChunkInfo* chunkInfo = 0;
    status = metatree.getalloc(fid, offset, &chunkInfo);
    if (status != 0) {
        KFS_LOG_STREAM_DEBUG <<
            "handle_getalloc(" << fid << "," << offset <<
            ") = " << status << ": kfsop failed" <<
        KFS_LOG_EOM;
        return;
    }

    chunkId      = chunkInfo->chunkId;
    chunkVersion = chunkInfo->chunkVersion;
    Servers    c;
    MetaFattr* fa = 0;
    replicasOrderedFlag = false;
    const int err = gLayoutManager.GetChunkToServerMapping(
        *chunkInfo, c, fa, &replicasOrderedFlag);
    if (! fa) {
        panic("invalid chunk to server map", false);
    }
    if (! CanAccessFile(fa, *this)) {
        return;
    }
    if (! fromChunkServerFlag && gLayoutManager.VerifyAllOpsPermissions()) {
        SetEUserAndEGroup(*this);
        if (! fa->CanRead(euser, egroup)) {
            status = -EACCES;
            return;
        }
    }
    if (err) {
        status    = -EAGAIN;
        statusMsg = "no replicas available chunk: ";
        AppendDecIntToString(statusMsg, chunkId);
        KFS_LOG_STREAM_ERROR <<
            "getalloc "
            "<" << fid << "," << chunkId << "," << offset << ">"
            " " << statusMsg <<
        KFS_LOG_EOM;
        return;
    }
    locations.reserve(c.size());
    for_each(c.begin(), c.end(), EnumerateLocations(locations));
    status = 0;
}

/*!
 * \brief Get the allocation information for a file.  Determine
 * how many chunks there and where they are located.
 */
/* virtual */ void
MetaGetlayout::handle()
{
    if (! HasEnoughIoBuffersForResponse(*this)) {
        return;
    }
    vector<MetaChunkInfo*> chunkInfo;
    MetaFattr*             fa = 0;
    if (lastChunkInfoOnlyFlag) {
        bool           kOnlyForNonStripedFileFlag = false;
        MetaChunkInfo* ci                         = 0;
        status = metatree.getLastChunkInfo(
            fid, kOnlyForNonStripedFileFlag, fa, ci);
        if (status == 0 && ci) {
            chunkInfo.push_back(ci);
        }
    } else if (startOffset <= 0) {
        status = metatree.getalloc(fid, fa, chunkInfo,
            maxResCnt > 0 ? maxResCnt + 1 : maxResCnt);
    } else {
        status = metatree.getalloc(fid, startOffset, chunkInfo,
            maxResCnt > 0 ? maxResCnt + 1 : maxResCnt);
        if (status == -ENOENT && (fa = metatree.getFattr(fid))) {
            status = 0;
        }
    }
    if (status != 0) {
        return;
    }
    if (! fa) {
        if (chunkInfo.empty()) {
            panic("MetaGetlayout::handle -- getalloc no chunks");
            status = -EFAULT;
            return;
        }
        fa = chunkInfo.front()->getFattr();
        if (! fa) {
            panic("MetaGetlayout::handle -- invalid chunk entry");
            status = -EFAULT;
            return;
        }
    }
    if (! CanAccessFile(fa, *this)) {
        return;
    }
    if (gLayoutManager.VerifyAllOpsPermissions()) {
        SetEUserAndEGroup(*this);
        if (! fa->CanRead(euser, egroup)) {
            status = -EACCES;
            return;
        }
    }
    fileSize  = fa->filesize;
    numChunks = (int)chunkInfo.size();
    if ((hasMoreChunksFlag = maxResCnt > 0 && maxResCnt < numChunks)) {
        numChunks = maxResCnt;
    }
    ostream&        os     = sWOStream.Set(resp);
    const char*     prefix = "";
    Servers         c;
    ChunkLayoutInfo l;
    for (int i = 0; i < numChunks; i++) {
        l.locations.clear();
        l.offset       = chunkInfo[i]->offset;
        l.chunkId      = chunkInfo[i]->chunkId;
        l.chunkVersion = chunkInfo[i]->chunkVersion;
        if (! omitLocationsFlag) {
            MetaFattr* cfa = 0;
            const int  err = gLayoutManager.GetChunkToServerMapping(
                *(chunkInfo[i]), c, cfa);
            assert(! fa || cfa == fa);
            if (err && ! continueIfNoReplicasFlag) {
                resp.Clear();
                status    = -EHOSTUNREACH;
                statusMsg = "no replicas available chunk: ";
                AppendDecIntToString(statusMsg, l.chunkId);
                break;
            }
            for_each(c.begin(), c.end(),
                EnumerateLocations(l.locations));
        }
        if (! (os << prefix << l)) {
            break;
        }
        prefix = " ";
    }
    os.flush();
    if (status == 0 && ! os) {
        resp.Clear();
        status    = -ENOMEM;
        statusMsg = "response exceeds max. size";
    }
    sWOStream.Reset();
}

/* virtual */ bool
MetaAllocate::dispatch(ClientSM& sm)
{
    return sm.Handle(*this);
}

/*!
 * \brief handle an allocation request for a chunk in a file.
 * \param[in] r write allocation request
 *
 * Write allocation proceeds as follows:
 *  1. The client has sent a write allocation request which has been
 * parsed and turned into an RPC request (which is handled here).
 *  2. We first get a unique chunk identifier (after validating the
 * fileid).
 *  3. We send the request to the layout manager to pick a location
 * for the chunk.
 *  4. The layout manager picks a location and sends an RPC to the
 * corresponding chunk server to create the chunk.
 *  5. When the RPC is going on, processing for this request is
 * suspended.
 *  6. When the RPC reply is received, this request gets re-activated
 * and we come back to this function.
 *  7. Assuming that the chunk server returned a success,  we update
 * the metatree to link the chunkId with the fileid (from this
 * request).
 *  8. Processing for this request is now complete; it is logged and
 * a reply is sent back to the client.
 *
 * Versioning/Leases introduces a few wrinkles to the above steps:
 * In step #2, the metatree could return -EEXIST if an allocation
 * has been done for the <fid, offset>.  In such a case, we need to
 * check with the layout manager to see if a new lease is required.
 * If a new lease is required, the layout manager bumps up the version
 * # for the chunk and notifies the chunkservers.  The message has to
 * be suspended until the chunkservers ack.  After the message is
 * restarted, we need to update the metatree to reflect the newer
 * version # before notifying the client.
 *
 * On the other hand, if a new lease isn't required, then the layout
 * manager tells us where the data has been placed; the process for
 * the request is therefore complete.
 */
/* virtual */ void
MetaAllocate::handle()
{
    suspended = false;
    if (layoutDone) {
        return;
    }
    KFS_LOG_STREAM_DEBUG << "Starting layout for req: " << opSeqno <<
    KFS_LOG_EOM;
    if (gWormMode && ! IsWormMutationAllowed(pathname.GetStr())) {
        statusMsg = "worm mode";
        status    = -EPERM;
        return;
    }
    if (! gLayoutManager.IsAllocationAllowed(this)) {
        if (status >= 0) {
            statusMsg = "allocation not allowed";
            status    = -EPERM;
        }
        return;
    }
    // Do not check permissions for chunk invalidation internally generated by
    // the chunk recovery.
    const bool checkPermissionsFlag =
        gLayoutManager.VerifyAllOpsPermissions() &&
        (clnt || ! invalidateAllFlag || fromChunkServerFlag ||
            fromClientSMFlag || authUid != kKfsUserNone);
    if (checkPermissionsFlag) {
        SetEUserAndEGroup(*this);
    }
    if (appendChunk) {
        if (invalidateAllFlag) {
            statusMsg = "chunk invalidation"
                " is not supported with append";
            status    = -EINVAL;
            return;
        }
        // pick a chunk for which a write lease exists
        status = 0;
        if (gLayoutManager.AllocateChunkForAppend(this) == 0) {
            // all good
            KFS_LOG_STREAM_DEBUG <<
                "For append re-using chunk " << chunkId <<
                (suspended ? "; allocation in progress" : "") <<
            KFS_LOG_EOM;
            logFlag = false; // Do not emit redundant log record.
            return;
        }
        if (status != 0) {
            return;
        }
        offset = -1; // Allocate a new chunk past eof.
    }
    // force an allocation
    chunkId = 0;
    initialChunkVersion = -1;
    vector<MetaChunkInfo*> chunkBlock;
    MetaFattr*             fa = 0;
    // start at step #2 above.
    status = metatree.allocateChunkId(
        fid, offset,
        &chunkId,
        &chunkVersion,
        &numReplicas,
        &stripedFileFlag,
        &chunkBlock,
        &chunkBlockStart,
        checkPermissionsFlag ?
            euser : kKfsUserRoot,
        egroup,
        &fa
    );
    if (status != 0 && (status != -EEXIST || appendChunk)) {
        // we have a problem
        return;
    }
    if (stripedFileFlag && appendChunk) {
        status    = -EINVAL;
        statusMsg = "append is not supported with striped files";
        return;
    }
    if (invalidateAllFlag) {
        if (! stripedFileFlag) {
            status    = -EINVAL;
            statusMsg = "chunk invalidation"
                " is not supported for non striped files";
            return;
        }
        if (status != -EEXIST) {
            // Allocate the chunk if doesn't exist to trigger the
            // recovery later.
            status = metatree.assignChunkId(
                fid, offset, chunkId, chunkVersion, 0);
            if (status == 0) {
                // Add the chunk to the recovery queue.
                gLayoutManager.ChangeChunkReplication(chunkId);
            }
            if (status != -EEXIST) {
                // Presently chunk can not possibly exist, as
                // metatree.allocateChunkId() the above
                // returned success.
                return;
            }
        }
        initialChunkVersion = chunkVersion;
        if (gLayoutManager.InvalidateAllChunkReplicas(
                fid, offset, chunkId, chunkVersion)) {
            // Add the chunk to the recovery queue.
            gLayoutManager.ChangeChunkReplication(chunkId);
            status = 0;
            return;
        }
        panic("failed to invalidate existing chunk", false);
        status = -ENOENT;
        return;
    }
    permissions = fa;
    minSTier    = fa->minSTier;
    maxSTier    = fa->maxSTier;
    int ret;
    if (status == -EEXIST) {
        initialChunkVersion = chunkVersion;
        bool isNewLease = false;
        // Get a (new) lease if possible
        status = gLayoutManager.GetChunkWriteLease(this, isNewLease);
        if (status != 0) {
            // couln't get the lease...bail
            return;
        }
        if (!isNewLease) {
            KFS_LOG_STREAM_DEBUG << "Got valid lease for req:" << opSeqno <<
            KFS_LOG_EOM;
            // we got a valid lease.  so, return
            return;
        }
        // new lease and chunkservers have been notified
        // so, wait for them to ack
    } else if ((ret = gLayoutManager.AllocateChunk(this, chunkBlock)) < 0) {
        // we have a problem
        status = ret;
        return;
    }
    // we have queued an RPC to the chunkserver.  so, hold
    // off processing (step #5)
    // If all allocate ops fail synchronously (all servers are down), then
    // the op is not suspended, and can proceed immediately.
    suspended =! layoutDone;
}

void
MetaAllocate::LayoutDone(int64_t chunkAllocProcessTime)
{
    const bool wasSuspended = suspended;
    suspended  = false;
    layoutDone = true;
    KFS_LOG_STREAM_DEBUG <<
        "layout is done for"
        " req: "    << opSeqno   <<
        " status: " << status    <<
        " "         << statusMsg <<
    KFS_LOG_EOM;
    if (status == 0) {
        // Check if all servers are still up, and didn't go down
        // and reconnected back.
        // In the case of reconnect smart pointers should be different:
        // the the previous server incarnation always taken down on
        // reconnect.
        // Since the chunk is "dangling" up until this point, then in
        // the case of reconnect the chunk becomes "stale", and chunk
        // server is instructed to delete its replica of this new chunk.
        for (Servers::const_iterator i = servers.begin();
                i != servers.end(); ++i) {
            if ((*i)->IsDown()) {
                KFS_LOG_STREAM_DEBUG << (*i)->GetServerLocation() <<
                    " went down during allocation, alloc failed" <<
                KFS_LOG_EOM;
                status = -EIO;
                break;
            }
        }
    }
    // Ensure that the op isn't stale.
    // Invalidate all replicas might make it stale if it comes while this op
    // is in flight. Do not do any cleanup if the op is invalid: all required
    // cleanup has already been done.
    if (gLayoutManager.Validate(this) && status != 0) {
        // we have a problem: it is possible that the server
        // went down.  ask the client to retry....
        if (status >= 0) {
            status = -EALLOCFAILED;
        }
        if (initialChunkVersion >= 0) {
            gLayoutManager.CommitOrRollBackChunkVersion(this);
        } else {
            // this is the first time the chunk was allocated.
            // since the allocation failed, remove existence of this chunk
            // on the metaserver.
            gLayoutManager.DeleteChunk(this);
        }
        // processing for this message is all done
    } else if (status < 0 && initialChunkVersion < 0) {
        // Cleanup stale chunk in the case when client has already invalidated
        // the chunk, and the chunk didn't exist. In this case a new chunk id
        // was created.
        gLayoutManager.DeleteChunk(this);
    }
    if (status == 0) {
        // layout is complete (step #6)

        // update the tree (step #7) and since we turned off the
        // suspend flag, the request will be logged and go on its
        // merry way.
        //
        // There could be more than one append allocation request set
        // (each set one or more request with the same chunk) in flight.
        // The append request sets can finish in any order.
        // The append request sets can potentially all have the same
        // offset: the eof at the time the first request in each set
        // started.
        // For append requests assignChunkId assigns past eof offset,
        // if it succeeds, and returns the value in appendOffset.
        chunkOff_t appendOffset = offset;
        chunkId_t  curChunkId   = chunkId;
        status = metatree.assignChunkId(fid, offset, chunkId, chunkVersion,
            appendChunk ? &appendOffset : 0, &curChunkId);

        //subrata add

        //We are keeping a new table with stripe_identifier(stripeID) to vector<chunkId_t> mapping. Our new recovery will be interms of this stripe_identifier and we will use it to communicate with chunk-servers as well. Specially for recovery.
         
        std::map<long, std::map<int,chunkId_t> > :: iterator stripePos = gLayoutManager.stripeIdentifierToChunkIDMap.find(this->stripe_identifier);
        if(stripePos == gLayoutManager.stripeIdentifierToChunkIDMap.end())
        {
           std::map<int, chunkId_t> chunkMap;
           chunkMap[this->rs_chunk_index] = curChunkId;
           gLayoutManager.stripeIdentifierToChunkIDMap[this->stripe_identifier] = chunkMap;
        }
        else
        {
           gLayoutManager.stripeIdentifierToChunkIDMap[this->stripe_identifier][this->rs_chunk_index] = curChunkId;
        }
        //subrata: find the chunk related entry that the LayoutManager keeps and add the stripe_identifier along with other information for the chunk
        MetaChunkInfo* chunkInfo = gLayoutManager.GetChunkInfoForChunkId(chunkId);
        chunkInfo->stripe_identifier = this->stripe_identifier;
  
        //subrata end

        if (status == 0) {
            // Offset can change in the case of append.
            offset = appendOffset;
            gLayoutManager.CancelPendingMakeStable(fid, chunkId);
            // assignChunkId() forces a recompute of the file's size.
        } else {
            KFS_LOG_STREAM((appendChunk && status == -EEXIST) ?
                    MsgLogger::kLogLevelERROR :
                    MsgLogger::kLogLevelDEBUG) <<
                "assign chunk id: " << chunkId << " failed for"
                " <" << fid << "," << offset << ">"
                " status: " << status <<
                " curChunkId: " << curChunkId <<
            KFS_LOG_EOM;
            if (appendChunk && status == -EEXIST) {
                panic("append chunk allocation internal error", false);
            } else if (status == -ENOENT ||
                    (status == -EEXIST && curChunkId != chunkId)) {
                gLayoutManager.DeleteChunk(this);
                servers.clear(); // Chunk delete is already issued.
            }
        }
        gLayoutManager.CommitOrRollBackChunkVersion(this);
    }
    if (appendChunk) {
        if (0 <= status) {
            if (responseStr.empty()) {
                ostringstream& os = GetTmpOStringStream();
                responseSelf(os);
                responseStr = os.str();
            }
            if (writeMasterKeyValidFlag && responseAccessStr.empty()) {
                tokenSeq = (TokenSeq)gLayoutManager.GetRandom().Rand();
                ostringstream& os = GetTmpOStringStream();
                writeChunkAccess(os);
                responseAccessStr = os.str();
            }
        }
        gLayoutManager.AllocateChunkForAppendDone(*this);
    }
    if (status >= 0 && pendingLeaseRelinquish) {
        pendingLeaseRelinquish->clnt = clnt;
        clnt = this;
    }
    if (! wasSuspended) {
        // Don't need need to resume, if it wasn't suspended: this
        // method is [indirectly] invoked from handle().
        // Presently the only way to get here is from synchronous chunk
        // server allocation failure. The request cannot possibly have
        // non empty request list in this case, as it wasn't ever
        // suspened.
        if (next) {
            panic(
                "non empty allocation queue,"
                " for request that was not suspended",
                false
            );
        }
        return;
    }
    // Currently the ops queue only used for append allocations.
    assert(appendChunk || ! next);
    // Update the process time, charged from MetaChunkAllocate.
    const int64_t now = microseconds();
    processTime += now - chunkAllocProcessTime;
    if (! next) {
        submit_request(this);
        return;
    }
    // Clone status for all ops in the queue.
    // Submit the replies in the same order as requests.
    // "this" might get deleted after submit_request()
    MetaAllocate*         n     = this;
    const string          ra    = responseAccessStr;
    const kfsUid_t        rauid = authUid;
    const CryptoKeys::Key wmkey = writeMasterKey;
    do {
        MetaAllocate& c = *n;
        n = c.next;
        c.next = 0;
        if (n) {
            MetaAllocate& q = *n;
            assert(q.fid == c.fid);
            q.status           = c.status;
            q.statusMsg        = c.statusMsg;
            q.suspended        = false;
            q.fid              = c.fid;
            q.offset           = c.offset;
            q.chunkId          = c.chunkId;
            q.chunkVersion     = c.chunkVersion;
            q.pathname         = c.pathname;
            q.numReplicas      = c.numReplicas;
            q.layoutDone       = c.layoutDone;
            q.appendChunk      = c.appendChunk;
            q.stripedFileFlag  = c.stripedFileFlag;
            q.numServerReplies = c.numServerReplies;
            q.responseStr      = c.responseStr;
            if (q.responseStr.empty()) {
                q.servers = c.servers;
                q.master  = c.master;
            }
            q.writeMasterKeyValidFlag = c.writeMasterKeyValidFlag;
            if (q.writeMasterKeyValidFlag) {
                q.tokenSeq                   = c.tokenSeq;
                q.clientCSAllowClearTextFlag = c.clientCSAllowClearTextFlag;
                q.issuedTime                 = c.issuedTime;
                q.validForTime               = c.validForTime;
                q.writeMasterKeyId           = c.writeMasterKeyId;
                if (ra.empty() || rauid != c.authUid) {
                    q.writeMasterKey = wmkey;
                } else {
                    q.responseAccessStr = ra;
                }
            }
        }
        submit_request(&c);
    } while (n);
}

int
MetaAllocate::logOrLeaseRelinquishDone(int code, void* data)
{
    if (code != EVENT_CMD_DONE ||
            (data != this && data != pendingLeaseRelinquish)) {
        panic("MetaChunkAllocate::logDone invalid invocation");
        return 1;
    }
    if (data == this) {
        clnt = pendingLeaseRelinquish->clnt;
        if (status >= 0) {
            pendingLeaseRelinquish->clnt = this;
            submit_request(pendingLeaseRelinquish);
            return 0;
        }
    }
    pendingLeaseRelinquish->clnt = 0;
    delete pendingLeaseRelinquish;
    pendingLeaseRelinquish = 0;
    submit_request(this);
    return 0;
}

int
MetaAllocate::CheckStatus(bool forceFlag) const
{
    if (status < 0) {
        return status;
    }
    const size_t sz = servers.size();
    if (numServerReplies == sz && ! forceFlag) {
        return status;
    }
    for (size_t i = 0; i < sz; i++) {
        if (servers[i]->IsDown()) {
            const_cast<MetaAllocate*>(this)->status    = -EALLOCFAILED;
            const_cast<MetaAllocate*>(this)->statusMsg = "server " +
                servers[i]->GetServerLocation().ToString() + " went down";
            break;
        }
    }
    return status;
}

bool
MetaAllocate::ChunkAllocDone(const MetaChunkAllocate& chunkAlloc)
{
    // if there is a non-zero status, don't throw it away
    if (chunkAlloc.status != 0 && status == 0 && firstFailedServerIdx < 0) {
        status    = chunkAlloc.status;
        statusMsg = chunkAlloc.statusMsg;
        // In the case of version change failure take the first failed
        // server out, otherwise allocation might never succeed.
        if (initialChunkVersion >= 0 && servers.size() > 1) {
            const ChunkServer* const cs = chunkAlloc.server.get();
            for (size_t i = 0; i < servers.size(); i++) {
                if (servers[i].get() == cs && ! servers[i]->IsDown()) {
                    firstFailedServerIdx = (int)i;
                    break;
                }
            }
        }
    }
    // Collect and pass back to the client chunk server allocation failure
    // messages.
    if (chunkAlloc.status != 0  && statusMsg.length() < 384 &&
            chunkAlloc.server) {
        if (! statusMsg.empty()) {
            statusMsg += " ";
        }
        statusMsg += chunkAlloc.server->GetServerLocation().ToString();
        if (! chunkAlloc.statusMsg.empty()) {
            statusMsg += " ";
            statusMsg += chunkAlloc.statusMsg;
        }
    }
    numServerReplies++;
    // wait until we get replies from all servers
    if (numServerReplies != servers.size()) {
        return false;
    }
    // The op are no longer suspended.
    const bool kForceFlag = true;
    if (firstFailedServerIdx >= 0 && status != 0) {
        // Check if all servers are up before discarding chunk.
        const int prevStatus = status;
        status = 0;
        if (CheckStatus(kForceFlag) == 0) {
            status = prevStatus;
            gLayoutManager.ChunkCorrupt(
                chunkId, servers[firstFailedServerIdx]);
        }
    } else {
        // Check if any server went down.
        CheckStatus(kForceFlag);
    }
    LayoutDone(chunkAlloc.processTime);
    return true;
}

/* virtual */ void
MetaChunkAllocate::handle()
{
    assert(req && req->op == META_ALLOCATE);
    if (req->ChunkAllocDone(*this)) {
        // The time was charged to alloc.
        processTime = microseconds();
    }
}

ostream&
MetaAllocate::ShowSelf(ostream& os) const
{
    os << "allocate:"
        " seq: "      << opSeqno     <<
        " path: "     << pathname    <<
        " fid: "      << fid         <<
        " chunkId: "  << chunkId     <<
        " offset: "   << offset      <<
        " client: "   << clientHost  <<
        " replicas: " << numReplicas <<
        " append: "   << appendChunk <<
        " log: "      << logFlag
    ;
    for (Servers::const_iterator i = servers.begin();
            i != servers.end();
            ++i) {
        os << " " << (*i)->GetServerLocation();
    }
    return os;
}

/* virtual */ void
MetaChunkVersChange::handle()
{
    gLayoutManager.Done(*this);
}

/* virtual */ void
MetaTruncate::handle()
{
    if (gWormMode && ! IsWormMutationAllowed(pathname.GetStr())) {
        statusMsg = "worm mode";
        status    = -EPERM;
        return;
    }
    mtime = microseconds();
    kfsUid_t eu;
    if (checkPermsFlag || gLayoutManager.VerifyAllOpsPermissions()) {
        SetEUserAndEGroup(*this);
        eu = euser;
    } else {
        eu = kKfsUserRoot;
    }
    if (pruneBlksFromHead) {
        status = metatree.pruneFromHead(fid, offset, &mtime, eu, egroup);
        return;
    }
    if (endOffset >= 0 && endOffset < offset) {
        status    = -EINVAL;
        statusMsg = "end offset less than offset";
        return;
    }
    status = metatree.truncate(fid, offset, &mtime, eu, egroup,
        endOffset, setEofHintFlag);
}

/* virtual */ void
MetaRename::handle()
{
    MetaFattr* fa = 0;
    status = 0;
    SetEUserAndEGroup(*this);
    if (gWormMode && (! IsWormMutationAllowed(oldname) ||
            (status = metatree.lookupPath(
                ROOTFID, newname, euser, egroup, fa)
            ) != -ENOENT)) {
        if (status == 0) {
            // renames are disabled in WORM mode: otherwise, we
            // ocould verwrite an existing file
            statusMsg = "worm mode";
            status    = -EPERM;
        }
        return;
    }
    todumpster = -1;
    status = metatree.rename(dir, oldname, newname,
        oldpath, overwrite, todumpster, euser, egroup);
}

/* virtual */ void
MetaSetMtime::handle()
{
    if (fid > 0 && status == 0) {
        // mtime is already set, only log.
        return;
    }
    SetEUserAndEGroup(*this);
    MetaFattr* fa = 0;
    status = metatree.lookupPath(ROOTFID, pathname,
        euser, egroup, fa);
    if (status != 0) {
        return;
    }
    if (! fa->CanWrite(euser, egroup)) {
        status = -EACCES;
        return;
    }
    fa->mtime = mtime;
    fid       = fa->id();
}

/* virtual */ void
MetaChangeFileReplication::handle()
{
    MetaFattr* const fa = metatree.getFattr(fid);
    if (! fa) {
        statusMsg = "no such file";
        status    = -ENOENT;
    }
    if (! CanAccessFile(fa, *this)) {
        return;
    }
    SetEUserAndEGroup(*this);
    if (! fa->CanWrite(euser, egroup)) {
        status = -EACCES;
        return;
    }
    if (fa->type == KFS_DIR) {
        if (euser != kKfsUserRoot) {
            status    = -EACCES;
            statusMsg = "root privileges are"
                " required to change directory storage tiers";
            return;
        }
    } else {
        numReplicas = min(numReplicas,
            max(int16_t(fa->numReplicas),
                (fa->striperType != KFS_STRIPED_FILE_TYPE_NONE &&
                        fa->numRecoveryStripes > 0) ?
                gLayoutManager.GetMaxReplicasPerRSFile() :
                gLayoutManager.GetMaxReplicasPerFile()
        ));
    }
    kfsSTier_t const prevMinSTier = fa->minSTier;
    kfsSTier_t const prevMaxSTier = fa->maxSTier;
    int16_t    const prevRepl     = (int16_t)fa->numReplicas;
    status = metatree.changeFileReplication(
        fa, numReplicas, minSTier, maxSTier);
    if (status == 0) {
        numReplicas = fa->type == KFS_DIR ?
            (int16_t)0 : (int16_t)fa->numReplicas; // update for log()
        logFlag = fa->type != KFS_DIR && numReplicas != prevRepl;
        if (fa->minSTier != prevMinSTier || fa->maxSTier != prevMaxSTier) {
            logFlag = true;
            minSTier = fa->minSTier;
            maxSTier = fa->maxSTier;
        } else if (logFlag) {
            // No change, do not log tiers.
            minSTier = kKfsSTierUndef;
            maxSTier = kKfsSTierUndef;
        }
    } else {
        logFlag = false;
    }
}

/*
 * Move chunks from src file into the end chunk boundary of the dst file.
 */
/* virtual */ void
MetaCoalesceBlocks::handle()
{
    dstStartOffset = -1;
    mtime = microseconds();
    SetEUserAndEGroup(*this);
    status = metatree.coalesceBlocks(
        srcPath, dstPath, srcFid, dstFid,
        dstStartOffset, &mtime, numChunksMoved,
        euser, egroup);
    KFS_LOG_STREAM(status == 0 ?
            MsgLogger::kLogLevelINFO : MsgLogger::kLogLevelERROR) <<
        "coalesce blocks " << srcPath << "->" << dstPath <<
        " " << srcFid << "->" << dstFid <<
        " status: "       << status <<
        " offset: "       << dstStartOffset <<
        " chunks moved: " << numChunksMoved <<
    KFS_LOG_EOM;
}

/* virtual */ void
MetaRetireChunkserver::handle()
{
    if (! HasMetaServerAdminAccess(*this)) {
        return;
    }
    status = gLayoutManager.RetireServer(location, nSecsDown);
}

/* virtual */ void
MetaToggleWORM::handle()
{
    if (! HasMetaServerAdminAccess(*this)) {
        return;
    }
    KFS_LOG_STREAM_INFO << "Toggle WORM: " << value << KFS_LOG_EOM;
    setWORMMode(value);
    status = 0;
}

/* virtual */ void
MetaHello::handle()
{
    if (! server) {
        // This is likely coming from the ClientSM.
        KFS_LOG_STREAM_DEBUG << "no server invalid cmd: " << Show() <<
        KFS_LOG_EOM;
        status = -EINVAL;
    }
    if (status < 0) {
        // bad hello request...possible cluster key mismatch
        return;
    }
    gLayoutManager.AddNewServer(this);
}

/* virtual */ void
MetaBye::handle()
{
    gLayoutManager.ServerDown(server);
}

/* virtual */ void
MetaLeaseAcquire::handle()
{
    if (status < 0) {
        return;
    }
    if (1 < submitCount) {
        // Minimize spurious read lease acquisitions, when client timed out, and
        // closed connection. The connection check is racy, but should suffice
        // for the purpose at hands.
        // Presently the request can only come from the client. fromClientSMFlag
        // check here isn't strictly required, well unless something pretends to
        // be chunkserver, sends hello, then this request.
        const int64_t kMicroseconds = 1000 * 1000;
        const int64_t now           = globalNetManager().Now() * kMicroseconds;
        if ((submitTime + leaseTimeout * kMicroseconds < now) ||
                (0 < maxWaitMillisec &&
                    submitTime + maxWaitMillisec * 1000 < now)) {
            statusMsg = "lease wait timed out";
            status    = -EAGAIN;
            return;
        }
        if (fromClientSMFlag &&
                ! static_cast<const ClientSM*>(clnt)->GetConnection()) {
            status = -EAGAIN;
            return;
        }
    } else if (gLayoutManager.VerifyAllOpsPermissions()) {
        SetEUserAndEGroup(*this);
    }
    status = gLayoutManager.GetChunkReadLease(this);
}

/* virtual */ void
MetaLeaseRenew::handle()
{
    if (gLayoutManager.VerifyAllOpsPermissions()) {
        SetEUserAndEGroup(*this);
    }
    status = gLayoutManager.LeaseRenew(this);
}

/* virtual */ void
MetaLeaseRelinquish::handle()
{
    status = gLayoutManager.LeaseRelinquish(this);
    KFS_LOG_STREAM(status == 0 ?
            MsgLogger::kLogLevelDEBUG : MsgLogger::kLogLevelERROR) <<
        Show() << " status: " << status <<
    KFS_LOG_EOM;
}

/* virtual */ void
MetaLeaseCleanup::handle()
{
    gLayoutManager.LeaseCleanup();
    // Some leases might be expired or relinquished: try to cleanup the
    // dumpster.
    // FIXME:
    // Dumpster cleanup needs to be logged, otherwise all files that ever
    // got there will accumulate during replay. Log compactor doesn't help
    // as it does not, and can not empty the dumpster.
    // Only meta server empties dumpster.
    // Checkpoints from the forked copy should alleviate the problem.
    // Defer this for now assuming that checkpoints from forked copy is
    // the default operating mode.
    metatree.cleanupDumpster();
    metatree.cleanupPathToFidCache();
    status = 0;
}

class PrintChunkServerLocations {
    ostream &os;
public:
    PrintChunkServerLocations(ostream &out): os(out) { }
    void operator () (const ChunkServerPtr &s)
    {
        os << ' ' <<  s->GetServerLocation();
    }
};

/* virtual */ void
MetaGetPathName::handle()
{
    ostringstream& os = GetTmpOStringStream();
    const MetaFattr* fa = 0;
    if (fid < 0) {
        const MetaChunkInfo*   chunkInfo = 0;
        LayoutManager::Servers srvs;
        if (! gLayoutManager.GetChunkFileId(
                chunkId, fid, &chunkInfo, &fa, &srvs)) {
            status    = -ENOENT;
            statusMsg = "no such chunk";
            return;
        }
        if (chunkInfo) {
            os <<
            "Chunk-offset: "  << chunkInfo->offset       << "\r\n"
            "Chunk-version: " << chunkInfo->chunkVersion << "\r\n"
            ;
        }
        os << "Num-replicas: " << srvs.size() << "\r\n";
        if (! srvs.empty()) {
            os << "Replicas:";
            for_each(srvs.begin(), srvs.end(),
                PrintChunkServerLocations(os));
            os << "\r\n";
        }
    } else {
        fa = metatree.getFattr(fid);
        if (! fa) {
            status    = -ENOENT;
            statusMsg = "no such file";
            return;
        }
    }
    if (fa) {
        if (gLayoutManager.VerifyAllOpsPermissions()) {
            SetEUserAndEGroup(*this);
            if (! fa->CanRead(euser, egroup)) {
                status = -EACCES;
                return;
            }
        }
        os << "Path-name: " << metatree.getPathname(fa) << "\r\n";
        FattrReply(fa, fattr);
    }
    result = os.str();
}

/* virtual */ void
MetaChmod::handle()
{
    MetaFattr* const fa = metatree.getFattr(fid);
    if (! fa) {
        status = -ENOENT;
        return;
    }
    if (! IsValidMode(mode, fa->type == KFS_DIR)) {
        status = -EINVAL;
        return;
    }
    SetEUserAndEGroup(*this);
    if (fa->user != euser && euser != kKfsUserRoot) {
        status = -EACCES;
        return;
    }
    status = 0;
    fa->mode = mode;
}

/* virtual */ void
MetaChown::handle()
{
    MetaFattr* const fa = metatree.getFattr(fid);
    if (! fa) {
        status = -ENOENT;
        return;
    }
    if (! SetUserAndGroup(*this)) {
        return;
    }
    if (fa->user != euser && euser != kKfsUserRoot) {
        status = -EACCES;
        return;
    }
    if (user != kKfsUserNone && euser != user && euser != kKfsUserRoot) {
        status = -EACCES;
        return;
    }
    if (group != kKfsGroupNone && euser != kKfsUserRoot) {
        kfsUid_t const owner = user == kKfsUserNone ? euser : user;
        if (! (authUid != kKfsUserNone ?
                gLayoutManager.GetUserAndGroup().IsGroupMember(owner, group) :
                IsGroupMember(owner, group))) {
            statusMsg = "user not a member of a group";
            status    = -EACCES;
            return;
        }
    }
    status = 0;
    if (user != kKfsUserNone) {
        fa->user = user;
    }
    if (group != kKfsGroupNone) {
        fa->group = group;
    }
}

/* virtual */ void
MetaChunkCorrupt::handle()
{
    if (server) {
        if (! chunkDir.empty()) {
            server->SetChunkDirStatus(chunkDir, dirOkFlag);
        }
        if (chunkId > 0) {
            gLayoutManager.ChunkCorrupt(this);
        }
    } else {
        // This is likely coming from the ClientSM.
        KFS_LOG_STREAM_DEBUG << "no server invalid cmd: " << Show() <<
        KFS_LOG_EOM;
        status = -EINVAL;
    }
}

/* virtual */ void
MetaChunkEvacuate::handle()
{
    if (server) {
        gLayoutManager.ChunkEvacuate(this);
    } else {
        // This is likely coming from the ClientSM.
        KFS_LOG_STREAM_DEBUG << "no server invalid cmd: " << Show() <<
        KFS_LOG_EOM;
        status = -EINVAL;
    }
}

/* virtual */ void
MetaChunkAvailable::handle()
{
    if (server) {
        gLayoutManager.ChunkAvailable(this);
    } else {
        // This is likely coming from the ClientSM.
        KFS_LOG_STREAM_DEBUG << "no server invalid cmd: " << Show() <<
        KFS_LOG_EOM;
        status = -EINVAL;
    }
}

/* virtual */ void
MetaChunkDirInfo::handle()
{
    if (server) {
        server->SetChunkDirInfo(dirName, props);
    } else {
        // This is likely coming from the ClientSM.
        KFS_LOG_STREAM_DEBUG << "no server invalid cmd: " << Show() <<
        KFS_LOG_EOM;
        status = -EINVAL;
    }
}

/* virtual */ void
MetaChunkReplicationCheck::handle()
{
    gLayoutManager.ChunkReplicationChecker();
    status = 0;
}

/* virtual */ void
MetaBeginMakeChunkStable::handle()
{
    gLayoutManager.BeginMakeChunkStableDone(this);
    status = 0;
}

int
MetaLogMakeChunkStable::logDone(int code, void *data)
{
    if (code != EVENT_CMD_DONE || data != this) {
        panic("MetaLogMakeChunkStable::logDone invalid invocation");
        return 1;
    }
    if (op == META_LOG_MAKE_CHUNK_STABLE) {
        gLayoutManager.LogMakeChunkStableDone(this);
    }
    delete this;
    return 0;
}

/* virtual */ void
MetaChunkMakeStable::handle()
{
    gLayoutManager.MakeChunkStableDone(this);
    status = 0;
}

/* virtual */ ostream&
MetaChunkMakeStable::ShowSelf(ostream& os) const
{
    return os <<
        "make-chunk-stable:"
        " server: "        << server->GetServerLocation() <<
        " seq: "           << opSeqno <<
        " status: "        << status <<
            (statusMsg.empty() ? "" : " ") << statusMsg <<
        " fileid: "        << fid <<
        " chunkid: "       << chunkId <<
        " chunkvers: "     << chunkVersion <<
        " chunkSize: "     << chunkSize <<
        " chunkChecksum: " << chunkChecksum
    ;
}

/* virtual */ void
MetaChunkSize::handle()
{
    status = gLayoutManager.GetChunkSizeDone(this);
}

/* virtual */ void
MetaChunkReplicate::handle()
{
    gLayoutManager.ChunkReplicationDone(this);
}

/* virtual */ ostream&
MetaChunkReplicate::ShowSelf(ostream& os) const
{
    return os <<
        (numRecoveryStripes > 0 ? "recover" : "replicate:") <<
        " chunk: "        << chunkId <<
        " version: "      << chunkVersion <<
        " file: "         << fid <<
        " fileSize: "     << fileSize <<
        " path: "         << pathname <<
        " recovStripes: " << numRecoveryStripes <<
        " seq: "          << opSeqno <<
        " from: "         << (dataServer ?
            dataServer->GetServerLocation() : ServerLocation()) <<
        " to: "           << (server ?
            server->GetServerLocation() : ServerLocation())
    ;
}

/* virtual */ void
MetaPing::handle()
{
    if (! HasMetaServerStatsAccess(*this)) {
        return;
    }
    status = 0;
    gLayoutManager.Ping(resp, gWormMode);

}

/* virtual */ void
MetaUpServers::handle()
{
    if (! HasEnoughIoBuffersForResponse(*this)) {
        return;
    }
    if (! HasMetaServerStatsAccess(*this)) {
        return;
    }
    ostream& os = sWOStream.Set(resp);
    gLayoutManager.UpServers(os);
    os.flush();
    sWOStream.Reset();
    if (! os) {
        resp.Clear();
        status    = -ENOMEM;
        statusMsg = "response exceeds max. size";
    }
}

/* virtual */ void
MetaRecomputeDirsize::handle()
{
    if (! HasMetaServerAdminAccess(*this)) {
        return;
    }
    status = 0;
    KFS_LOG_STREAM_INFO << "Processing a recompute dir size..." << KFS_LOG_EOM;
    metatree.recomputeDirSize();
}

static void SigHupHandler(int /* sinum */)
{
    _exit(1);
}

static void SigAlarmHandler(int /* sinum */)
{
    _exit(2);
}

static void ChildAtFork(int childTimeLimit)
{
    signal(SIGHUP, &SigHupHandler);
    signal(SIGALRM, &SigAlarmHandler);
    if (childTimeLimit > 0) {
        alarm(childTimeLimit);
    }
    if (MsgLogger::GetLogger()) {
        MsgLogger::GetLogger()->ChildAtFork();
    }
    AuditLog::ChildAtFork();
    globalNetManager().ChildAtFork();
    gNetDispatch.ChildAtFork();
}

static int DoFork(int childTimeLimit)
{
    gNetDispatch.PrepareCurrentThreadToFork();
    AuditLog::PrepareToFork();
    MsgLogger* const logger = MsgLogger::GetLogger();
    if (logger) {
        logger->PrepareToFork();
    }
    const int ret = fork();
    if (ret == 0) {
        ChildAtFork(childTimeLimit);
    } else {
        if (logger) {
            logger->ForkDone();
        }
        AuditLog::ForkDone();
    }
    return ret;
}

/* virtual */ void
MetaDumpChunkToServerMap::handle()
{
    suspended = false;
    if (pid > 0) {
        pid = -1;
        return; // Child finished.
    }
    if (! HasMetaServerAdminAccess(*this)) {
        return;
    }
    if (gChildProcessTracker.GetProcessCount() > 0) {
        statusMsg = "another child process running";
        status    = -EAGAIN;
        return;
    }
    if ((pid = DoFork(20 * 60)) == 0) {
        // let the child write out the map; if the map is large, this'll
        // take several seconds.  we get the benefits of writing out the
        // map in the background while the metaserver continues to
        // process other RPCs
        gLayoutManager.DumpChunkToServerMap(gChunkmapDumpDir);
        _exit(0); // Child does not do graceful exit.
    }
    KFS_LOG_STREAM_INFO << "chunk to server map writer pid: " << pid <<
    KFS_LOG_EOM;
    // if fork() failed, let the sender know
    if (pid < 0) {
        status = -1;
        return;
    }
    // hold on to the request until the child  finishes
    ostringstream& os = GetTmpOStringStream();
    os << gChunkmapDumpDir << "/chunkmap.txt";
    chunkmapFile = os.str();
    suspended = true;
    gChildProcessTracker.Track(pid, this);
}

/* virtual */ void
MetaDumpChunkReplicationCandidates::handle()
{
    if (! HasEnoughIoBuffersForResponse(*this)) {
        return;
    }
    if (! HasMetaServerAdminAccess(*this)) {
        return;
    }
    gLayoutManager.DumpChunkReplicationCandidates(this);
}

/* virtual */ void
MetaFsck::handle()
{
    suspended = false;
    resp.Clear();
    if (pid > 0) {
        if (! HasEnoughIoBuffersForResponse(*this)) {
            return;
        }
        // Child finished.
        pid = -1;
        if (status == 0) {
            int maxReadSize = min(
                gLayoutManager.GetMaxResponseSize(),
                sMaxFsckResponseSize);
            if (maxReadSize <= 0) {
                statusMsg = "out of io buffers";
                status    = -ENOMEM;
            } else {
                if (fd.empty()) {
                    statusMsg = "internal error";
                    status    = -EINVAL;
                    return;
                }
                for (Fds::const_iterator it = fd.begin();
                        maxReadSize > 0 &&
                            it != fd.end();
                        ++it) {
                    struct stat st = {0};
                    if (fstat(*it, &st) < 0) {
                        status    = -errno;
                        statusMsg = QCUtils::SysError(
                            -status);
                        if (status >= 0) {
                            status = -EIO;
                        }
                        break;
                    }
                    if (st.st_size <= 0 &&
                            it == fd.begin()) {
                        status = -EIO;
                        break;
                    }
                    const int nRead =
                        resp.Read(*it, maxReadSize);
                    if (st.st_size > maxReadSize &&
                            maxReadSize == nRead) {
                        ostream& os =
                            sWOStream.Set(resp);
                        os  <<
                            "\nWARNING: output"
                            " truncated to " <<
                            maxReadSize << " bytes"
                        "\n";
                        os.flush();
                        if (! os) {
                            resp.Clear();
                            statusMsg ="out of io "
                                "buffers";
                            status    = -ENOMEM;
                        }
                        sWOStream.Reset();
                        break;
                    }
                    if (nRead < st.st_size) {
                        statusMsg = "short read";
                        status    = -EIO;
                        break;
                    }
                    maxReadSize -= nRead;
                }
            }
        } else if (status > 0) {
            status = -status;
        }
        for (Fds::const_iterator it = fd.begin();
                it != fd.end();
                ++it) {
            close(*it);
        }
        fd.clear();
        if (status != 0) {
            status = status < 0 ? status : -EIO;
            if (statusMsg.empty()) {
                statusMsg = "fsck io failure";
            }
            resp.Clear();
        }
        return;
    }
    if (! HasMetaServerAdminAccess(*this)) {
        return;
    }
    if (gChildProcessTracker.GetProcessCount() > 0) {
        statusMsg = "another child process running";
        status    = -EAGAIN;
        return;
    }
    const int cnt = gLayoutManager.FsckStreamCount(
        reportAbandonedFilesFlag);
    if (cnt <= 0) {
        statusMsg = "internal error";
        status    = -EINVAL;
        return;
    }
    const char* const    suffix    = ".XXXXXX";
    const size_t         suffixLen = strlen(suffix);
    StBufferT<char, 128> buf;
    vector<string>       names;
    names.reserve(cnt);
    fd.reserve(cnt);
    for (int i = 0; i < cnt; i++) {
        char* const ptr = buf.Resize(sTmpName.length() + suffixLen + 1);
        memcpy(ptr, sTmpName.data(), sTmpName.size());
        strcpy(ptr + sTmpName.size(), suffix);
        const int tfd = mkstemp(ptr);
        if (tfd < 0) {
            status    = errno > 0 ? -errno : -EINVAL;
            statusMsg = "failed to create temporary file";
            while (--i >= 0) {
                close(fd[i]);
                unlink(names[i].c_str());
            }
            return;
        }
        fd.push_back(tfd);
        names.push_back(string(ptr));
    }
    if ((pid = DoFork((int)(gLayoutManager.GetMaxFsckTime() /
                    (1000 * 1000)))) == 0) {
        StBufferT<ostream*, 8> streamsPtrBuf;
        ostream** const ptr        = streamsPtrBuf.Resize(cnt + 1);
        ofstream* const streams    = new ofstream[cnt];
        bool            failedFlag = false;
        for (int i = 0; i < cnt; i++) {
            const char* const name = names[i].c_str();
            if (! failedFlag) {
                streams[i].open(name);
                ptr[i] = streams + i;
            }
            close(fd[i]);
            unlink(name);
            failedFlag = failedFlag || ! streams[i];
        }
        if (! failedFlag) {
            ptr[cnt] = 0;
            gLayoutManager.Fsck(ptr, reportAbandonedFilesFlag);
            failedFlag = false;
            for (int i = 0; i < cnt; i++) {
                streams[i].flush();
                streams[i].close();
                if (! streams[i]) {
                    failedFlag = true;
                    break;
                }
            }
            for (int i = 0; i < cnt; i++) {
                if (failedFlag) {
                    // Zero length file means error.
                    if (ftruncate(fd[i], 0) < 0) {
                        QCUtils::SetLastIgnoredError(errno);
                    }
                }
            }
        }
        delete [] streams;
        _exit(failedFlag ? 3 : 0); // Child does not do graceful close.
    }
    if (pid < 0) {
        status    = errno > 0 ? -errno : -EINVAL;
        statusMsg = "fork failure";
        for (int i = 0; i < cnt; i++) {
            close(fd[i]);
            unlink(names[i].c_str());
        }
        return;
    }
    KFS_LOG_STREAM_INFO << "fsck pid: " << pid <<
    KFS_LOG_EOM;
    suspended = true;
    gChildProcessTracker.Track(pid, this);
}

void
MetaFsck::SetParameters(const Properties& props)
{
    sTmpName = props.getValue(
        "metaServer.fsck.tmpfile",             sTmpName);
    sMaxFsckResponseSize = props.getValue(
        "metaServer.fsck.maxFsckResponseSize", sMaxFsckResponseSize);
}

string MetaFsck::sTmpName("/tmp/kfsfsck.tmp");
int    MetaFsck::sMaxFsckResponseSize(20 << 20);

/* virtual */ void
MetaCheckLeases::handle()
{
    if (! HasMetaServerAdminAccess(*this)) {
        return;
    }
    status = 0;
    gLayoutManager.CheckAllLeases();
}

/* virtual */ void
MetaStats::handle()
{
    if (! HasMetaServerStatsAccess(*this)) {
        return;
    }
    ostringstream& os = GetTmpOStringStream();
    status = 0;
    globals().counterManager.Show(os);
    stats = os.str();
}

/* virtual */ void
MetaOpenFiles::handle()
{
    if (! HasEnoughIoBuffersForResponse(*this)) {
        return;
    }
    if (! HasMetaServerAdminAccess(*this)) {
        return;
    }
    ReadInfo  openForRead;
    WriteInfo openForWrite;
    gLayoutManager.GetOpenFiles(openForRead, openForWrite);
    status = 0;
    ostream& os = sWOStream.Set(resp);
    for (ReadInfo::const_iterator it = openForRead.begin();
            it != openForRead.end();
            ++it) {
        os << it->first;
        for (std::vector<std::pair<chunkId_t, size_t> >::const_iterator
                i = it->second.begin();
                i != it->second.end();
                ++i) {
            if (! (os << "  " << i->first << " " << i->second)) {
                break;
            }
        }
        os << "\n";
    }
    os << "\n";
    for (WriteInfo::const_iterator it = openForWrite.begin();
            it != openForWrite.end();
            ++it) {
        os << it->first;
        for (std::vector<chunkId_t>::const_iterator
                i = it->second.begin();
                i != it->second.end();
                ++i) {
            if (! (os << " " << *i)) {
                break;
            }
        }
        os << "\n";
    }
    os.flush();
    if (! os) {
        resp.Clear();
        status    = -ENOMEM;
        statusMsg = "response exceeds max. size";
    } else {
        openForReadCnt  = openForRead.size();
        openForWriteCnt = openForWrite.size();
    }
    sWOStream.Reset();
}

/* virtual */ void
MetaSetChunkServersProperties::handle()
{
    if (! HasMetaServerAdminAccess(*this)) {
        return;
    }
    status = (int)properties.size();
    gLayoutManager.SetChunkServersProperties(properties);
}

/* virtual */ void
MetaGetChunkServersCounters::handle()
{
    if (! HasEnoughIoBuffersForResponse(*this)) {
        return;
    }
    if (! HasMetaServerStatsAccess(*this)) {
        return;
    }
    status = 0;
    gLayoutManager.GetChunkServerCounters(resp);
}

/* virtual */ void
MetaGetChunkServerDirsCounters::handle()
{
    if (! HasEnoughIoBuffersForResponse(*this)) {
        return;
    }
    if (! HasMetaServerStatsAccess(*this)) {
        return;
    }
    status = 0;
    gLayoutManager.GetChunkServerDirCounters(resp);
}

/* virtual */ void
MetaGetRequestCounters::handle()
{
    if (! HasEnoughIoBuffersForResponse(*this)) {
        return;
    }
    if (! HasMetaServerStatsAccess(*this)) {
        return;
    }
    status = 0;
    gNetDispatch.GetStatsCsv(resp);
    userCpuMicroSec   = gNetDispatch.GetUserCpuMicroSec();
    systemCpuMicroSec = gNetDispatch.GetSystemCpuMicroSec();
}

/* virtual */ void
MetaCheckpoint::handle()
{
    suspended = false;
    if (pid > 0) {
        // Child finished.
        KFS_LOG_STREAM(status == 0 ?
                MsgLogger::kLogLevelINFO :
                MsgLogger::kLogLevelERROR) <<
            "checkpoint: "    << lastCheckpointId <<
            " pid: "          << pid <<
            " done; status: " << status <<
            " failures: "     << failedCount <<
        KFS_LOG_EOM;
        if (status < 0) {
            failedCount++;
        } else {
            failedCount = 0;
            lastCheckpointId = runningCheckpointId;
        }
        if (lockFd >= 0) {
            close(lockFd);
        }
        if (failedCount > maxFailedCount) {
            panic("checkpoint failures", false);
        }
        runningCheckpointId = -1;
        pid = -1;
        return;
    }
    status = 0;
    if (intervalSec <= 0) {
        return; // Disabled.
    }
    const time_t now = globalNetManager().Now();
    if (lastCheckpointId < 0) {
        // First call -- init.
        lastCheckpointId = oplog.checkpointed();
        lastRun          = now;
        return;
    }
    if (now < lastRun + intervalSec) {
        return;
    }
    if (oplog.checkpointed() == lastCheckpointId &&
            ! cp.isCPNeeded()) {
        return;
    }
    if (lockFd >= 0) {
        close(lockFd);
    }
    if (! lockFileName.empty() &&
            (lockFd = try_to_acquire_lockfile(lockFileName)) < 0) {
        KFS_LOG_STREAM_INFO << "checkpoint: " <<
            " failed to acquire lock: " << lockFileName <<
            " " << QCUtils::SysError(lockFd) <<
        KFS_LOG_EOM;
        return; // Retry later.
    }
    status = oplog.finishLog();
    if (status != 0) {
        KFS_LOG_STREAM_ERROR << "failed to finish log:"
            " status:" << status <<
        KFS_LOG_EOM;
        if (lockFd >= 0) {
            close(lockFd);
        }
        return;
    }
    lastRun = now;
    // If logger decided not to start new log it won't reset checkpoint
    // mutation count.
    if (cp.isCPNeeded()) {
        if (lockFd >= 0) {
            close(lockFd);
        }
        if (lastCheckpointId != oplog.checkpointed()) {
            panic("finish log failure");
            return;
        }
        KFS_LOG_STREAM_WARN << "finish log: no new log started"
            "; delaying checkpoint" <<
        KFS_LOG_EOM;
        return;
    }
    runningCheckpointId = oplog.checkpointed();
    // DoFork() / PrepareCurrentThreadToFork() releases and re-acquires the
    // global mutex by waiting on condition with this mutex, but must ensure
    // that no other RPC gets processed. If checkpoint mutation count isn't
    // zero after DoFork() invocation, then there is a bug with the prepare to
    // fork logic, and checkpoint will not be valid. In such case do not write
    // the checkpoint in the child, and "panic" the parent.
    if ((pid = DoFork(checkpointWriteTimeoutSec)) == 0) {
        if (cp.isCPNeeded()) {
            status = -EINVAL;
        } else {
            metatree.disableFidToPathname();
            metatree.recomputeDirSize();
            cp.setWriteSyncFlag(checkpointWriteSyncFlag);
            cp.setWriteBufferSize(checkpointWriteBufferSize);
            status = cp.do_CP();
        }
        // Child does not attempt graceful exit.
        _exit(status == 0 ? 0 : 1);
    }
    if (cp.isCPNeeded()) {
        panic("checkpoint: meta data changed after prepare to fork");
    }
    KFS_LOG_STREAM(pid > 0 ?
            MsgLogger::kLogLevelINFO :
            MsgLogger::kLogLevelERROR) <<
        "checkpoint: " << lastCheckpointId <<
        " pid: "       << pid <<
    KFS_LOG_EOM;
    if (pid < 0) {
        status = -1;
        return;
    }
    suspended = true;
    gChildProcessTracker.Track(pid, this);
}

void
MetaCheckpoint::ScheduleNow()
{
    lastRun = globalNetManager().Now() - intervalSec - 1;
}

void
MetaCheckpoint::SetParameters(const Properties& props)
{
    intervalSec = props.getValue(
        "metaServer.checkpoint.interval",       intervalSec);
    lockFileName = props.getValue(
        "metaServer.checkpoint.lockFileName",   lockFileName);
    maxFailedCount = max(0, props.getValue(
        "metaServer.checkpoint.maxFailedCount", maxFailedCount));
    checkpointWriteTimeoutSec = max(0, (int)props.getValue(
        "metaServer.checkpoint.writeTimeoutSec",
        (double)checkpointWriteTimeoutSec));
    checkpointWriteSyncFlag = props.getValue(
        "metaServer.checkpoint.writeSync",
        checkpointWriteSyncFlag ? 0 : 1) != 0;
    checkpointWriteBufferSize = props.getValue(
        "metaServer.checkpoint.writeBufferSize",
        checkpointWriteBufferSize);
}

/*!
 * \brief add a new request to the queue: we used to have threads before; at
 * that time, the requests would be dropped into the queue and the request
 * processor would pick it up.  We have taken out threads; so this method is
 * just pass thru
 * \param[in] r the request
 */
void
submit_request(MetaRequest *r)
{
    const int64_t start = microseconds();
    if (r->submitCount++ == 0) {
        r->submitTime  = start;
        r->processTime = start;
    } else {
        // accumulate processing time.
        r->processTime = start - r->processTime;
    }
    r->handle();
    if (r->suspended) {
        r->processTime = microseconds() - r->processTime;
    } else {
        oplog.dispatch(r);
    }
}

/*!
 * \brief print out the leaf nodes for debugging
 */
void
printleaves()
{
    metatree.printleaves();
}

/*!
 * \brief log lookup request (nop)
 */
int
MetaLookup::log(ostream& /* file */) const
{
    return 0;
}

/*!
 * \brief log lookup path request (nop)
 */
int
MetaLookupPath::log(ostream& /* file */) const
{
    return 0;
}

/*!
 * \brief log a file create
 */
int
MetaCreate::log(ostream& file) const
{
    // use the log entry time as a proxy for when the file was created
    file << "create"
        "/dir/"         << dir <<
        "/name/"        << name <<
        "/id/"          << fid <<
        "/numReplicas/" << numReplicas <<
        "/ctime/"       << ShowTime(microseconds())
    ;
    if (striperType != KFS_STRIPED_FILE_TYPE_NONE) {
        file <<
            "/striperType/"        << striperType <<
            "/numStripes/"         << numStripes <<
            "/numRecoveryStripes/" << numRecoveryStripes <<
            "/stripeSize/"         << stripeSize
        ;
    }
    if (todumpster > 0) {
        file << "/todumpster/" << todumpster;
    }
    file << "/user/"  << user <<
        "/group/" << group <<
        "/mode/"  << mode
    ;
    if (minSTier < kKfsSTierMax) {
        file << "/minTier/" << (int)minSTier << "/maxTier/" << (int)maxSTier;
    }
    file << '\n';
    return file.fail() ? -EIO : 0;
}

/*!
 * \brief log a directory create
 */
int
MetaMkdir::log(ostream &file) const
{
    file << "mkdir"
        "/dir/"   << dir <<
        "/name/"  << name <<
        "/id/"    << fid <<
        "/ctime/" << ShowTime(microseconds()) <<
        "/user/"  << user <<
        "/group/" << group <<
        "/mode/"  << mode <<
    '\n';
    return file.fail() ? -EIO : 0;
}

/*!
 * \brief log a file deletion
 */
int
MetaRemove::log(ostream &file) const
{
    file << "remove/dir/" << dir << "/name/" << name;
    if (todumpster > 0) {
        file << "/todumpster/" << todumpster;
    }
    file << '\n';
    return file.fail() ? -EIO : 0;
}

/*!
 * \brief log a directory deletion
 */
int
MetaRmdir::log(ostream &file) const
{
    file << "rmdir/dir/" << dir << "/name/" << name << '\n';
    return file.fail() ? -EIO : 0;
}

/*!
 * \brief log directory read (nop)
 */
int
MetaReaddir::log(ostream &file) const
{
    return 0;
}

/*!
 * \brief log directory read (nop)
 */
int
MetaReaddirPlus::log(ostream &file) const
{
    return 0;
}

/*!
 * \brief log getalloc (nop)
 */
int
MetaGetalloc::log(ostream &file) const
{
    return 0;
}

/*!
 * \brief log getlayout (nop)
 */
int
MetaGetlayout::log(ostream &file) const
{
    return 0;
}

/*!
 * \brief log a chunk allocation
 */
int
MetaAllocate::log(ostream &file) const
{
    if (! logFlag) {
        return 0;
    }
    // use the log entry time as a proxy for when the block was created/file
    // was modified
    file << "allocate/file/" << fid << "/offset/" << offset
        << "/chunkId/" << chunkId
        << "/chunkVersion/" << chunkVersion
        << "/mtime/" << ShowTime(microseconds())
        << "/append/" << (appendChunk ? 1 : 0)
        << '\n';
    return file.fail() ? -EIO : 0;
}

/*!
 * \brief log a file truncation
 */
int
MetaTruncate::log(ostream &file) const
{
    // use the log entry time as a proxy for when the file was modified
    if (pruneBlksFromHead) {
        file << "pruneFromHead/file/" << fid << "/offset/" << offset
            << "/mtime/" << ShowTime(mtime) << '\n';
    } else {
        file << "truncate/file/" << fid << "/offset/" << offset
            << "/mtime/" << ShowTime(mtime);
        if (endOffset >= 0) {
            file << "/endoff/" << endOffset;
        }
        file << '\n';
    }
    return file.fail() ? -EIO : 0;
}

/*!
 * \brief log a rename
 */
int
MetaRename::log(ostream &file) const
{
    file << "rename"
        "/dir/" << dir <<
        "/old/" << oldname <<
        "/new/" << newname
    ;
    if (todumpster > 0) {
        // Insert sentinel empty entry for pop_path() to work.
        file << "//todumpster/" << todumpster;
    }
    file << '\n';
    return file.fail() ? -EIO : 0;
}

/*!
 * \brief log a block coalesce
 */
int
MetaCoalesceBlocks::log(ostream &file) const
{
    file << "coalesce"
        "/old/"   << srcFid <<
        "/new/"   << dstFid <<
        "/count/" << numChunksMoved <<
        "/mtime/" << ShowTime(mtime) <<
    '\n';
    return file.fail() ? -EIO : 0;
}

/*!
 * \brief log a setmtime
 */
int
MetaSetMtime::log(ostream &file) const
{
    file << "setmtime/file/" << fid
        << "/mtime/" << ShowTime(mtime) << '\n';
    return file.fail() ? -EIO : 0;
}

/*!
 * \brief log change file replication
 */
int
MetaChangeFileReplication::log(ostream &file) const
{
    if (! logFlag) {
        return 0;
    }
    file << "setrep/file/" << fid << "/replicas/" << numReplicas;
    if (minSTier != kKfsSTierUndef && maxSTier != kKfsSTierUndef) {
        file << "/minTier/" << (int)minSTier << "/maxTier/" << (int)maxSTier;
    }
    file  << '\n';
    return file.fail() ? -EIO : 0;
}

/*!
 * \brief log retire chunkserver (nop)
 */
int
MetaRetireChunkserver::log(ostream &file) const
{
    return 0;
}

/*!
 * \brief log toggling of metaserver WORM state (nop)
 */
int
MetaToggleWORM::log(ostream &file) const
{
    return 0;
}

/*!
 * \brief for a chunkserver hello, there is nothing to log
 */
int
MetaHello::log(ostream &file) const
{
    return 0;
}

/*!
 * \brief for a chunkserver's death, there is nothing to log
 */
int
MetaBye::log(ostream &file) const
{
    return 0;
}

/*!
 * \brief When asking a chunkserver for a chunk's size, there is
 * write out the estimate of the file's size.
 */
int
MetaChunkSize::log(ostream &file) const
{
    if (filesize < 0)
        return 0;

    file << "size/file/" << fid << "/filesize/" << filesize << '\n';
    return file.fail() ? -EIO : 0;
}

/*!
 * \brief for a ping, there is nothing to log
 */
int
MetaPing::log(ostream &file) const
{
    return 0;
}

/*!
 * \brief for a request of upserver, there is nothing to log
 */
int
MetaUpServers::log(ostream &file) const
{
    return 0;
}

/*!
 * \brief for a stats request, there is nothing to log
 */
int
MetaStats::log(ostream &file) const
{
    return 0;
}

/*!
 * \brief for a map dump request, there is nothing to log
 */
int
MetaDumpChunkToServerMap::log(ostream &file) const
{
    return 0;
}

/*!
 * \brief for a fsck request, there is nothing to log
 */
int
MetaFsck::log(ostream &file) const
{
    return 0;
}


/*!
 * \brief for a recompute dir size request, there is nothing to log
 */
int
MetaRecomputeDirsize::log(ostream &file) const
{
    return 0;
}

/*!
 * \brief for a check all leases request, there is nothing to log
 */
int
MetaCheckLeases::log(ostream &file) const
{
    return 0;
}

/*!
 * \brief for a dump chunk replication candidates request, there is nothing to log
 */
int
MetaDumpChunkReplicationCandidates::log(ostream &file) const
{
    return 0;
}

/*!
 * \brief for an open files request, there is nothing to log
 */
int
MetaOpenFiles::log(ostream &file) const
{
    return 0;
}

int
MetaSetChunkServersProperties::log(ostream & /* file */) const
{
    return 0;
}

int
MetaGetChunkServersCounters::log(ostream & /* file */) const
{
    return 0;
}

int
MetaGetChunkServerDirsCounters::log(ostream & /* file */) const
{
    return 0;
}

/*!
 * \brief for an open files request, there is nothing to log
 */
int
MetaChunkCorrupt::log(ostream &file) const
{
    return 0;
}

/*!
 * \brief for a lease acquire request, there is nothing to log
 */
int
MetaLeaseAcquire::log(ostream &file) const
{
    return 0;
}

/*!
 * \brief for a lease renew request, there is nothing to log
 */
int
MetaLeaseRenew::log(ostream &file) const
{
    return 0;
}

/*!
 * \brief for a lease renew relinquish, there is nothing to log
 */
int
MetaLeaseRelinquish::log(ostream &file) const
{
    return 0;
}

/*!
 * \brief for a lease cleanup request, there is nothing to log
 */
int
MetaLeaseCleanup::log(ostream &file) const
{
    return 0;
}

/*!
 * \brief This is an internally generated op.  There is
 * nothing to log.
 */
int
MetaChunkReplicationCheck::log(ostream &file) const
{
    return 0;
}

/*!
 * \brief This is an internally generated op. Log chunk id, size, and checksum.
 */
int
MetaLogMakeChunkStable::log(ostream &file) const
{
    if (chunkVersion < 0) {
        KFS_LOG_STREAM_WARN << "invalid chunk version ignoring: " <<
            Show() <<
        KFS_LOG_EOM;
        return 0;
    }
    file << "mkstable"         <<
            (op == META_LOG_MAKE_CHUNK_STABLE ? "" : "done") <<
        "/fileId/"         << fid <<
        "/chunkId/"        << chunkId <<
        "/chunkVersion/"   << chunkVersion  <<
        "/size/"           << chunkSize <<
        "/checksum/"       << chunkChecksum <<
        "/hasChecksum/"    << (hasChunkChecksum ? 1 : 0) <<
    '\n';
    return file.fail() ? -EIO : 0;
}

int
MetaChmod::log(ostream& file) const
{
    file << "chmod" <<
        "/file/" << fid <<
        "/mode/" << mode <<
    '\n';
    return file.fail() ? -EIO : 0;
}

int
MetaChown::log(ostream& file) const
{
    file << "chown" <<
        "/file/"  << fid <<
        "/user/"  << user <<
        "/group/" << group <<
    '\n';
    return file.fail() ? -EIO : 0;
}

/*!
 * \brief Various parse handlers.  All of them follow the same model:
 * If parse is successful, returns a dynamically
 * allocated meta request object. It is the callers responsibility to dispose
 * of this pointer.
 */

void
MetaRequest::Init()
{
    MetaRequestsList::Init(*this);
    QCStMutexLocker locker(gNetDispatch.GetClientManagerMutex());
    MetaRequestsList::PushBack(sMetaRequestsPtr, *this);
    sMetaRequestCount++;
}

/* virtual */
MetaRequest::~MetaRequest()
{
    QCStMutexLocker locker(gNetDispatch.GetClientManagerMutex());
    MetaRequestsList::Remove(sMetaRequestsPtr, *this);
    sMetaRequestCount--;
}

/* virtual */ void
MetaRequest::handle()
{
    status = -ENOSYS;  // Not implemented
}

/* static */ void
MetaRequest::SetParameters(const Properties& props)
{
    sRequireHeaderChecksumFlag = props.getValue(
        "metaServer.request.requireHeaderChecksum", 0) != 0;
    sVerifyHeaderChecksumFlag = props.getValue(
        "metaServer.request.verifyHeaderChecksum", 1) != 0;
}

/* static */ uint32_t
MetaRequest::Checksum(
    const char* name,
    size_t      nameLen,
    const char* header,
    size_t      headerLen)
{
    return ComputeBlockChecksum(
        ComputeBlockChecksum(name, nameLen), header, headerLen);
}

bool MetaRequest::sRequireHeaderChecksumFlag  = false;
bool MetaRequest::sVerifyHeaderChecksumFlag   = true;
int  MetaRequest::sMetaRequestCount           = 0;
MetaRequest* MetaRequest::sMetaRequestsPtr[1] = {0};

bool MetaCreate::Validate()
{
    return (dir >= 0 && ! name.empty() && numReplicas > 0);
}

/*!
 * \brief Generate response (a string) for various requests that
 * describes the result of the request execution.  The generated
 * response string is based on the KFS protocol.  All follow the same
 * model:
 * @param[out] os: A string stream that contains the response.
 */
void
MetaLookup::response(ostream& os)
{
    if (! OkHeader(this, os)) {
        return;
    }
    if (authType != kAuthenticationTypeUndef) {
        os << "Auth-type: " << authType << "\r\n\r\n";
        return;
    }
    os <<
        "EUserId: "  << euser  << "\r\n"
        "EGroupId: " << egroup << "\r\n"
    ;
    if (authInfoOnlyFlag) {
        os <<
            "User: "  << authUid  << "\r\n"
            "Group: " << authGid << "\r\n"
        ;
        const UserAndGroupNames* const ugn = GetUserAndGroupNames(*this);
        UserAndGroupNamesReply(os, ugn, euser,   egroup, "E");
        UserAndGroupNamesReply(os, ugn, authUid, authGid) << "\r\n";
    } else {
        FattrReply(os, fattr, GetUserAndGroupNames(*this)) << "\r\n";
    }
}

void
MetaLookupPath::response(ostream &os)
{
    if (! OkHeader(this, os)) {
        return;
    }
    os <<
        "EUserId: "  << euser  << "\r\n"
        "EGroupId: " << egroup << "\r\n"
    ;
    FattrReply(os, fattr, GetUserAndGroupNames(*this)) << "\r\n";
}

void
MetaCreate::response(ostream &os)
{
    if (! OkHeader(this, os)) {
        return;
    }
    os << "File-handle: "  << fid << "\r\n";
    if (striperType != KFS_STRIPED_FILE_TYPE_NONE) {
        os << "Striper-type: " << striperType << "\r\n";
    }
    os <<
    "User: "  << user  <<  "\r\n"
    "Group: " << group <<  "\r\n"
    "Mode: "  << mode  <<  "\r\n"
    ;
    if (minSTier < kKfsSTierMax) {
        os <<
        "Min-tier: " << (int)minSTier << "\r\n"
        "Max-tier: " << (int)maxSTier << "\r\n";
    }
    UserAndGroupNamesReply(os, GetUserAndGroupNames(*this), user, group) <<
    "\r\n";
}

void
MetaRemove::response(ostream &os)
{
    PutHeader(this, os) << "\r\n";
}

void
MetaMkdir::response(ostream &os)
{
    if (! OkHeader(this, os)) {
        return;
    }
    os <<
    "File-handle: " << fid   << "\r\n"
    "User: "        << user  << "\r\n"
    "Group: "       << group << "\r\n"
    "Mode: "        << mode  << "\r\n"
    ;
    if (minSTier < kKfsSTierMax) {
        os <<
        "Min-tier: " << (int)minSTier << "\r\n"
        "Max-tier: " << (int)maxSTier << "\r\n";
    }
    UserAndGroupNamesReply(os, GetUserAndGroupNames(*this), user, group) <<
    "\r\n";
}

void
MetaRmdir::response(ostream &os)
{
    PutHeader(this, os) << "\r\n";
}

void
MetaReaddir::response(ostream& os, IOBuffer& buf)
{
    if (! OkHeader(this, os)) {
        return;
    }
    os <<
        "Num-Entries: "      << numEntries << "\r\n"
        "Has-more-entries: " << (hasMoreEntriesFlag ? 1 : 0) << "\r\n"
        "Content-length: "   << resp.BytesConsumable() << "\r\n"
    "\r\n";
    os.flush();
    buf.Move(&resp);
}

void
MetaReaddirPlus::response(ostream& os, IOBuffer& buf)
{
    if (! OkHeader(this, os)) {
        return;
    }
    size_t   entryCount;
    IOBuffer resp;
    if (numEntries >= 0) {
        ReaddirPlusWriter<true> writer(
            resp,
            maxRespSize,
            getLastChunkInfoOnlyIfSizeUnknown,
            omitLastChunkInfoFlag,
            fileIdAndTypeOnlyFlag);
        entryCount = writer.Write(dentries, lastChunkInfos,
            noAttrsFlag, GetUserAndGroupNames(*this));
    } else {
        ReaddirPlusWriter<false> writer(
            resp,
            maxRespSize,
            getLastChunkInfoOnlyIfSizeUnknown,
            omitLastChunkInfoFlag,
            fileIdAndTypeOnlyFlag);
        entryCount = writer.Write(dentries, lastChunkInfos,
            noAttrsFlag, GetUserAndGroupNames(*this));
    }
    hasMoreEntriesFlag = hasMoreEntriesFlag || entryCount < dentries.size();
    dentries.clear();
    lastChunkInfos.clear();
    if (ioBufPending > 0) {
        gLayoutManager.ChangeIoBufPending(-ioBufPending);
        ioBufPending = 0;
    }
    if (hasMoreEntriesFlag && numEntries < 0) {
        resp.Clear();
        status    = -ENOMEM;
        statusMsg = "response exceeds max. size";
        OkHeader(this, os);
        return;
    }
    os <<
        "Num-Entries: "      << entryCount << "\r\n"
        "Has-more-entries: " << (hasMoreEntriesFlag ? 1 : 0) << "\r\n"
        "Content-length: "   << resp.BytesConsumable() << "\r\n"
    "\r\n";
    os.flush();
    buf.Move(&resp);
}

void
MetaRename::response(ostream &os)
{
    PutHeader(this, os) << "\r\n";
}

void
MetaSetMtime::response(ostream &os)
{
    PutHeader(this, os) << "\r\n";
}

void
MetaGetalloc::response(ostream& os)
{
    if (! OkHeader(this, os)) {
        return;
    }
    os <<
        "Chunk-handle: " << chunkId << "\r\n"
        "Chunk-version: " << chunkVersion << "\r\n";
    if (replicasOrderedFlag) {
        os << "Replicas-ordered: 1\r\n";
    }
    os << "Num-replicas: " << locations.size() << "\r\n";

    assert(locations.size() > 0);

    os << "Replicas:";
    for_each(locations.begin(), locations.end(), ListServerLocations(os));
    os << "\r\n\r\n";
}

void
MetaGetlayout::response(ostream& os, IOBuffer& buf)
{
    if (! OkHeader(this, os)) {
        return;
    }
    if (hasMoreChunksFlag) {
        os << "Has-more-chunks:  1\r\n";
    }
    if (0 <= fileSize) {
        os << "File-size: " << fileSize << "\r\n";
    }
    os <<
        "Num-chunks: "     << numChunks << "\r\n"
        "Content-length: " << resp.BytesConsumable() << "\r\n"
    "\r\n";
    os.flush();
    buf.Move(&resp);
}

void
MetaAllocate::response(ostream& os)
{
    if (! OkHeader(this, os)) {
        return;
    }
    writeChunkAccess(os);
    responseSelf(os);
}

void
MetaAllocate::writeChunkAccess(ostream& os)
{
    if (authUid == kKfsUserNone) {
        return;
    }
    if (! responseAccessStr.empty()) {
        os.write(responseAccessStr.data(), responseAccessStr.size());
        return;
    }
    if (validForTime <= 0) {
        return;
    }
    if (clientCSAllowClearTextFlag) {
        os << "CS-clear-text: 1\r\n";
    }
    os <<
        "CS-acess-issued: " << issuedTime   << "\r\n"
        "CS-acess-time: "   << validForTime << "\r\n"
        "CS-access: ";
    const int16_t kDelegationFlags = 0;
    DelegationToken::WriteTokenAndSessionKey(
        os,
        authUid,
        tokenSeq,
        writeMasterKeyId,
        issuedTime,
        kDelegationFlags,
        validForTime,
        writeMasterKey.GetPtr(),
        writeMasterKey.GetSize()
    );
    os <<
        "\r\n"
        "C-access: ";
    ChunkAccessToken::WriteToken(
        os,
        chunkId,
        authUid,
        tokenSeq,
        writeMasterKeyId,
        issuedTime,
        ChunkAccessToken::kAllowWriteFlag |
            (clientCSAllowClearTextFlag ?
                ChunkAccessToken::kAllowClearTextFlag : 0),
        LEASE_INTERVAL_SECS * 2,
        writeMasterKey.GetPtr(),
        writeMasterKey.GetSize()
    );
    os << "\r\n";
}

void
MetaAllocate::responseSelf(ostream& os)
{
    if (status < 0) {
        return;
    }
    if (! responseStr.empty()) {
        os.write(responseStr.data(), responseStr.size());
        return;
    }
    os <<
        "Chunk-handle: "  << chunkId      << "\r\n"
        "Chunk-version: " << chunkVersion << "\r\n";
    if (appendChunk) {
        os << "Chunk-offset: " << offset << "\r\n";
    }
    assert((! servers.empty() && master) || invalidateAllFlag);
    if (master) {
        os << "Master: " << master->GetServerLocation() << "\r\n";
    }
    os << "Num-replicas: " << servers.size() << "\r\n";
    if (! servers.empty()) {
        os << "Replicas:";
        for_each(servers.begin(), servers.end(),
            PrintChunkServerLocations(os));
    }
    os << "\r\n\r\n";
}

void
MetaLeaseAcquire::response(ostream& os, IOBuffer& buf)
{
    DelegationToken::TokenSeq tokenSeq =
        (DelegationToken::TokenSeq)(leaseId >> 1);
    const size_t              count    = chunkAccess.GetSize();
    if (status == 0 && 0 < count && leaseId <= 0 &&
            ! CryptoKeys::PseudoRand(&tokenSeq, sizeof(tokenSeq))) {
        status    = -EFAULT;
        statusMsg = "pseudo random generator failure";
    }
    if (! OkHeader(this, os)) {
        return;
    }
    if (leaseId >= 0) {
        os << "Lease-id: " << leaseId << "\r\n";
    }
    if (clientCSAllowClearTextFlag) {
        os << "CS-clear-text: 1\r\n";
    }
    if (0 < count) {
        os << "CS-access: " << count << "\r\n";
    }
    if (0 < validForTime) {
        os <<
            "CS-acess-issued: " << issuedTime   << "\r\n"
            "CS-acess-time: "   << validForTime << "\r\n";
    }
    if (! getChunkLocationsFlag && ! responseBuf.IsEmpty()) {
        os << "Lease-ids:";
        os.flush();
        responseBuf.CopyIn("\r\n", 2);
        buf.Move(&responseBuf);
    }
    if (0 < count) {
        IntIOBufferWriter            writer(responseBuf);
        const ChunkAccessInfo*       ptr = chunkAccess.GetPtr();
        const ChunkAccessInfo* const end = ptr + count;
        if (! responseBuf.IsEmpty() && ptr < end) {
            writer.Write("\n", 1);
        }
        while (ptr < end) {
            writer.WriteHexInt(ptr->chunkId);
            writer.Write(" ", 1);
            writer.Write(
                ptr->serverLocation.hostname.data(),
                ptr->serverLocation.hostname.size());
            writer.Write(" ", 1);
            writer.WriteHexInt(ptr->serverLocation.port);
            writer.Write(" ", 1);
            const int16_t kDelegationFlags = 0;
            if (ptr->authUid == kKfsUserNone) {
                writer.Write("? ? ?", 5);
            } else {
                DelegationToken::WriteTokenAndSessionKey(
                    writer,
                    ptr->authUid,
                    tokenSeq,
                    ptr->keyId,
                    issuedTime,
                    kDelegationFlags,
                    validForTime,
                    ptr->key.GetPtr(),
                    ptr->key.GetSize()
                );
                writer.Write(" ", 1);
                ChunkAccessToken::WriteToken(
                    writer,
                    ptr->chunkId,
                    ptr->authUid,
                    tokenSeq ^ (uint32_t)leaseId,
                    ptr->keyId,
                    issuedTime,
                    (appendRecoveryFlag ?
                        ChunkAccessToken::kAppendRecoveryFlag :
                        ChunkAccessToken::kAllowReadFlag) |
                        (clientCSAllowClearTextFlag ?
                            ChunkAccessToken::kAllowClearTextFlag : 0),
                    leaseTimeout * 2,
                    ptr->key.GetPtr(),
                    ptr->key.GetSize()
                );
            }
            tokenSeq++;
            writer.Write("\n", 1);
            ptr++;
        }
        writer.Close();
    }
    const int len = responseBuf.BytesConsumable();
    if (len <= 0) {
        buf.CopyIn("\r\n", 2);
        return;
    }
    char tbuf[48];
    char* const end = tbuf + sizeof(tbuf);
    char* const pe  = end - 4;
    memcpy(pe, "\r\n\r\n", end - pe);
    char* const pn = IntToDecString(len, pe);
    char* const p  = pn - 16;
    memcpy(p, "Content-length: ", pn - p);
    buf.CopyIn(p, end - p);
    buf.Move(&responseBuf);
    return;
}

void
MetaLeaseRenew::response(ostream& os, IOBuffer& buf)
{
    if (! OkHeader(this, os)) {
        return;
    }
    if (clientCSAllowClearTextFlag) {
        os << "CS-clear-text: 1\r\n";
    }
    const size_t count = chunkAccess.GetSize();
    if (count <= 0) {
        os << "\r\n";
        return;
    }
    if (0 < validForTime) {
        os <<
            "CS-acess-issued: " << issuedTime   << "\r\n"
            "CS-acess-time: "   << validForTime << "\r\n";
    }
    IOBuffer                     iobuf;
    IntIOBufferWriter            writer(iobuf);
    const ChunkAccessInfo*       ptr = chunkAccess.GetPtr();
    const ChunkAccessInfo* const end = ptr + count;
    while (ptr < end) {
        if (leaseType != WRITE_LEASE) {
            writer.Write(
                ptr->serverLocation.hostname.data(),
                ptr->serverLocation.hostname.size());
            writer.Write(" ", 1);
            writer.WriteHexInt(ptr->serverLocation.port);
            writer.Write(" ", 1);
            if (0 < validForTime) {
                const int16_t kDelegationFlags = 0;
                DelegationToken::WriteTokenAndSessionKey(
                    writer,
                    ptr->authUid,
                    tokenSeq,
                    ptr->keyId,
                    issuedTime,
                    kDelegationFlags,
                    validForTime,
                    ptr->key.GetPtr(),
                    ptr->key.GetSize()
                );
                writer.Write(" ", 1);
            }
        }
        ChunkAccessToken::WriteToken(
            writer,
            ptr->chunkId,
            ptr->authUid,
            tokenSeq,
            ptr->keyId,
            issuedTime,
            (leaseType == WRITE_LEASE ?
                (DelegationToken::kChunkServerFlag |
                    ChunkAccessToken::kAllowWriteFlag) :
                ChunkAccessToken::kAllowReadFlag) |
                (clientCSAllowClearTextFlag ?
                    ChunkAccessToken::kAllowClearTextFlag : 0),
            LEASE_INTERVAL_SECS * 2,
            ptr->key.GetPtr(),
            ptr->key.GetSize()
        );
        tokenSeq++;
        writer.Write("\n", 1);
        ptr++;
    }
    if (leaseType == WRITE_LEASE && 0 < validForTime &&
            (ptr = chunkAccess.GetPtr()) < end) {
        os << "C-access-length: " << writer.GetTotalSize() << "\r\n";
        DelegationToken::Subject* const kNoSubjectPtr = 0;
        const ChunkAccessInfo* prev = 0;
        do {
            DelegationToken::WriteTokenAndSessionKey(
                writer,
                ptr->authUid,
                tokenSeq,
                ptr->keyId,
                issuedTime,
                DelegationToken::kChunkServerFlag,
                validForTime,
                ptr->key.GetPtr(),
                ptr->key.GetSize(),
                kNoSubjectPtr,
                prev ? prev->keyId         : kfsKeyId_t(),
                prev ? prev->key.GetPtr()  : 0,
                prev ? prev->key.GetSize() : 0
            );
            writer.Write("\n", 1);
            prev = ptr++;
            tokenSeq++;
        } while (ptr < end);
    }
    writer.Close();
    os <<
        "C-access: "       << count                   << "\r\n"
        "Content-length: " << iobuf.BytesConsumable() << "\r\n"
    "\r\n";
    os.flush();
    buf.Move(&iobuf);
}

void
MetaLeaseRelinquish::response(ostream &os)
{
    PutHeader(this, os) << "\r\n";
}

void
MetaCoalesceBlocks::response(ostream &os)
{
    if (! OkHeader(this, os)) {
        return;
    }
    os <<
        "Dst-start-offset: " << dstStartOffset  << "\r\n"
        "M-Time: "           << ShowTime(mtime) << "\r\n"
    "\r\n";
}

void
MetaHello::response(ostream &os)
{
    if (! OkHeader(this, os)) {
        return;
    }
    if (0 <= metaFileSystemId) {
        os << "File-system-id: " << metaFileSystemId << "\r\n";
        if (deleteAllChunksFlag) {
            os << "Delete-all-chunks: " << metaFileSystemId << "\r\n";
        }
    }
    os << "\r\n";
}

void
MetaChunkCorrupt::response(ostream &os)
{
    if (noReplyFlag) {
        return;
    }
    PutHeader(this, os) << "\r\n";
}

void
MetaChunkEvacuate::response(ostream& os)
{
    PutHeader(this, os) << "\r\n";
}

void
MetaChunkAvailable::response(ostream& os)
{
    PutHeader(this, os) << "\r\n";
}

void
MetaChunkDirInfo::response(ostream& os)
{
    if (noReplyFlag) {
        return;
    }
    PutHeader(this, os) << "\r\n";
}

void
MetaTruncate::response(ostream &os)
{
    if (! OkHeader(this, os)) {
        return;
    }
    if (endOffset >= 0) {
        os << "End-offset: " << endOffset << "\r\n";
    }
    os << "\r\n";
}

void
MetaChangeFileReplication::response(ostream &os)
{
    PutHeader(this, os) <<
        "Num-replicas: " << numReplicas << "\r\n\r\n";
}

void
MetaRetireChunkserver::response(ostream &os)
{
    PutHeader(this, os) << "\r\n";
}

void
MetaToggleWORM::response(ostream &os)
{
    PutHeader(this, os) << "\r\n";
}

void
MetaPing::response(ostream &os, IOBuffer& buf)
{
    if (! OkHeader(this, os)) {
        return;
    }
    os.flush();
    buf.Move(&resp);
}

void
MetaUpServers::response(ostream& os, IOBuffer& buf)
{
    if (! OkHeader(this, os)) {
        return;
    }
    os << "Content-length: " << resp.BytesConsumable() << "\r\n\r\n";
    os.flush();
    buf.Move(&resp);
}

void
MetaStats::response(ostream &os)
{
    PutHeader(this, os) << stats << "\r\n";
}

void
MetaCheckLeases::response(ostream &os)
{
    PutHeader(this, os) << "\r\n";
}

void
MetaRecomputeDirsize::response(ostream &os)
{
    PutHeader(this, os) << "\r\n";
}

void
MetaDumpChunkToServerMap::response(ostream &os)
{
    PutHeader(this, os) << "Filename: " << chunkmapFile << "\r\n\r\n";
}

void
MetaDumpChunkReplicationCandidates::response(ostream &os, IOBuffer& buf)
{
    if (! OkHeader(this, os)) {
        return;
    }
    os <<
        "Num-replication: "       << numReplication         << "\r\n"
        "Num-pending-recovery: "  << numPendingRecovery     << "\r\n"
        "Content-length: "        << resp.BytesConsumable() << "\r\n"
    "\r\n";
    os.flush();
    buf.Move(&resp);
}

void
MetaFsck::response(ostream &os, IOBuffer& buf)
{
    if (! OkHeader(this, os)) {
        return;
    }
    os  << "Content-length: " << resp.BytesConsumable() << "\r\n\r\n";
    os.flush();
    buf.Move(&resp);
}

void
MetaOpenFiles::response(ostream& os, IOBuffer& buf)
{
    if (! OkHeader(this, os)) {
        return;
    }
    os <<
        "Read: "  << openForReadCnt  << "\r\n"
        "Write: " << openForWriteCnt << "\r\n"
        "Content-length: " << resp.BytesConsumable() << "\r\n"
    "\r\n";
    os.flush();
    buf.Move(&resp);
}

void
MetaSetChunkServersProperties::response(ostream &os)
{
    PutHeader(this, os) << "\r\n";
}

void
MetaGetChunkServersCounters::response(ostream &os, IOBuffer& buf)
{
    if (! OkHeader(this, os)) {
        return;
    }
    os << "Content-length: " << resp.BytesConsumable() << "\r\n\r\n";
    os.flush();
    buf.Move(&resp);
}

void
MetaGetChunkServerDirsCounters::response(ostream &os, IOBuffer& buf)
{
    if (! OkHeader(this, os)) {
        return;
    }
    os << "Content-length: " << resp.BytesConsumable() << "\r\n\r\n";
    os.flush();
    buf.Move(&resp);
}

void
MetaGetRequestCounters::response(ostream &os, IOBuffer& buf)
{
    if (! OkHeader(this, os)) {
        return;
    }
    os <<
        "Content-length: " << resp.BytesConsumable()  << "\r\n"
        "User-cpu-micro-sec: "  << userCpuMicroSec    << "\r\n"
        "System-cpu-mcro-sec: " << systemCpuMicroSec  << "\r\n"
    "\r\n";
    os.flush();
    buf.Move(&resp);
}

void
MetaGetPathName::response(ostream& os)
{
    if (! OkHeader(this, os)) {
        return;
    }
    os << result;
    FattrReply(os, fattr, GetUserAndGroupNames(*this)) << "\r\n";
}

void
MetaChmod::response(ostream& os)
{
    PutHeader(this, os) << "\r\n";
}

void
MetaChown::response(ostream& os)
{
    if (! OkHeader(this, os)) {
        return;
    }
    os <<
    "User: "  << user  << "\r\n"
    "Group: " << group << "\r\n"
    ;
    UserAndGroupNamesReply(os, GetUserAndGroupNames(*this), user, group) <<
    "\r\n";
}

/* virtual */ bool
MetaAuthenticate::dispatch(ClientSM& sm)
{
    return sm.Handle(*this);
}

void
MetaAuthenticate::response(ostream& os)
{
    if (! OkHeader(this, os)) {
        return;
    }
    os <<
        "Auth-type: " << responseAuthType               << "\r\n"
        "Use-ssl: "   << (filter ? 1 : 0)               << "\r\n"
        "Curtime: "   << (int64_t)time(0)               << "\r\n"
        "Endtime: "   << (int64_t)sessionExpirationTime << "\r\n"
    ;
    if (responseContentLen <= 0) {
        os << "\r\n";
        return;
    }
    os << "Content-length: " << responseContentLen << "\r\n"
    "\r\n";
    os.write(responseContentPtr, responseContentLen);
}

/* virtual */ bool
MetaDelegate::dispatch(ClientSM& sm)
{
    return sm.Handle(*this);
}

void
MetaDelegate::response(ostream& os)
{
    if (status == 0 && validForTime <= 0) {
        status    = -EINVAL;
        statusMsg = "invalid 0 delegation expiration time";
    }
    if (status == 0 && (! fromClientSMFlag || ! clnt)) {
        status    = -EPERM;
        statusMsg = "no client authentication";
    }
    if (status == 0 && authUid == kKfsUserNone) {
        status    = -EPERM;
        statusMsg = "not authenticated";
    }
    const CryptoKeys* const keys =
        status == 0 ? gNetDispatch.GetCryptoKeys() : 0;
    if (status == 0 && ! keys) {
        status    = -EFAULT;
        statusMsg = "no crypto keys";
    }
    CryptoKeys::KeyId keyId = 0;
    CryptoKeys::Key   key;
    uint32_t          keyValidForSec = 0;
    if (status == 0 && ! keys->GetCurrentKey(keyId, key, keyValidForSec)) {
        status    = -EAGAIN;
        statusMsg = "no valid key exists";
    }
    if (! OkHeader(this, os)) {
        return;
    }
    os <<
        "Issued-time: "    << issuedTime   << "\r\n"
        "Valid-for-time: " << validForTime << "\r\n";
    if (keyValidForSec < validForTime) {
        os << "Token-valid-for-time: " << keyValidForSec << "\r\n";
    }
    os << "Access: ";
    DelegationToken::WriteTokenAndSessionKey(
        os,
        renewTokenStr.empty() ? authUid : renewToken.GetUid(),
        tokenSeq,
        keyId,
        issuedTime,
        delegationFlags,
        validForTime,
        key.GetPtr(),
        key.GetSize()
    );
    os <<
        "\r\n"
        "\r\n"
    ;
}

/*!
 * \brief Generate request (a string) that should be sent to the chunk
 * server.  The generated request string is based on the KFS
 * protocol.  All follow the same model:
 * @param[out] os: A string stream that contains the response.
 */
void
MetaChunkAllocate::request(ostream &os)
{
    assert(req && ! req->servers.empty());

    os << "ALLOCATE \r\n"
        "Cseq: "          << opSeqno           << "\r\n"
        "Version: KFS/1.0\r\n"
        "File-handle: "   << req->fid          << "\r\n"
        "Chunk-handle: "  << req->chunkId      << "\r\n"
        "Chunk-version: " << req->chunkVersion << "\r\n"
        "Min-tier: "      << (int)minSTier     << "\r\n"
        "Max-tier: "      << (int)maxSTier     << "\r\n"
        "STRIPE_IDENTIFIER: "      << req->stripe_identifier     << "\r\n"    //subrata: adding beacuse meta server will senf this request to the chunkl server
    ;
    KFS_LOG_STREAM_ERROR << "subrata : MetaChunkAllocate for stripe_identifier = " << req->stripe_identifier << KFS_LOG_EOM;
    if (0 <= leaseId) {
        os << "Lease-id: " << leaseId << "\r\n";
        if (req->clientCSAllowClearTextFlag) {
            os << "CS-clear-text: 1\r\n";
        }
    }
    os <<
        "Chunk-append: " << (req->appendChunk ? 1 : 0) << "\r\n"
        "Num-servers: "  << req->servers.size()        << "\r\n"
        "Servers:"
    ;
    for_each(req->servers.begin(), req->servers.end(),
            PrintChunkServerLocations(os));
    const size_t cAccessLen = chunkAccessStr.size();
    const size_t len        = cAccessLen + chunkServerAccessStr.size();
    if (len <= 0) {
        os << "\r\n\r\n";
        return;
    }
    os <<
        "\r\n"
        "Content-length: " << len << "\r\n";
    if (cAccessLen < len) {
        os << "C-access-length: " << cAccessLen << "\r\n";
        if (0 < req->validForTime) {
            os <<
                "CS-acess-issued: " << req->issuedTime   << "\r\n"
                "CS-acess-time: "   << req->validForTime << "\r\n";
        }
    }
    os << "\r\n";
    os.write(chunkAccessStr.data(),       cAccessLen);
    os.write(chunkServerAccessStr.data(), chunkServerAccessStr.size());
}

void
MetaChunkDelete::request(ostream &os)
{
    os << "DELETE \r\n";
    os << "Cseq: " << opSeqno << "\r\n";
    os << "Version: KFS/1.0\r\n";
    os << "Chunk-handle: " << chunkId << "\r\n\r\n";
}

void
MetaChunkHeartbeat::request(ostream &os)
{
    //subrata add
    //just for testing. Do not know where to add for printing ..
    //gLayoutManager.print_stripeIdentifierToChunkIDMap();

    //subrata end
    os <<
    "HEARTBEAT \r\n"
    "Cseq: " << opSeqno << "\r\n"
    "Version: KFS/1.0\r\n"
    "Num-evacuate: " << evacuateCount << "\r\n"
    ;
    if (reAuthenticateFlag) {
        os << "Authenticate: 1\r\n";
    }
    os <<
    "\r\n"
    ;
}

static inline char*
ChunkIdToString(chunkId_t id, bool hexFormatFlag, char* end)
{
    return (hexFormatFlag ? IntToHexString(id, end) : IntToDecString(id, end));
}

void
MetaChunkStaleNotify::request(ostream& os, IOBuffer& buf)
{
    const size_t count = staleChunkIds.GetSize();
    os <<
        "STALE_CHUNKS \r\n"
        "Cseq: " << opSeqno << "\r\n"
        "Version: KFS/1.0\r\n"
        "Num-chunks: " << count << "\r\n"
    ;
    if (evacuatedFlag) {
        os << "Evacuated: 1\r\n";
    }
    if (hexFormatFlag) {
        os << "HexFormat: 1\r\n";
    }
    if (hasAvailChunksSeqFlag) {
        os << "AvailChunksSeq: " << availChunksSeq << "\r\n";
    }
    const int   kBufEnd = 30;
    char        tmpBuf[kBufEnd + 1];
    char* const end = tmpBuf + kBufEnd;
    if (count <= 1) {
        char* const p   = count < 1 ? end :
            ChunkIdToString(staleChunkIds.Front(), hexFormatFlag, end);
        size_t      len = end - p;
        os << "Content-length: " << len << "\r\n\r\n";
        os.write(p, len);
        return;
    }

    ChunkIdQueue::ConstIterator it(staleChunkIds);
    const chunkId_t*            id;
    IOBuffer                    ioBuf;
    IOBufferWriter              writer(ioBuf);
    tmpBuf[kBufEnd] = (char)' ';
    while ((id = it.Next())) {
        char* const p = ChunkIdToString(*id, hexFormatFlag, end);
        writer.Write(p, (int)(end - p + 1));
    }
    writer.Close();
    const int len = ioBuf.BytesConsumable();
    os << "Content-length: " << len << "\r\n\r\n";
    IOBuffer::iterator const bi = ioBuf.begin();
    const int defsz = IOBufferData::GetDefaultBufferSize();
    if (len < defsz - defsz / 4 &&
            bi != ioBuf.end() && len == bi->BytesConsumable()) {
        os.write(bi->Consumer(), len);
    } else {
        os.flush();
        buf.Move(&ioBuf);
    }
}

void
MetaChunkRetire::request(ostream &os)
{
    os << "RETIRE \r\n";
    os << "Cseq: " << opSeqno << "\r\n";
    os << "Version: KFS/1.0\r\n\r\n";
}

void
MetaChunkVersChange::request(ostream &os)
{
    os <<
    "CHUNK_VERS_CHANGE \r\n"
    "Cseq: "               << opSeqno      << "\r\n"
    "Version: KFS/1.0\r\n"
    "File-handle: "        << fid          << "\r\n"
    "Chunk-handle: "       << chunkId      << "\r\n"
    "From-chunk-version: " << fromVersion  << "\r\n"
    "Chunk-version: "      << chunkVersion << "\r\n"
    ;
    if (makeStableFlag) {
        os << "Make-stable: 1\r\n";
    }
    os << "\r\n";
}

void
MetaBeginMakeChunkStable::request(ostream &os)
{
    os << "BEGIN_MAKE_CHUNK_STABLE\r\n"
        "Cseq: "          << opSeqno      << "\r\n"
        "Version: KFS/1.0\r\n"
        "File-handle: "   << fid          << "\r\n"
        "Chunk-handle: "  << chunkId      << "\r\n"
        "Chunk-version: " << chunkVersion << "\r\n"
    "\r\n";
}

void
MetaChunkMakeStable::request(ostream &os)
{
    os << "MAKE_CHUNK_STABLE \r\n";
    os << "Cseq: " << opSeqno << "\r\n";
    os << "Version: KFS/1.0\r\n";
    os << "File-handle: " << fid << "\r\n";
    os << "Chunk-handle: " << chunkId << "\r\n";
    os << "Chunk-version: " << chunkVersion << "\r\n";
    os << "Chunk-size: " << chunkSize << "\r\n";
    if (hasChunkChecksum) {
        os << "Chunk-checksum: " << chunkChecksum << "\r\n";
    }
    os << "\r\n";
}

static const string sReplicateCmdName("REPLICATE");

void
MetaChunkReplicate::request(ostream& os)
{
    //subrata: this function creates a protocol message on metaserver and sends to chunk servers over a network socket....

    // OK to use global here as chunk server state machine runs in the main
    // thread.
    ostringstream& rs = GetTmpOStringStream();
    rs <<
    "Cseq: "          << opSeqno      << "\r\n"
    "Version: KFS/1.0\r\n"
    "File-handle: "   << fid          << "\r\n"
    "Chunk-handle: "  << chunkId      << "\r\n"
    "Min-tier: "      << (int)minSTier << "\r\n"
    "Max-tier: "      << (int)maxSTier << "\r\n"
    ;
    if (numRecoveryStripes > 0) {
        rs <<
        "Chunk-version: 0\r\n"
        "Chunk-offset: "         << chunkOffset          << "\r\n"
        "Striper-type: "         << striperType          << "\r\n"
        "Num-stripes: "          << numStripes           << "\r\n"
        "Num-recovery-stripes: " << numRecoveryStripes   << "\r\n"
        "Stripe-size: "          << stripeSize           << "\r\n"
        "Meta-port: "            << srcLocation.port     << "\r\n"
        "Target-version: "       << chunkVersion         << "\r\n"
        ;
        if (fileSize > 0) {
            rs << "File-size: " << fileSize << "\r\n";
        }
    } else {
        //subrata: Note: For RS (Reed-Solomon) source location is not specified
        rs << "Chunk-location: " << srcLocation << "\r\n";
    }
    if (0 < validForTime) {
        if (clientCSAllowClearTextFlag) {
            rs << "CS-clear-text: 1\r\n";
        }
        if (dataServer) {
            rs << "CS-access: ";
            DelegationToken::WriteTokenAndSessionKey(
                rs,
                authUid,
                tokenSeq,
                keyId,
                issuedTime,
                DelegationToken::kChunkServerFlag,
                validForTime,
                key.GetPtr(),
                key.GetSize()
            );
            rs << "\r\n"
                "C-access: ";
            ChunkAccessToken::WriteToken(
                rs,
                chunkId,
                authUid,
                tokenSeq,
                keyId,
                issuedTime,
                ChunkAccessToken::kAllowReadFlag |
                    DelegationToken::kChunkServerFlag |
                    (clientCSAllowClearTextFlag ?
                        ChunkAccessToken::kAllowClearTextFlag : 0),
                LEASE_INTERVAL_SECS * 2,
                key.GetPtr(),
                key.GetSize()
            );
        } else {
            rs << "CS-access: ";
            if (metaServerAccess.empty()) {
                DelegationToken::WriteTokenAndSessionKey(
                    rs,
                    authUid,
                    tokenSeq,
                    keyId,
                    issuedTime,
                    DelegationToken::kChunkServerFlag,
                    validForTime,
                    key.GetPtr(),
                    key.GetSize()
                );
            } else {
                rs.write(metaServerAccess.data(), metaServerAccess.size());
            }
        }
        rs << "\r\n";
    }
    rs << "\r\n";
    const string req = rs.str();
    //subrata : the replication command is "REPLICATE"
    //KFS_LOG_STREAM_DEBUG << "subrata: metaserver is about to send replication instruiction to chunkserver from meta/MetaRequest.cc line 5126" << KFS_LOG_EOM; 
    os << sReplicateCmdName << " " << Checksum(
        sReplicateCmdName.data(),
        sReplicateCmdName.size(),
        req.data(),
        req.size()) <<
    "\r\n";
    os.write(req.data(), req.size());
    
   requestTime = microseconds(); //current time in microsec . record this. we need to measure how long does the repair take
   KFS_LOG_STREAM_DEBUG << "subrata: metaserver is sending REPAIR instruiction to chunkserver at time = " << requestTime << KFS_LOG_EOM; 
}

void
MetaChunkReplicate::handleReply(const Properties& prop)
{
    if (status == 0) {
        const seq_t cVers = prop.getValue("Chunk-version", seq_t(0));
        if (numRecoveryStripes <= 0) {
            chunkVersion = cVers;
        } else if (cVers != 0) {
            status    = -EINVAL;
            statusMsg = "invalid chunk version in reply";
            return;
        }
    }
    fid = prop.getValue("File-handle", fid_t(0));

    int64_t now = microseconds(); //current time in microsec . 
    //lets check how long did it take to complete the repair process..
    int64_t repairDuration = now - this->requestTime;

    //KFS_LOG_STREAM_DEBUG << "subrata: MetaChunkReplicate::handleReply for fid= " << fid << " REPAIR took time = " << repairDuration << " request time = " << this->requestTime << " reply time = " << now << KFS_LOG_EOM;
    KFS_LOG_STREAM_DEBUG << "subrata: MetaChunkReplicate::handleReply for fid= " << fid << " for chunkId = " << chunkId <<" REPAIR took time = " << repairDuration << " request time = " << this->requestTime << "reply time = " << now << KFS_LOG_EOM;


    //subrata : clean up the repair info
    std::map<chunkId_t, RepairServerInfo*> :: iterator repairInfoPos = gLayoutManager.ChunkReapirServersBeingUsed.find(chunkId);
     assert(repairInfoPos != gLayoutManager.ChunkReapirServersBeingUsed.end());
    std::map<ChunkServerPtr, bool> :: iterator begIter = (repairInfoPos->second->RepairServersMap).begin();
    std::map<ChunkServerPtr, bool> :: iterator endIter = (repairInfoPos->second->RepairServersMap).end();
    for(;begIter != endIter; begIter++)
    {
         std::map<ChunkServerPtr, bool> :: iterator  serverPos = gLayoutManager.ServersBeingUsedMap.find(begIter->first);
	 assert(serverPos != gLayoutManager.ServersBeingUsedMap.end());
	 gLayoutManager.ServersBeingUsedMap.erase(serverPos);
    }
    delete (repairInfoPos->second);
    gLayoutManager.ChunkReapirServersBeingUsed.erase(repairInfoPos);



    invalidStripes.clear();
    const int sc = numStripes + numRecoveryStripes;
    if (status == 0 || sc <= 0) {
        return;
    }
    const string idxStr(prop.getValue("Invalid-stripes", string()));
    if (idxStr.empty()) {
        return;
    }
    istringstream is(idxStr);
    is >> std::ws;
    while (! is.eof()) {
        int       idx       = -1;
        chunkId_t chunkId   = -1;
        seq_t     chunkVers = -1;
        if (! (is >> idx >> chunkId >> chunkVers >> std::ws) ||
                idx < 0 || sc <= idx ||
                (int)invalidStripes.size() >= sc - 1) {
            KFS_LOG_STREAM_ERROR << "replicate reply: parse error:"
                " pos: " << invalidStripes.size() <<
                " Invalid-stripes: " << idxStr <<
            KFS_LOG_EOM;
            invalidStripes.clear();
            break;
        }
        invalidStripes.insert(
            make_pair(idx, make_pair(chunkId, chunkVers)));
    }
}

//subrata add

static const string sReplicateDistributedCmdName("REPAIRDISTRIBUTED");
/* virtual */ 
ostream& MetaDistributedRepairChunk::ShowSelf(ostream& os) const
{
     os << "MetaDistributedRepairChunk";
     return os;
}
void
MetaDistributedRepairChunk::handle()
{
    //KFS_LOG_STREAM_DEBUG << " subrata : MetaDistributedRepairChunk::handle was called" << KFS_LOG_EOM;
}

void
MetaDistributedRepairChunk::request(ostream& os)
{
    ostringstream& rs = GetTmpOStringStream();
    rs <<
    "Cseq: "          << opSeqno      << "\r\n"
    "Version: KFS/1.0\r\n"
    "File-handle: "   << fid          << "\r\n"
    "Chunk-handle: "  << chunkId      << "\r\n"
    "Decoding-coefficient: "  << decodeCoeff    << "\r\n"
    "STRIPE-IDENTIFIER: "  << stripe_identifier    << "\r\n"
    "Operation-sequence-list: "  << operation_sequence_list << "\r\n"

    "Min-tier: "      << (int)minSTier << "\r\n"
    "Max-tier: "      << (int)maxSTier << "\r\n"
    ;
    if (numRecoveryStripes > 0) {
        rs <<
        "Chunk-version: 0\r\n"
        "Chunk-offset: "         << chunkOffset          << "\r\n"
        "Striper-type: "         << striperType          << "\r\n"
        "Num-stripes: "          << numStripes           << "\r\n"
        "Num-recovery-stripes: " << numRecoveryStripes   << "\r\n"
        "Stripe-size: "          << stripeSize           << "\r\n"
        "Meta-port: "            << srcLocation.port     << "\r\n"
        "Target-version: "       << chunkVersion         << "\r\n"
        ;
        if (fileSize > 0) {
            rs << "File-size: " << fileSize << "\r\n";
        }
    } else {
        //subrata: Note: For RS (Reed-Solomon) source location is not specified
        rs << "Chunk-location: " << srcLocation << "\r\n";
    }
    if (0 < validForTime) {
        if (clientCSAllowClearTextFlag) {
            rs << "CS-clear-text: 1\r\n";
        }
        if (dataServer) {
            rs << "CS-access: ";
            DelegationToken::WriteTokenAndSessionKey(
                rs,
                authUid,
                tokenSeq,
                keyId,
                issuedTime,
                DelegationToken::kChunkServerFlag,
                validForTime,
                key.GetPtr(),
                key.GetSize()
            );
            rs << "\r\n"
                "C-access: ";
        ChunkAccessToken::WriteToken(
                rs,
                chunkId,
                authUid,
                tokenSeq,
                keyId,
                issuedTime,
                ChunkAccessToken::kAllowReadFlag |
                    DelegationToken::kChunkServerFlag |
                    (clientCSAllowClearTextFlag ?
                        ChunkAccessToken::kAllowClearTextFlag : 0),
                LEASE_INTERVAL_SECS * 2,
                key.GetPtr(),
                key.GetSize()
            );
        } else {
            rs << "CS-access: ";
            if (metaServerAccess.empty()) {
                DelegationToken::WriteTokenAndSessionKey(
                    rs,
                    authUid,
                    tokenSeq,
                    keyId,
                    issuedTime,
                    DelegationToken::kChunkServerFlag,
                    validForTime,
                    key.GetPtr(),
                    key.GetSize()
                );
            } else {
                rs.write(metaServerAccess.data(), metaServerAccess.size());
            }
        }
        rs << "\r\n";
    }
    rs << "\r\n";
    const string req = rs.str();
    //subrata : the replication command is "REPLICATEDISTRIBUTE"
    //KFS_LOG_STREAM_DEBUG << "subrata: metaserver is distributing the coeffcients and task-list for distributed/partial repair" << KFS_LOG_EOM;
    os << sReplicateDistributedCmdName << " " << Checksum(
        sReplicateDistributedCmdName.data(),
        sReplicateDistributedCmdName.size(),
        req.data(),
        req.size()) <<
    "\r\n";
    os.write(req.data(), req.size());

   requestTime = microseconds(); //current time in microsec . record this. we need to measure how long does the repair take
   KFS_LOG_STREAM_DEBUG << "subrata: MetaDistributedRepairChunk (" << this << ") : metaserver is sending REPAIR instruiction to chunkserver for stripe_identifier = "  << stripe_identifier << " at time = " << requestTime << KFS_LOG_EOM; 

}
void
MetaDistributedRepairChunk::handleReply(const Properties& prop)
{
    const string receivedFileId(prop.getValue("File-handle", string()));
    const long receivedStripeIdentifier(prop.getValue("STRIPE-IDENTIFIER", long(0)));
    const chunkId_t receivedChunkId(prop.getValue("Chunk-handle", 0));
    const seq_t receivedChunkVersion(prop.getValue("Chunk-version", seq_t(0)));

    KFS_LOG_STREAM_DEBUG << "subrata: MetaDistributedRepairChunk::handleReply (" << this << ") for fid= " << fid << " for stripe_identifier = " << stripe_identifier << KFS_LOG_EOM;
    assert(receivedStripeIdentifier == this->stripe_identifier);
    assert(receivedChunkId == this->chunkId);

    int64_t now = microseconds(); //current time in microsec . 
    //lets check how long did it take to complete the repair process..
    int64_t repairDuration = now - this->requestTime;

    //KFS_LOG_STREAM_DEBUG << "subrata: MetaChunkReplicate::handleReply for fid= " << receivedFileId << " REPAIR took time = " << repairDuration << KFS_LOG_EOM;
    KFS_LOG_STREAM_DEBUG << "subrata: MetaDistributedRepairChunk::handleReply for fid= " << fid << " for stripe_identifier = " << stripe_identifier << " chunkId = " << chunkId <<" REPAIR took time = " << repairDuration << " request time = " << this->requestTime << "reply time = " << now << KFS_LOG_EOM;
}

//subrata end


void
MetaChunkSize::request(ostream &os)
{
    os <<
    "SIZE \r\n"
    "Cseq: "          << opSeqno      << "\r\n"
    "Version: KFS/1.0\r\n"
    "File-handle: "   << fid          << "\r\n"
    "Chunk-version: " << chunkVersion << "\r\n"
    "Chunk-handle: "  << chunkId      << "\r\n"
    "\r\n";
}

void
MetaChunkSetProperties::request(ostream &os)
{
    os <<
    "CMD_SET_PROPERTIES\r\n"
    "Cseq: " << opSeqno << "\r\n"
    "Version: KFS/1.0\r\n"
    "Content-length: " << serverProps.length() << "\r\n\r\n" <<
    serverProps
    ;
}

void
MetaChunkServerRestart::request(ostream &os)
{
    os <<
    "RESTART_CHUNK_SERVER\r\n"
    "Cseq: " << opSeqno << "\r\n"
    "Version: KFS/1.0\r\n"
    "\r\n"
    ;
}

int
MetaAuthenticate::Read(IOBuffer& iobuf)
{
    int rem = contentLength - contentBufPos;
    if (contentBuf) {
        rem = iobuf.CopyOut(contentBuf + contentBufPos, rem);
    }
    contentBufPos += iobuf.Consume(rem);
    return (contentLength - contentBufPos);
}

/* virtual */ bool
MetaDelegateCancel::dispatch(ClientSM& sm)
{
    return sm.Handle(*this);
}

/* virtual */ void
MetaDelegateCancel::handle()
{
    if (status == 0) {
        writeLogFlag = gNetDispatch.CancelToken(token);
    }
}

/* virtual */ int
MetaDelegateCancel::log(ostream& file) const
{
    if (status == 0 && writeLogFlag) {
        file <<
            "delegatecancel"
            "/exp/"    << (token.GetIssuedTime() + token.GetValidForSec()) <<
            "/issued/" << token.GetIssuedTime() <<
            "/uid/"    << token.GetUid()        <<
            "/seq/"    << token.GetSeq()        <<
            "/flags/"  << token.GetFlags()      <<
        "\n";
        return file.fail() ? -EIO : 0;
    }
    return 0;
}

bool
MetaDelegateCancel::Validate()
{
    const CryptoKeys* const keys = gNetDispatch.GetCryptoKeys();
    if (! keys) {
        status    = -EINVAL;
        statusMsg = "no crypto keys";
        return true;
    }
    CryptoKeys::Key key;
    if (! key.Parse(tokenKeyStr.GetPtr(), tokenKeyStr.GetSize())) {
        status    = -EINVAL;
        statusMsg = "invalid key format";
        return true;
    }
    char keyBuf[CryptoKeys::Key::kLength];
    const int len = token.Process(
        tokenStr.GetPtr(),
        (int)tokenStr.GetSize(),
        time(0),
        *keys,
        keyBuf,
        CryptoKeys::Key::kLength,
        &statusMsg
    );
    if (len < 0) {
        status = len;
        return true;
    }
    if (len != key.GetSize() || memcmp(key.GetPtr(), keyBuf, len) != 0) {
        status    = -EINVAL;
        statusMsg = "invalid key";
    }
    return true;
}

void
MetaDelegateCancel::response(ostream& os)
{
    PutHeader(this, os) << "\r\n";
}

void
MetaForceChunkReplication::handle()
{
    if (! HasMetaServerAdminAccess(*this)) {
        return;
    }
    gLayoutManager.Handle(*this);
}

void
MetaForceChunkReplication::response(ostream& os)
{
    PutHeader(this, os) << "\r\n";
}

} /* namespace KFS */
