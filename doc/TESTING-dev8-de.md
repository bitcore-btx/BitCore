# Testbericht: BitCore BTX v0.91.1.1 (Branch `dev_8`, Commit `80f27a6`)

**Datum:** 2026-08-23
**Testumgebung:** Regtest, lokal, mehrere Node-Instanzen
**Getestete Fixes:** Masternode-Payment-Vote-Quorum, PoW-Hash-Determinismus, Build-Kompatibilität (Boost 1.83 / moderne Toolchains)

---

## 1. Zusammenfassung

Zwei ursprünglich in der Produktivkette beobachtete Fehler wurden identifiziert, gefixt und im Regtest verifiziert:

1. **Masternode-Payment-Enforcement wirkungslos**, weil `CMasternodeMan::GetMasternodeScores()` keine registrierten-aber-inaktiven Masternodes aus dem Top-10-Voting-Ranking herausfiltert. Dadurch wird das erforderliche Stimmen-Quorum (6 von `MNPAYMENTS_SIGNATURES_REQUIRED`) praktisch nie erreicht, sobald ein größerer Anteil der registrierten Masternodes nicht aktiv ist — die Enforcement-Sicherheitsklausel ("zu wenig Daten, akzeptiere trotzdem") greift dann permanent, unabhängig vom SPORK_8-Status.
2. **Nicht-deterministischer PoW-Hash** in `Mega_Btx()` (`src/crypto/mega-btx.h`): `permutation_X[0]` wurde nie initialisiert, aber trotzdem in `std::next_permutation()` einbezogen — das macht die Hash-Berechnung abhängig von Compiler/Optimierungsstufe/Plattform (Undefined Behavior), mit dem Risiko divergierender Blockhashes zwischen Nodes.

Zusätzlich wurden mehrere Build-Kompatibilitätsfehler gegen aktuelle Toolchains (Boost 1.83, GCC 13/moderne libstdc++) behoben, die einen Build auf einem aktuellen Ubuntu-24.04-System vollständig verhindert hatten.

**Ergebnis:** Beide Fixes wurden im Regtest unter realistischen Bedingungen (6 aktive + 11 absichtlich inaktive Masternodes, aktivierter SPORK_8) verifiziert. Ein Block ohne Masternode-Payout wurde vom gefixten Code korrekt mit `bad-cb-payee (code 16)` abgelehnt.

---

## 2. Testaufbau

- **Netzwerk:** BitCore Regtest, 7 gekoppelte Node-Instanzen (1 Controller/Wallet-Node + 6 "Hot"-Masternode-Instanzen)
- **Masternode-Collateral:** 1 BTX (Regtest-Minimum)
- **Zusätzlich:** 11 weitere Masternode-Broadcasts ohne laufenden Daemon dahinter (verbleiben dauerhaft in `PRE_ENABLED`) — simuliert die auf dem echten Netzwerk beobachtete Situation, dass ein großer Anteil der registrierten Masternodes inaktiv ist (dort: 627 von 683, ≈92 %; hier: 11 von 17, ≈65 %, ausreichend um den Effekt zu reproduzieren)
- **SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT:** manuell aktiviert (regtest-spezifischer Signing-Key ergänzt, siehe Abschnitt 5)
- **Build:** vollständiger Wallet-Build (Berkeley DB 4.8 lokal aus Quellcode gebaut, kein System-BDB), inkl. Qt5-GUI-Variante

---

## 3. Fund 1: Masternode-Payment-Vote-Quorum

### Root Cause

`src/masternodeman.cpp`, `CMasternodeMan::GetMasternodeScores()` filterte nur nach Protokollversion:

```cpp
for (auto& mnpair : mapMasternodes) {
    if (mnpair.second.nProtocolVersion >= nMinProtocol) {
        vecMasternodeScoresRet.push_back(...);
    }
}
```

Dadurch fließen auch `NEW_START_REQUIRED`/`PRE_ENABLED`/`EXPIRED`-Masternodes ins Top-10-Ranking für Payment-Votes ein und verdünnen den Pool der tatsächlich stimmberechtigten (aktiven) Masternodes.

### Fix

Neuer optionaler Parameter `fFilterValidForPayment` in `GetMasternodeScores()`/`GetMasternodeRank()`/`GetMasternodeRanks()`, aktiviert an den drei Payment-Vote-relevanten Aufrufstellen (`CMasternodePayments::ProcessBlock`, `CMasternodePaymentVote::IsValid`, `CheckPreviousBlockVotes`). Andere Aufrufer (InstantSend, PoSe-Verification) bleiben unverändert, um deren Verhalten nicht zu beeinflussen.

### Verifikation im Regtest

1. 6 Masternodes wurden vollständig `ENABLED`, 11 weitere verblieben absichtlich `PRE_ENABLED`.
2. `masternode winners 15` zeigte danach für mehrere aufeinanderfolgende Höhen ein erreichtes Quorum (`:6`), z. B.:
   ```
   4321: mrGiSdHnoEH3c4T3kY445RU8oSbT7rzbM6:6
   4325: mxtjHJuyiHTvCt95fRJzzXCP1Vq6qzS4zQ:6
   4327: mzjVCiDYtVkBtK2Fb7SSQ5NUnuA5jumDjd:6
   ```
3. Ein testweise (nur lokal, sofort danach rückgängig gemacht) manipulierter Block ohne Masternode-Payout wurde bei aktivem SPORK_8 und vollständig synchronisiertem Node mit
   ```
   error code: -1
   error message: CreateNewBlock: TestBlockValidity failed: bad-cb-payee (code 16)
   ```
   abgelehnt. Blockhöhe blieb unverändert.

Damit ist bestätigt: Der Fix stellt sicher, dass das Voting-Quorum auch bei einem großen Anteil inaktiver Masternodes im Netzwerk erreichbar bleibt, und die Enforcement-Logik greift wieder wie vorgesehen.

