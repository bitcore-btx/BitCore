# Test Report: BitCore BTX v0.91.1.1 (branch `dev_8`, commit `80f27a6`)

**Date:** 2026-08-23
**Test environment:** Regtest, local, multiple node instances
**Fixes tested:** Masternode payment vote quorum, PoW hash determinism, build compatibility (Boost 1.83 / modern toolchains)

---

## 1. Summary

Two bugs originally observed on the production chain were identified, fixed, and verified on regtest:

1. **Masternode payment enforcement rendered ineffective** because `CMasternodeMan::GetMasternodeScores()` does not filter out registered-but-inactive masternodes from the top-10 payment-vote ranking. As a result, the required signature quorum (6 out of `MNPAYMENTS_SIGNATURES_REQUIRED`) is practically never reached once a large share of registered masternodes is inactive — the enforcement fallback clause ("insufficient data, accept anyway") then triggers permanently, regardless of `SPORK_8` status.
2. **Non-deterministic PoW hash** in `Mega_Btx()` (`src/crypto/mega-btx.h`): `permutation_X[0]` was never initialized, yet still fed into `std::next_permutation()` — making the hash computation depend on compiler/optimization level/platform (Undefined Behavior), risking divergent block hashes across nodes.

In addition, several build compatibility issues against current toolchains (Boost 1.83, GCC 13 / modern libstdc++) were fixed; these had completely prevented building on a current Ubuntu 24.04 system.

**Result:** Both fixes were verified on regtest under realistic conditions (6 active + 11 deliberately inactive masternodes, `SPORK_8` enabled). A block omitting the masternode payout was correctly rejected by the fixed code with `bad-cb-payee (code 16)`.

---

## 2. Test setup

- **Network:** BitCore regtest, 7 linked node instances (1 controller/wallet node + 6 "hot" masternode instances)
- **Masternode collateral:** 1 BTX (regtest minimum)
- **Additionally:** 11 further masternode broadcasts with no daemon actually running behind them (permanently remain `PRE_ENABLED`) — reproducing the situation observed on the real network where a large share of registered masternodes is inactive (there: 627 of 683, ≈92%; here: 11 of 17, ≈65%, sufficient to reproduce the effect)
- **`SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT`:** manually activated (regtest-specific signing key added, see section 5)
- **Build:** full wallet build (Berkeley DB 4.8 built locally from source, not system BDB), including the Qt5 GUI variant

---

## 3. Finding 1: Masternode payment vote quorum

### Root cause

`src/masternodeman.cpp`, `CMasternodeMan::GetMasternodeScores()` only filtered by protocol version:

```cpp
for (auto& mnpair : mapMasternodes) {
    if (mnpair.second.nProtocolVersion >= nMinProtocol) {
        vecMasternodeScoresRet.push_back(...);
    }
}
```

This lets `NEW_START_REQUIRED`/`PRE_ENABLED`/`EXPIRED` masternodes into the top-10 payment-vote ranking as well, diluting the pool of masternodes actually eligible to vote.

### Fix

Added an opt-in `fFilterValidForPayment` parameter to `GetMasternodeScores()`/`GetMasternodeRank()`/`GetMasternodeRanks()`, enabled at the three payment-vote-relevant call sites (`CMasternodePayments::ProcessBlock`, `CMasternodePaymentVote::IsValid`, `CheckPreviousBlockVotes`). Other callers (InstantSend, PoSe verification) are left unchanged to avoid affecting their behavior.

### Regtest verification

1. 6 masternodes were brought to full `ENABLED` status, 11 more were deliberately left `PRE_ENABLED`.
2. `masternode winners 15` then showed quorum reached (`:6`) for several consecutive heights, e.g.:
   ```
   4321: mrGiSdHnoEH3c4T3kY445RU8oSbT7rzbM6:6
   4325: mxtjHJuyiHTvCt95fRJzzXCP1Vq6qzS4zQ:6
   4327: mzjVCiDYtVkBtK2Fb7SSQ5NUnuA5jumDjd:6
   ```
3. A block deliberately built (locally only, immediately reverted afterward) without the masternode payout was rejected, with `SPORK_8` active and the node fully synced, with:
   ```
   error code: -1
   error message: CreateNewBlock: TestBlockValidity failed: bad-cb-payee (code 16)
   ```
   Block height did not advance.

