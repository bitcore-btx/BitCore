// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Copyright (c) 2014-2017 The Dash Core developers
// Copyright (c) 2019 Limxtec developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <validationinterface.h>

#include <primitives/block.h>
#include <scheduler.h>
#include <sync.h>
#include <txmempool.h>
#include <util.h>
#include <validation.h>

#include <list>
#include <map>
#include <vector>
#include <atomic>
#include <future>

#include <boost/signals2/signal.hpp>
// Boost >= 1.73 no longer injects _1/_2/... into the global namespace by
// default; pull in the namespaced placeholders explicitly instead.
#include <boost/bind/placeholders.hpp>
using namespace boost::placeholders;

struct MainSignalsInstance {
    // Dash
    boost::signals2::signal<void (const CBlockIndex *)> AcceptedBlockHeader;
    boost::signals2::signal<void (const CBlockIndex *, bool fInitialDownload)> NotifyHeaderTip;
    //

    boost::signals2::signal<void (const CBlockIndex *, const CBlockIndex *, bool fInitialDownload)> UpdatedBlockTip;

    // Dash
    boost::signals2::signal<void (const CTransaction &, const CBlock *)> SyncTransaction;
    //

    boost::signals2::signal<void (const CTransactionRef &)> TransactionAddedToMempool;
    boost::signals2::signal<void (const std::shared_ptr<const CBlock> &, const CBlockIndex *pindex, const std::vector<CTransactionRef>&)> BlockConnected;
    boost::signals2::signal<void (const std::shared_ptr<const CBlock> &)> BlockDisconnected;
    boost::signals2::signal<void (const CTransactionRef &)> TransactionRemovedFromMempool;

    // Dash
    boost::signals2::signal<void (const CTransaction &)> NotifyTransactionLock;
    //

    boost::signals2::signal<void (const CBlockLocator &)> ChainStateFlushed;
    boost::signals2::signal<void (int64_t nBestBlockTime, CConnman* connman)> Broadcast;
    boost::signals2::signal<void (const CBlock&, const CValidationState&)> BlockChecked;
    boost::signals2::signal<void (const CBlockIndex *, const std::shared_ptr<const CBlock>&)> NewPoWValidBlock;

    // We are not allowed to assume the scheduler only runs in one thread,
    // but must ensure all callbacks happen in-order, so we end up creating
    // our own queue here :(
    SingleThreadedSchedulerClient m_schedulerClient;

    // Boost >= 1.74 changed boost::function's internal storage for
    // boost::bind(...)-built slots, which breaks signals2's disconnect-by-value
    // (it needs to compare the stored slot against a freshly built one). Track
    // the connection handles returned by connect() instead and disconnect
    // through those, avoiding the comparison entirely.
    std::map<CValidationInterface*, std::vector<boost::signals2::connection>> m_connMainSignals;
    boost::signals2::connection m_connMempoolNotifyEntryRemoved;

    explicit MainSignalsInstance(CScheduler *pscheduler) : m_schedulerClient(pscheduler) {}
};

static CMainSignals g_signals;

void CMainSignals::RegisterBackgroundSignalScheduler(CScheduler& scheduler) {
    assert(!m_internals);
    m_internals.reset(new MainSignalsInstance(&scheduler));
}

void CMainSignals::UnregisterBackgroundSignalScheduler() {
    m_internals.reset(nullptr);
}

void CMainSignals::FlushBackgroundCallbacks() {
    if (m_internals) {
        m_internals->m_schedulerClient.EmptyQueue();
    }
}

size_t CMainSignals::CallbacksPending() {
    if (!m_internals) return 0;
    return m_internals->m_schedulerClient.CallbacksPending();
}

void CMainSignals::RegisterWithMempoolSignals(CTxMemPool& pool) {
    m_internals->m_connMempoolNotifyEntryRemoved = pool.NotifyEntryRemoved.connect(boost::bind(&CMainSignals::MempoolEntryRemoved, this, _1, _2));
}