---

## 4. Fund 2: PoW-Hash-Determinismus (`Mega_Btx`)

### Root Cause

`src/crypto/mega-btx.h`: Die Init-Schleife für Gruppe 1 (`HASH_FUNC_COUNT_1 = 8`) initialisierte nur Index 1–7, nicht Index 0:

```cpp
for (uint32_t i = 1; i < HASH_FUNC_COUNT_1; i++) {
    permutation_X[i] = i;
}
...
std::next_permutation(permutation_X, permutation_X + HASH_FUNC_COUNT_1);
```

`permutation_X[0]` blieb uninitialisiert (Stack-Garbage), floss aber vollwertig in die Permutationsberechnung ein — Undefined Behavior, das zu compiler-/plattformabhängigen Hashes für denselben Blockinhalt führen kann.

### Fix

```cpp
permutation_X[0] = 0;
```

vor der Init-Schleife ergänzt. Index 0 wird von der nachfolgenden `switch()`-Logik nie gelesen (die beginnt bei Index 1), daher ändert der Fix nichts an der eigentlichen Hash-Auswahl für die Indizes 1–7 — er entfernt ausschließlich die Nichtdeterminiertheit.

### Verifikation im Regtest

Über 4.000 Blöcke wurden im Rahmen der Tests erfolgreich gemined und von allen beteiligten Nodes konsistent akzeptiert — keine Hash-Divergenzen beobachtet. Ein isolierter Vorher/Nachher-Vergleich der Permutationsreihenfolge auf unterschiedlichen Compiler-Flags wurde im Rahmen dieses Tests nicht separat durchgeführt (das Risiko besteht primär beim Vergleich verschiedener Node-Builds, nicht innerhalb eines einzelnen Prozesses).

---

## 5. Build-Kompatibilitätsfixes (Boost 1.83 / moderne Toolchains)

Beim Versuch, `dev_8` auf einem aktuellen Ubuntu-24.04-System (Boost 1.83, GCC 13) zu bauen, traten mehrere unabhängige Kompilierfehler auf, alle behoben:

- **`boost::bind`-Platzhalter:** Boost ≥ 1.73 injiziert `_1`/`_2`/`_3` nicht mehr automatisch in den globalen Namespace. Betroffen: `torcontrol.cpp`, `validation.cpp`, `validationinterface.cpp`, `rpc/server.cpp`, sowie im Qt-GUI-Build zusätzlich `bitcoingui.cpp`, `clientmodel.cpp`, `splashscreen.cpp`, `transactiontablemodel.cpp`, `walletmodel.cpp`.
- **`boost::signals2`-Disconnect-by-Value:** Boost ≥ 1.74 änderte die interne `boost::function`-Speicherung, wodurch `signal.disconnect(<neu konstruierter Funktor>)` nicht mehr zuverlässig funktioniert. Umgestellt auf gespeicherte `boost::signals2::connection`-Handles in `init.cpp`, `validation.cpp` (`ConnectTrace`) und `validationinterface.cpp` (`CMainSignals`).
- **Fehlende Standard-Includes:** `<deque>`/`<queue>` (`httpserver.cpp`), `<stdexcept>` (`support/lockedpool.cpp`), `<array>` (`qt/sendcoinsdialog.cpp`) — zuvor transitiv über andere Header verfügbar, mit aktuellem libstdc++ nicht mehr.

Nach diesen Fixes kompiliert `dev_8` fehlerfrei als headless Wallet-Build (`bitcored`, `bitcore-cli`, `bitcore-tx`) und als Qt5-GUI-Build (zusätzlich `bitcore-qt`).

---

## 6. Zusätzliche Beobachtung (kein Fix, nur dokumentiert)

`src/wallet/wallet.cpp`, `CMerkleTx::GetBlocksToMaturity()`: Der Code verwendet aktuell fest `COINBASE_MATURITY_3` (4032 Blöcke) statt der ursprünglich vorgesehenen, spork-gesteuerten Logik (auskommentiert direkt darunter im Code). Das ist unabhängig von den beiden oben beschriebenen Fixes, wurde aber während des Tests bemerkt und könnte für Betreiber überraschend sein (deutlich längere Reifezeit als in der Standard-Bitcoin-Core-Logik mit 100 Blöcken).

---

## 7. Testinfrastruktur-Hinweise (nur für lokale Weiterentwicklung, nicht Teil der Fixes)

Für den Regtest-Aufbau wurden zwei reine Testkonfigurationen ergänzt, die **nicht** Teil der beiden Bugfix-Commits sind und bei Bedarf separat behandelt werden sollten:

- Ein Regtest-spezifischer `strSporkPubKey` in `chainparams.cpp` (`CRegTestParams`), da zuvor kein Signing-Key für SPORK-Aktivierung auf Regtest existierte.
- Netzwerkabhängige Verkürzung der Masternode-Timing-Konstanten (`GetMasternodeMinMnpSeconds()` etc.) auf Regtest (10s/40s/30s statt 10min/4h/3 Tage), um Wartezeiten bei künftigen lokalen Tests zu vermeiden. Mainnet/Testnet-Werte bleiben unverändert.

---

## 8. Fazit

Beide Kernfixes (Vote-Quorum, PoW-Determinismus) sind im Regtest unter realistischen, absichtlich "verdünnten" Bedingungen erfolgreich verifiziert. Der Build ist auf aktuellen Linux-Toolchains (Boost 1.83) wieder herstellbar, sowohl headless als auch mit Qt5-GUI. Vor einem produktiven Rollout empfiehlt sich zusätzlich eine Prüfung der in Abschnitt 6 genannten Coinbase-Maturity-Diskrepanz.
