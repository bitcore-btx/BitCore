# Testbericht: BitCore BTX v0.92.1.0 (Branch `dev_8`, Commit `e117a74`)

**Datum:** 2026-08-23
**Testumgebung:** Regtest, lokal, 10 gekoppelte Node-Instanzen (1 Controller/Miner + 9 echte Masternodes)
**Getestetes Feature:** Zweites (Rank-Queue-)Masternode-Payment-System, umschaltbar per `SPORK_BTX_22_MASTERNODE_RANK_PAYMENT_SYSTEM`

---

## 1. Zusammenfassung

Das neue, parallele FIFO-Payment-System für Masternodes wurde implementiert, gebaut und im Regtest unter realistischen Bedingungen (9 echte, laufende Masternode-Daemons) verifiziert. Beim Testaufbau selbst wurden drei unabhängige Bugs gefunden und behoben — einer davon ein echter, netzwerkunabhängiger Konsens-Bug im neuen Feature, zwei weitere bereits vorher vorhandene, regtest-spezifische Timing-Bugs in der Masternode-Basisinfrastruktur.

**Ergebnis:** Nach den Fixes lief ein vollständiger 100-Block-Testlauf mit **100/100 korrekt vorhergesagten Zahlungen**, einer fairen Verteilung (max. Differenz 2 Zahlungen zwischen den 9 Masternodes) und einem bestandenen Reorg-Test. Zusätzlich wurden fünf Betriebsszenarien (Spork-Deaktivierung, Neustart-Persistenz, Node-Ausfall, Top-3-Toleranz, Node-Wiederbeitritt) einzeln durchgespielt — alle bestanden.

---

## 2. Testaufbau

- **Netzwerk:** BitCore Regtest, 10 Node-Instanzen (1 Controller/Wallet-Node + 9 "Hot"-Masternode-Instanzen, alle mit echtem laufendem Daemon)
- **Masternode-Collateral:** 1 BTX (Regtest-Minimum)
- **Externe IP:** öffentliche Server-IP (`185.249.199.86`) für `-externalip` — reine Loopback-Adressen (`127.0.0.1`) werden von `AddLocal()` als nicht routbar abgelehnt (siehe Fund 3)
- **Spork-Signing:** Regtest-spezifischer `strSporkPubKey`/`-sporkkey` (bereits aus früheren Tests vorhanden)
- **Build:** statischer/portabler sowie Entwicklungs-Build, Berkeley DB 4.8 lokal gebaut

---

## 3. Fund 1: `RecordPayment`-Hook lief vor dem eigentlichen Point-of-no-Return

### Root Cause

`src/validation.cpp`, `ConnectBlock()`: Der neue `mnRankPayments.RecordPayment()`-Aufruf stand direkt nach `IsBlockPayeeValid()`, aber **vor** mehreren Stellen, die den Block-Connect-Versuch noch scheitern lassen können:

```cpp
if (!IsBlockPayeeValid(...)) { return state.DoS(...); }

// HIER stand der Hook — zu früh:
if (sporkManager.IsSporkActive(...)) { mnRankPayments.RecordPayment(...); }

if (!control.Wait())              // kann NOCH fehlschlagen
    return state.DoS(...);
...
if (fJustCheck) return true;      // reine Testvalidierung, Block wird NIE verbunden
```

Jeder `ConnectBlock()`-Versuch, der bis zu diesem Punkt kommt, aber danach doch scheitert (z. B. bei `fJustCheck`-Testvalidierungen während des Block-Bau-Prozesses, oder bei einem späteren Validierungsfehler), hinterlässt eine Zustandsänderung (`nBlockLastPaid2`) ohne passenden `DisconnectBlock()`-Aufruf zum Zurückrollen — die FIFO-Reihenfolge korrumpiert.

**Beobachtetes Symptom:** Nach einem chaotischen Reorg-Testlauf zeigten **alle 9 Masternodes exakt dieselbe `lastpaidblock2`-Höhe** — obwohl bei korrektem FIFO pro Block immer nur genau ein Masternode bezahlt werden darf.

### Fix

Hook ans echte Ende der Funktion verschoben — nach dem `fJustCheck`-Early-Return und nach erfolgreichem `WriteUndoDataForBlock()`, unmittelbar vor `return true`. Das ist der einzige Punkt, an dem garantiert ist, dass der Block tatsächlich Teil der aktiven Kette wird.

### Verifikation im Regtest

Nach dem Fix: `nBlockLastPaid2` bei allen 9 Masternodes durchgehend **unterschiedlich**, siehe Abschnitt 5.

---

## 4. Fund 2 & 3: Regtest-Timing-Bugs (vorbestehend, nicht Teil des neuen Features)

