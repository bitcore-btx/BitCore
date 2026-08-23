// Copyright (c) 2014-2017 The Dash Core developers
// Copyright (c) 2019 Limxtec developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCORE_MASTERNODE_PAYMENTS_H
#define BITCORE_MASTERNODE_PAYMENTS_H

#include <util.h>
#include <core_io.h>
#include <key.h>
#include <masternode.h>
#include <net_processing.h>
#include <utilstrencodings.h>

class CMasternodePayments;
class CMasternodePaymentVote;
class CMasternodeBlockPayees;

static const int MNPAYMENTS_SIGNATURES_REQUIRED         = 6;
static const int MNPAYMENTS_SIGNATURES_TOTAL            = 10;
static const int DEFAULTMINPROTO                        = 79999;
static const int FACTOR_ENFORCEMENT                     = 2;

//! minimum peer version that can receive and send masternode payment messages,
//  vote for masternode and be elected as a payment winner
// V1 - Last protocol version before update
// V2 - Newest protocol version

extern CCriticalSection cs_vecPayees;
extern CCriticalSection cs_mapMasternodeBlocks;
extern CCriticalSection cs_mapMasternodePayeeVotes;

extern CMasternodePayments mnpayments;

/// TODO: all 4 functions do not belong here really, they should be refactored/moved somewhere (main.cpp ?)
bool IsBlockValueValid(const CBlock& block, int nBlockHeight, CAmount blockReward, std::string &strErrorRet);
bool IsBlockPayeeValid(const CTransactionRef txNew, int nBlockHeight, CAmount blockReward, CBlockHeader pblock);
void FillBlockPayments(CMutableTransaction& txNew, int nBlockHeight, CAmount blockReward, CTxOut& txoutMasternodeRet, std::vector<CTxOut>& voutSuperblockRet);
std::string GetRequiredPaymentsString(int nBlockHeight);

class CMasternodePayee
{
private:
    CScript scriptPubKey;
    std::vector<uint256> vecVoteHashes;

public:
    CMasternodePayee() :
        scriptPubKey(),
        vecVoteHashes()
        {}

    CMasternodePayee(CScript payee, uint256 hashIn) :
        scriptPubKey(payee),
        vecVoteHashes()
    {
        vecVoteHashes.push_back(hashIn);
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(*(CScriptBase*)(&scriptPubKey));
        READWRITE(vecVoteHashes);
    }

    CScript GetPayee() { return scriptPubKey; }

    void AddVoteHash(uint256 hashIn) { vecVoteHashes.push_back(hashIn); }
    std::vector<uint256> GetVoteHashes() { return vecVoteHashes; }
    int GetVoteCount() { return vecVoteHashes.size(); }
};

// Keep track of votes for payees from masternodes
class CMasternodeBlockPayees
{
public:
    int nBlockHeight;
    std::vector<CMasternodePayee> vecPayees;

    CMasternodeBlockPayees() :
        nBlockHeight(0),
        vecPayees()
        {}
    CMasternodeBlockPayees(int nBlockHeightIn) :
        nBlockHeight(nBlockHeightIn),
        vecPayees()
        {}

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(nBlockHeight);
        READWRITE(vecPayees);
    }

    void AddPayee(const CMasternodePaymentVote& vote);
    bool GetBestPayee(CScript& payeeRet);
    bool HasPayeeWithVotes(const CScript& payeeIn, int nVotesReq);

    bool IsTransactionValid(const CTransactionRef txNew);

    std::string GetRequiredPaymentsString();
};

// vote for the winning payment
class CMasternodePaymentVote
{
public:
    CTxIn vinMasternode;

    int nBlockHeight;
    CScript payee;
    std::vector<unsigned char> vchSig;

    CMasternodePaymentVote() :
        vinMasternode(),
        nBlockHeight(0),
        payee(),
        vchSig()
        {}

    CMasternodePaymentVote(COutPoint outpointMasternode, int nBlockHeight, CScript payee) :
        vinMasternode(outpointMasternode),
        nBlockHeight(nBlockHeight),
        payee(payee),
        vchSig()
        {}

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(vinMasternode);
        READWRITE(nBlockHeight);
        READWRITE(*(CScriptBase*)(&payee));
        READWRITE(vchSig);
    }

    uint256 GetHash() const {
        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        ss << *(CScriptBase*)(&payee);
        ss << nBlockHeight;
        ss << vinMasternode.prevout;
        return ss.GetHash();
    }

    bool Sign();
    bool CheckSignature(const CPubKey& pubKeyMasternode, int nValidationHeight, int &nDos);

    bool IsValid(CNode* pnode, int nValidationHeight, std::string& strError, CConnman& connman);
    void Relay(CConnman& connman);

    bool IsVerified() { return !vchSig.empty(); }
    void MarkAsNotVerified() { vchSig.clear(); }

    std::string ToString() const;
};