void CMainSignals::UnregisterWithMempoolSignals(CTxMemPool& pool) {
    m_internals->m_connMempoolNotifyEntryRemoved.disconnect();
}

CMainSignals& GetMainSignals()
{
    return g_signals;
}

void RegisterValidationInterface(CValidationInterface* pwalletIn) {
    std::vector<boost::signals2::connection>& conns = g_signals.m_internals->m_connMainSignals[pwalletIn];

    // Dash
    conns.push_back(g_signals.m_internals->AcceptedBlockHeader.connect(boost::bind(&CValidationInterface::AcceptedBlockHeader, pwalletIn, _1)));
    conns.push_back(g_signals.m_internals->NotifyHeaderTip.connect(boost::bind(&CValidationInterface::NotifyHeaderTip, pwalletIn, _1, _2)));
    //

    conns.push_back(g_signals.m_internals->UpdatedBlockTip.connect(boost::bind(&CValidationInterface::UpdatedBlockTip, pwalletIn, _1, _2, _3)));

    // Dash
    conns.push_back(g_signals.m_internals->SyncTransaction.connect(boost::bind(&CValidationInterface::SyncTransaction, pwalletIn, _1, _2)));
    //

    conns.push_back(g_signals.m_internals->TransactionAddedToMempool.connect(boost::bind(&CValidationInterface::TransactionAddedToMempool, pwalletIn, _1)));
    conns.push_back(g_signals.m_internals->BlockConnected.connect(boost::bind(&CValidationInterface::BlockConnected, pwalletIn, _1, _2, _3)));
    conns.push_back(g_signals.m_internals->BlockDisconnected.connect(boost::bind(&CValidationInterface::BlockDisconnected, pwalletIn, _1)));
    conns.push_back(g_signals.m_internals->TransactionRemovedFromMempool.connect(boost::bind(&CValidationInterface::TransactionRemovedFromMempool, pwalletIn, _1)));

    // Dash
    conns.push_back(g_signals.m_internals->NotifyTransactionLock.connect(boost::bind(&CValidationInterface::NotifyTransactionLock, pwalletIn, _1)));
    //

    conns.push_back(g_signals.m_internals->ChainStateFlushed.connect(boost::bind(&CValidationInterface::ChainStateFlushed, pwalletIn, _1)));
    conns.push_back(g_signals.m_internals->Broadcast.connect(boost::bind(&CValidationInterface::ResendWalletTransactions, pwalletIn, _1, _2)));
    conns.push_back(g_signals.m_internals->BlockChecked.connect(boost::bind(&CValidationInterface::BlockChecked, pwalletIn, _1, _2)));
    conns.push_back(g_signals.m_internals->NewPoWValidBlock.connect(boost::bind(&CValidationInterface::NewPoWValidBlock, pwalletIn, _1, _2)));
}

void UnregisterValidationInterface(CValidationInterface* pwalletIn) {
    auto it = g_signals.m_internals->m_connMainSignals.find(pwalletIn);
    if (it == g_signals.m_internals->m_connMainSignals.end()) {
        return;
    }
    for (boost::signals2::connection& conn : it->second) {
        conn.disconnect();
    }
    g_signals.m_internals->m_connMainSignals.erase(it);
}

void UnregisterAllValidationInterfaces() {
    if (!g_signals.m_internals) {
        return;
    }
    for (auto& item : g_signals.m_internals->m_connMainSignals) {
        for (boost::signals2::connection& conn : item.second) {
            conn.disconnect();
        }
    }
    g_signals.m_internals->m_connMainSignals.clear();
}

void CallFunctionInValidationInterfaceQueue(std::function<void ()> func) {
    g_signals.m_internals->m_schedulerClient.AddToProcessQueue(std::move(func));
}