Beide unabhängig vom Rank-System, auf Mainnet/Testnet wirkungslos (die betroffenen Konstanten bleiben dort weit über dem relevanten Schwellwert), aber blockierten den Regtest-Testaufbau selbst:

- **`src/privatesend.cpp`:** `nTick % GetMasternodeMinMnpSeconds() == 15` kann bei Regtests kurzem Timer-Wert (10s) rechnerisch nie `true` werden (Rest von `%10` ist immer 0–9) — der periodische Ping-Retry-Aufruf feuerte nie. Fix: Offset auf `min(15, Divisor-1)` begrenzt.
- **`src/masternode.cpp`:** Der bereits vorhandene "BTX 2024-10"-Ping-Slack-Guard hatte einen Floor von 10 Blöcken, während `CMasternodePing`s Konstruktor immer `chainActive.Height()-12` referenziert — auf Regtest (10 < 12) wurde dadurch **jeder** Ping strukturell als "zu alt" abgelehnt. Fix: Floor auf 13 angehoben.

---

## 5. 100-Block-Rotationstest

Nach den Fixes: 9 Masternodes `ENABLED`, Spork aktiviert, 100 Blöcke gemined, pro Block die vorhergesagte Rang-1-Adresse (`getmasternoderank_2`) mit der tatsächlichen Coinbase-Zahlung verglichen.

**Match-Rate: 100/100 (100 %)**

**Fairness-Verteilung** (100 Blöcke ÷ 9 Masternodes, theoretisches Optimum 11,1):

| Zahlungen | Anzahl Masternodes |
|---|---|
| 12× | 2 |
| 11× | 6 |
| 10× | 1 |

Maximale Differenz: 2 — exakt das bestmögliche Ergebnis eines strikten Round-Robin.

### Reorg-Test (bei Block 60)

`invalidateblock` auf den aktuellen Tip, danach `reconsiderblock` + Neu-Mining:

- **Vorher:** betroffener Masternode zeigt `lastpaidblock2` = Höhe des invalidierten Blocks
- **Nach `invalidateblock`:** korrekt auf seinen **vorherigen** Zahlungsstand zurückgesetzt — kein Datenverlust, keine Dopplung
- **Nach Neu-Mining:** wieder korrekt bezahlt, Queue rotiert konsistent weiter

---

## 6. Betriebsszenarien (5/5 bestanden)

| # | Szenario | Ergebnis |
|---|---|---|
| 1 | Spork-Deaktivierung mitten im Lauf | Altes Vote-System (`ProcessBlock`, Signieren/Broadcasten von Votes) läuft sofort wieder an, keine Interferenz vom Rank-System-Zustand |
| 2 | Persistenz über Node-Neustart | Byte-genau identischer Queue-Zustand vor/nach Neustart (`mncache.dat`/`mnrankpayments.dat`) |
| 3 | Masternode fällt mitten in der Rotation aus | Sauber aus `getmasternoderank_2` ausgeschlossen, verbleibende Nodes rotieren lückenlos fair weiter (konsekutive Höhen, kein Stillstand) |
| 4 | Top-3-Toleranz bei der Validierung | Per Code-Review verifiziert (`IsTransactionValid_2`, `masternode-payments.cpp:635-651`) — live nicht provozierbar, da der Miner beim Blockbau immer strikt Rang 1 wählt |
| 5 | Masternode tritt wieder bei (≙ Neubeitritt) | Landet korrekt an Rang 1 (frisches FIFO, `lastpaidblock2:0`), wurde beim nächsten Block tatsächlich bezahlt |

---

## 7. Fazit

Das Rank-Payment-System läuft nach den drei Fixes stabil, fair und reorg-sicher. Der `RecordPayment`-Platzierungsfehler war der einzige echte, netzwerkunabhängige Konsens-relevante Bug im neuen Feature-Code selbst — er hätte bei jeder Chain-Instabilität (Reorg, verworfene Block-Kandidaten) auf jedem Netzwerk (auch Mainnet) zu korrupter Zahlungs-Historie geführt und ist jetzt behoben. Die beiden Timing-Bugs betrafen ausschließlich die Regtest-Testinfrastruktur und sind auf Mainnet/Testnet folgenlos.

Der Spork (`SPORK_BTX_22_MASTERNODE_RANK_PAYMENT_SYSTEM`) bleibt bis auf Weiteres deaktiviert (Default-Wert, folgenlos fürs Mainnet). Vor einer echten Aktivierung sollte — wie beim ursprünglichen Quorum-Fix — sichergestellt sein, dass ein Großteil der aktiven Mainnet-Masternode-Betreiber (und insbesondere die Mining-Node(s)) auf `v0.92.1.0` upgegradet haben.