//
// Masternode Payments Class
// Keeps track of who should get paid for which blocks
//

class CMasternodePayments
{
private:
    // masternode count times nStorageCoeff payments blocks should be stored ...
    const float nStorageCoeff;
    // ... but at least nMinBlocksToStore (payments blocks)
    const int nMinBlocksToStore;

    // Keep track of current block height
    int nCachedBlockHeight;

public:
    std::map<uint256, CMasternodePaymentVote> mapMasternodePaymentVotes;
    std::map<int, CMasternodeBlockPayees> mapMasternodeBlocks;
    std::map<COutPoint, int> mapMasternodesLastVote;
    std::map<COutPoint, int> mapMasternodesDidNotVote;

    CMasternodePayments() : nStorageCoeff(1.25), nMinBlocksToStore(5000) {}

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(mapMasternodePaymentVotes);
        READWRITE(mapMasternodeBlocks);
    }

    void Clear();

    bool AddPaymentVote(const CMasternodePaymentVote& vote);
    bool HasVerifiedPaymentVote(uint256 hashIn);
    bool ProcessBlock(int nBlockHeight, CConnman& connman);
    void CheckPreviousBlockVotes(int nPrevBlockHeight);

    void Sync(CNode* node, CConnman& connman);
    void RequestLowDataPaymentBlocks(CNode* pnode, CConnman& connman);
    void CheckAndRemove();

    bool GetBlockPayee(int nBlockHeight, CScript& payee);
    bool IsTransactionValid(const CTransactionRef txNew, int nBlockHeight);
    bool IsScheduled(CMasternode& mn, int nNotBlockHeight);

    // Second (rank-queue) masternode payment system, see SPORK_BTX_22_MASTERNODE_RANK_PAYMENT_SYSTEM.
    // Additive to the vote-based system above; only used while that spork is active.
    void FillBlockPayee_2(CMutableTransaction& txNew, int nBlockHeight, CAmount blockReward, CTxOut& txoutMasternodeRet);
    bool IsTransactionValid_2(const CTransactionRef txNew, int nBlockHeight, CAmount blockReward);

    bool CanVote(COutPoint outMasternode, int nBlockHeight);

    int GetMinMasternodePaymentsProto();
    int GetFactorEnforcement();
    void ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv, CConnman& connman);
    std::string GetRequiredPaymentsString(int nBlockHeight);
    void FillBlockPayee(CMutableTransaction& txNew, int nBlockHeight, CAmount blockReward, CTxOut& txoutMasternodeRet);
    std::string ToString() const;

    int GetBlockCount() { return mapMasternodeBlocks.size(); }
    int GetVoteCount() { return mapMasternodePaymentVotes.size(); }

    bool IsEnoughData();
    int GetStorageLimit();

    void UpdatedBlockTip(const CBlockIndex *pindex, CConnman& connman);
};

// Reorg-safe "who got paid at which height" history for the second (rank-queue)
// masternode payment system, see SPORK_BTX_22_MASTERNODE_RANK_PAYMENT_SYSTEM.
// Only populated while that spork is active (ConnectBlock/DisconnectBlock hooks
// in validation.cpp are themselves gated on the spork). Kept small on purpose --
// pruned to the last MNRANKPAYMENTS_UNDO_DEPTH blocks, far beyond any realistic reorg.
static const int MNRANKPAYMENTS_UNDO_DEPTH = 2000;

class CMasternodeRankPayments
{
public:
    // height -> outpoint paid at that height under the rank system
    std::map<int, COutPoint> mapRankBlockPayee;
    mutable CCriticalSection cs_rankpayments;

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        LOCK(cs_rankpayments);
        READWRITE(mapRankBlockPayee);
    }

    // Record that `outpoint` was paid at height `nHeight`, and update its
    // masternode's nBlockLastPaid2 accordingly (if still known).
    void RecordPayment(int nHeight, const COutPoint& outpoint);
    // Undo the payment recorded at height `nHeight`: restore the affected
    // masternode's nBlockLastPaid2 to its previous value (0 if none).
    void UndoPayment(int nHeight);
    // Most recent recorded payment height for `outpoint` still within the
    // (pruned) history, or 0 if none is present. Used to restore a
    // re-registering masternode's place in the rank-queue FIFO instead of
    // treating it as never-paid.
    int GetLastPaidHeight(const COutPoint& outpoint) const;
    // Drop history entries older than nTipHeight - MNRANKPAYMENTS_UNDO_DEPTH.
    void Prune(int nTipHeight);
    void Clear() { LOCK(cs_rankpayments); mapRankBlockPayee.clear(); }
    void CheckAndRemove();
    std::string ToString() const;
};

extern CMasternodeRankPayments mnRankPayments;

#endif // BITCORE_MASTERNODE-PAYMENTS_H