This confirms the fix keeps the voting quorum reachable even when a large share of the network's registered masternodes is inactive, restoring the enforcement logic's intended behavior.

---

## 4. Finding 2: PoW hash determinism (`Mega_Btx`)

### Root cause

`src/crypto/mega-btx.h`: the group-1 init loop (`HASH_FUNC_COUNT_1 = 8`) only initialized indices 1–7, never index 0:

```cpp
for (uint32_t i = 1; i < HASH_FUNC_COUNT_1; i++) {
    permutation_X[i] = i;
}
...
std::next_permutation(permutation_X, permutation_X + HASH_FUNC_COUNT_1);
```

`permutation_X[0]` remained uninitialized (stack garbage) but was fed into the permutation computation as a full element — Undefined Behavior that can produce compiler-/platform-dependent hashes for identical block content.

### Fix

```cpp
permutation_X[0] = 0;
```

added before the init loop. Index 0 is never read by the subsequent `switch()` logic (which starts at index 1), so the fix does not change the actual hash-branch selection for indices 1–7 — it only removes the non-determinism.

### Regtest verification

Over 4,000 blocks were successfully mined and consistently accepted by all participating nodes during testing, with no hash divergence observed. An isolated before/after comparison of the permutation order under different compiler flags was not separately performed in this test round (the risk mainly applies when comparing different node builds, not within a single process).

---

## 5. Build compatibility fixes (Boost 1.83 / modern toolchains)

Attempting to build `dev_8` on a current Ubuntu 24.04 system (Boost 1.83, GCC 13) surfaced several independent compile errors, all fixed:

- **`boost::bind` placeholders:** Boost ≥ 1.73 no longer injects `_1`/`_2`/`_3` into the global namespace by default. Affected: `torcontrol.cpp`, `validation.cpp`, `validationinterface.cpp`, `rpc/server.cpp`, and, in the Qt GUI build, additionally `bitcoingui.cpp`, `clientmodel.cpp`, `splashscreen.cpp`, `transactiontablemodel.cpp`, `walletmodel.cpp`.
- **`boost::signals2` disconnect-by-value:** Boost ≥ 1.74 changed `boost::function`'s internal storage, breaking `signal.disconnect(<freshly built functor>)`. Switched to stored `boost::signals2::connection` handles in `init.cpp`, `validation.cpp` (`ConnectTrace`), and `validationinterface.cpp` (`CMainSignals`).
- **Missing standard includes:** `<deque>`/`<queue>` (`httpserver.cpp`), `<stdexcept>` (`support/lockedpool.cpp`), `<array>` (`qt/sendcoinsdialog.cpp`) — previously pulled in transitively via other headers, no longer with current libstdc++.

After these fixes, `dev_8` compiles cleanly as a headless wallet build (`bitcored`, `bitcore-cli`, `bitcore-tx`) and as a Qt5 GUI build (adding `bitcore-qt`).

---

## 6. Additional observation (not fixed, documented only)

`src/wallet/wallet.cpp`, `CMerkleTx::GetBlocksToMaturity()`: the code currently hardcodes `COINBASE_MATURITY_3` (4032 blocks) instead of the originally intended spork-gated logic (commented out directly below it in the source). This is unrelated to the two fixes described above but was noticed during testing and could surprise operators (much longer coinbase maturity than the standard Bitcoin Core 100-block default).

---

## 7. Test infrastructure notes (local development only, not part of the fixes)

Setting up the regtest environment required two pure test-configuration additions that are **not** part of the two bugfix commits and should be handled separately if needed:

- A regtest-specific `strSporkPubKey` in `chainparams.cpp` (`CRegTestParams`), since no signing key for spork activation previously existed on regtest.
- Network-dependent shortening of the masternode timing constants (`GetMasternodeMinMnpSeconds()` etc.) on regtest (10s/40s/30s instead of 10min/4h/3 days) to avoid long waits in future local testing. Mainnet/testnet values are unchanged.

---

## 8. Conclusion

Both core fixes (vote quorum, PoW determinism) have been successfully verified on regtest under realistic, deliberately "diluted" conditions. The build is restorable on current Linux toolchains (Boost 1.83), both headless and with the Qt5 GUI. Before a production rollout, additionally reviewing the coinbase maturity discrepancy noted in section 6 is recommended.