void SyncWithValidationInterfaceQueue() {
    AssertLockNotHeld(cs_main);
    // Block until the validation queue drains
    std::promise<void> promise;
    CallFunctionInValidationInterfaceQueue([&promise] {
        promise.set_value();
    });
    promise.get_future().wait();
}

void CMainSignals::MempoolEntryRemoved(CTransactionRef ptx, MemPoolRemovalReason reason) {
    if (reason != MemPoolRemovalReason::BLOCK && reason != MemPoolRemovalReason::CONFLICT) {
        m_internals->m_schedulerClient.AddToProcessQueue([ptx, this] {
            m_internals->TransactionRemovedFromMempool(ptx);
        });
    }
}

// Dash
void CMainSignals::AcceptedBlockHeader(const CBlockIndex *pindexNew) {
    m_internals->m_schedulerClient.AddToProcessQueue([pindexNew, this] {
        m_internals->AcceptedBlockHeader(pindexNew);
    });
}

void CMainSignals::NotifyHeaderTip(const CBlockIndex *pindexNew, bool fInitialDownload) {
    m_internals->m_schedulerClient.AddToProcessQueue([pindexNew, fInitialDownload, this] {
        m_internals->NotifyHeaderTip(pindexNew, fInitialDownload);
    });
}
//

void CMainSignals::UpdatedBlockTip(const CBlockIndex *pindexNew, const CBlockIndex *pindexFork, bool fInitialDownload) {
    // Dependencies exist that require UpdatedBlockTip events to be delivered in the order in which
    // the chain actually updates. One way to ensure this is for the caller to invoke this signal
    // in the same critical section where the chain is updated

    m_internals->m_schedulerClient.AddToProcessQueue([pindexNew, pindexFork, fInitialDownload, this] {
        m_internals->UpdatedBlockTip(pindexNew, pindexFork, fInitialDownload);
    });
}

// Dash
void CMainSignals::SyncTransaction(const CTransaction &tx, const CBlock *pblock) {
    m_internals->m_schedulerClient.AddToProcessQueue([tx, pblock, this] {
        m_internals->SyncTransaction(tx, pblock);
    });
}
//

void CMainSignals::TransactionAddedToMempool(const CTransactionRef &ptx) {
    m_internals->m_schedulerClient.AddToProcessQueue([ptx, this] {
        m_internals->TransactionAddedToMempool(ptx);
    });
}

void CMainSignals::BlockConnected(const std::shared_ptr<const CBlock> &pblock, const CBlockIndex *pindex, const std::shared_ptr<const std::vector<CTransactionRef>>& pvtxConflicted) {
    m_internals->m_schedulerClient.AddToProcessQueue([pblock, pindex, pvtxConflicted, this] {
        m_internals->BlockConnected(pblock, pindex, *pvtxConflicted);
    });
}

void CMainSignals::BlockDisconnected(const std::shared_ptr<const CBlock> &pblock) {
    m_internals->m_schedulerClient.AddToProcessQueue([pblock, this] {
        m_internals->BlockDisconnected(pblock);
    });
}

// Dash
void CMainSignals::NotifyTransactionLock(const CTransaction &tx) {
    m_internals->m_schedulerClient.AddToProcessQueue([tx, this] {
        m_internals->NotifyTransactionLock(tx);
    });
}
//

void CMainSignals::ChainStateFlushed(const CBlockLocator &locator) {
    m_internals->m_schedulerClient.AddToProcessQueue([locator, this] {
        m_internals->ChainStateFlushed(locator);
    });
}

void CMainSignals::Broadcast(int64_t nBestBlockTime, CConnman* connman) {
    m_internals->Broadcast(nBestBlockTime, connman);
}

void CMainSignals::BlockChecked(const CBlock& block, const CValidationState& state) {
    m_internals->BlockChecked(block, state);
}

void CMainSignals::NewPoWValidBlock(const CBlockIndex *pindex, const std::shared_ptr<const CBlock> &block) {
    m_internals->NewPoWValidBlock(pindex, block);
}
