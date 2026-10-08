# V7.2 — Commande SET_POWER du CORE vers la lampe

## Statut et audit avant modification

V7.1 est validée sur le montage matériel de référence. V7.2 ajoute le trajet de commande CORE-WIFI → UART → CORE-ZIGBEE → Zigbee → MAIN_LIGHTING → Zigbee → LAMP_C6 et son retour. **La validation fonctionnelle V7.2 sur matériel est confirmée le 7 octobre 2026** (résultats ci-dessous). Les routes HTTP V7.1 restent en lecture seule.

L’audit préalable identifie `Message`, `MessageRouter`, `MessageTracker`, `MessageDeduplicator`, `ZigbeeTransport`, `DeviceRegistry`, `CoreMainRuntime` et `V5ProvisioningRuntime` comme composants à réutiliser. `SET_POWER` utilise `ActionType::SET_LAMP_POWER`; brightness et automatic restent hors du nouveau trajet CORE.

Avant V7.2, `processAck` termine une commande pour tout ACK, même avec `ExecutionStatus::NOT_EXECUTED`, et ne vérifie pas sa provenance. Cela confond réception et exécution. Le cache circulaire de vingt résultats peut également évincer une commande pendant ses retransmissions. Les changements suivants répondent à ces problèmes avant toute extension des structures :

- ACK d’acceptation MAIN : `NOT_EXECUTED`, indicateur de réception distinct, commande toujours en attente.
- ACK final LAMP : `EXECUTED` ou `FAILED`, corrélé à l’ID original, aux identités et au type de commande.
- Rejet explicite de relais : `FAILED`, sans prétendre à une exécution sur lampe.
- Expiration : transport `FAILED`, `timedOut: true`, exécution inconnue (`NOT_EXECUTED`).
- CORE conserve seul le tracker et les retries : délai 5 secondes, deux retransmissions, même ID sur les trois tentatives.
- LAMP réserve une entrée de déduplication avant exécution et protège les résultats récents; saturation provoque un refus.

Le payload `Message` conserve les IDs métier globaux. Le prochain saut est passé séparément au transport; seul Zigbee traduit un identifiant de routage en adresse radio. Les champs existants `parentMainId` et `provisioningDeviceId` transportent respectivement le MAIN global et l’ID local V5 de la lampe pour ce trajet. Aucun second registre MAIN ni second modèle de message n’est créé.

Les tests natifs utilisent les services applicatifs, les codecs UART/Zigbee et le même cache de routes que le transport de production, avec une horloge de test. Ils vérifient exécution, ACK perdu, retry, déduplication, expiration, offline, destination inconnue et corrélation des ACK, ainsi que la collision entre CORE `1` et lampe locale `1`. Le test radio, les pertes réelles et la reprise après redémarrage restent à effectuer sur matériel.

## Incident de routage observé le 7 octobre 2026

L’essai matériel des commandes `497037133` et `497037135` atteint MAIN_LIGHTING : CORE reçoit l’acceptation, retransmet deux fois, puis expire sans confirmation d’exécution LAMP. Cette acceptation valide la réception par MAIN; elle ne confirme pas que MAIN a adressé la transmission suivante à la lampe. **Le trajet matériel V7.2 reste à valider après correction.**

Les journaux MAIN montrent une lampe dont l’ID local V5 est `1` dans `DEVICE_ANNOUNCE`. CORE utilise également l’ID logique `1`. L’ancien cache physique indexait les routes par ce seul nombre. La réception d’une commande de source CORE `1` remplaçait alors la route locale de la lampe `1` par l’adresse courte de CORE-ZIGBEE. MAIN pouvait envoyer son ACK d’acceptation au CORE, puis adresser aussi la commande destinée à la lampe à CORE-ZIGBEE. Un log APS `message sent` confirme cet envoi radio, sans identifier une exécution sur LAMP.

La correction sépare les espaces de routage avec `CommunicationRouteScope::CORE` et `CommunicationRouteScope::LOCAL_DEVICE`. La clé physique devient `(espace, identifiant)` : `(CORE, 1)` désigne CORE-ZIGBEE et `(LOCAL_DEVICE, 1)` désigne la lampe. `sendMessageVia` choisit le prochain saut et son espace. Les IDs métier V7, le payload des messages, les identités de pairing persistées et les adresses courtes existantes sont conservés.

La régression native doit exercer le même cache de routes que le transport Zigbee de production : apprendre les deux identités `1` dans les deux ordres, conserver leurs deux adresses distinctes, puis résoudre séparément le retour CORE et l’aller LAMP. Les premiers tests de services et de codecs ne reproduisaient pas cette collision dans le cache physique; leurs succès ne suffisaient donc pas à valider ce routage.

Les quatre cartes doivent recevoir le firmware V7.2 corrigé, y compris LAMP_C6 pour traiter les commandes et ACK V7.2. L’incident observé localise la collision dans le cache de routage utilisé par MAIN. La procédure ci-dessous reprend le montage et le pairing V7.1 conservés, avec reflash des quatre profils puis essais ON/OFF normaux.

### Résultats matériels après correction — validation fonctionnelle confirmée le 7 octobre 2026

Le premier essai `SET_POWER ON` a réussi sur les cartes : commande `1364364413`, destination lampe V7 `3559985816`; LAMP a reçu la commande et journalisé `Power -> ON`. CORE a traité l’ACK final et affiché `state=executed retries=0`. Le retour LAMP→MAIN indique le prochain saut local `104670534`, scope `LOCAL`, adresse courte `0x0000` — l’adresse normale du coordinateur MAIN dans ce réseau Zigbee. Le journal ne montre qu’une ligne d’exécution `Power -> ON`.

Cela valide le trajet logiciel ON et le retour de son ACK.

Le test suivant a perdu l’ACK pour la commande ON `1406382499`, elle aussi destinée à `3559985816`. LAMP a affiché une fois `Power -> ON`, puis `ACK volontairement perdu pour commande CORE`. Au retry, elle a reçu le même ID, journalisé `duplicate command, no execution` et renvoyé le résultat en cache. CORE a affiché `executed retries=1`. Ce test confirme l’ACK perdu, la retransmission et l’absence de seconde exécution dans le cache LAMP.

OFF et les refus offline/destination inconnue ont été confirmés comme fonctionnels par l’utilisateur; leurs IDs et logs détaillés ne sont pas archivés.

Pour la commande `2731802491`, CORE a affiché `RETRY #1`, `RETRY #2`, puis `expired (execution unknown)` avec `retries=2`, comme attendu pendant la coupure du retour UART. L’exécution LAMP de cette commande reste volontairement inconnue côté CORE. Après rétablissement du fil de retour, l’utilisateur confirme qu’une nouvelle commande fonctionne. Les tests planifiés ON/OFF, perte d’ACK avec déduplication, refus offline/inconnu, expiration et récupération sont ainsi confirmés sur le montage; les logs disponibles sont consignés ci-dessus.

## Architecture et responsabilités

```text
Console série CORE-WIFI : power <id V7 de la lampe> on|off
    │ CoreCommandService, DeviceRegistry, MessageTracker
    ▼
CORE-WIFI ─── UART COMMAND ───► CORE-ZIGBEE
                                  │ CoreCommandBridge, prochain saut MAIN local
                                  ▼ Zigbee APS réel
                            MAIN_LIGHTING
                                  │ MainCommandRelay, LampRegistry existant
                                  ▼ Zigbee APS réel, prochain saut LAMP local
                              LAMP_C6
                                  │ MessageRouter → executeAction → GPIO18
                                  ▼ ACK final, même commande corrélée
LAMP ── Zigbee ──► MAIN ── Zigbee ──► CORE-ZIGBEE ── UART ──► CORE-WIFI
```

CORE-WIFI vérifie l’inventaire puis suit la commande. CORE-ZIGBEE relaie sans registre global. MAIN vérifie son attribution V7, son pairing local et l’état de la lampe, émet une acceptation et transmet le payload inchangé. LAMP résout l’identité globale vers sa lampe locale à la frontière d’exécution. Le MAIN ne déclenche pas son propre `executeAction` pour ces commandes et ne possède pas de tracker supplémentaire.

La console CORE utilise une file FreeRTOS bornée de huit demandes. Le runtime traite cette file et tous les ACK dans sa boucle : la tâche de saisie ne modifie ni le registre ni le tracker. L’accès au DeviceRegistry réutilise son mutex HTTP existant.

## Message et sérialisation

Le modèle existant `Message` est réutilisé :

| Champ | SET_POWER V7.2 |
| --- | --- |
| `id` | ID de commande non nul sur 32 bits, inchangé aux retries |
| `sourceId` | CORE logique, `1` |
| `destinationId` | ID V7 global de la lampe |
| `type` | `COMMAND` |
| `commandType` | `ActionType::SET_LAMP_POWER` |
| `value` | `1` pour on, `0` pour off |
| `timestamp` | Horloge du CORE à la création, conservée aux retries |
| `status` / `executionStatus` | États existants de transmission / exécution |
| `parentMainId` | ID V7 global du MAIN propriétaire |
| `provisioningDeviceId` | ID local V5 de la lampe, pour résoudre le prochain saut |

Les champs de provisioning gardent leur signification V5 dans les messages de pairing. La destination métier reste globale dans toute la chaîne de commande; `sendMessageVia` fournit séparément le prochain saut local et son espace de routage. Les routes CORE et les routes des appareils locaux peuvent utiliser le même nombre sans se remplacer. L’adresse courte Zigbee reste interne au transport.

UART conserve sa version 1, son CRC, les anciennes trames topologiques et son payload maximal de 46 octets. Deux types sont ajoutés : `COMMAND=7` et `COMMAND_RESULT=8`. Le sous-ensemble de `Message` nécessaire est sérialisé explicitement en 39 octets, soit une trame de 49 octets. La séquence UART sur 16 bits n’est jamais l’ID de commande. Le format Zigbee existant est conservé.

CORE initialise le générateur d’IDs sur une valeur aléatoire au démarrage, puis utilise une séquence 32 bits qui évite zéro. Cela réduit les collisions avec un cache LAMP encore vivant; il ne s’agit pas d’un journal durable.

## ACK, résultat et erreurs

Tous les ACK portent leur propre `id`, l’ID original complet dans `value2`, le même type de commande, les mêmes métadonnées de routage et la destination CORE `1`.

| Résultat | Source de l’ACK | `executionStatus` | Effet CORE |
| --- | --- | --- | --- |
| Acceptation | MAIN global | `NOT_EXECUTED`, `value=0` | `accepted=true`, attente maintenue |
| Exécution | LAMP globale | `EXECUTED`, `value=1` | Commande terminée, succès logiciel GPIO |
| Échec GPIO | LAMP globale | `FAILED`, `value=3` | Commande terminée, erreur d’exécution |
| Refus / erreur de relais | CORE/MAIN/LAMP concerné | `FAILED`, code ci-dessous | Commande terminée, cause explicite |
| Aucune confirmation finale | Aucun ACK final valide | `NOT_EXECUTED` | `timedOut=true`, résultat physique inconnu |

Une acceptation ne décale pas le délai d’attente. Un ACK `PARTIAL` est rejeté pour SET_POWER, qui est indivisible. Le tracker contrôle provenance, destination, type, ID original et métadonnées. Il ignore les ACK tardifs après expiration ou clôture.

`EXECUTED` confirme le retour réussi de l’écriture GPIO et la mise à jour de l’état local. Ce n’est pas une mesure électrique de la lampe : le contrôle visuel ou au multimètre reste nécessaire. Un refus reçu après un retry ne prouve pas que toutes les tentatives précédentes étaient restées sans effet.

| Code | Cause |
| --- | --- |
| 10 `unknown_destination` | Lampe absente ou rôle incorrect |
| 11 `offline` | Lampe ou MAIN offline |
| 12 `invalid_command` | Action ou valeur non prise en charge |
| 13 `invalid_route` | Parent, pairing ou attribution V7 incohérents |
| 14 `transport_failure` | UART/Zigbee indisponible ou envoi refusé |
| 15 `dedup_full` | Toutes les entrées de résultat sont protégées |
| 16 `tracker_full` | Vingt commandes CORE attendent un ACK |
| 17 `duplicate_id` | ID déjà suivi, ou réutilisé avec une consigne différente |

CORE refuse localement une destination inconnue/offline avant émission. MAIN et LAMP revérifient la route et l’état local, sans modifier arbitrairement le registre. Le code d’échec final est conservé dans `pending.message.value2`, après la fin de l’attente; la consigne `value` reste intacte.

## Délais et déduplication

- Délai de chaque tentative : `MESSAGE_TIMEOUT=5000 ms`.
- Maximum : un envoi initial et `MAX_MESSAGE_RETRIES=2` retransmissions.
- Horizon total CORE : 15 secondes depuis le premier envoi, même si la boucle est suspendue longtemps.
- Les retries gardent le même ID, la destination, la consigne et le timestamp initial.
- Cache LAMP : vingt entrées, clé `(sourceId, messageId)`, réservées avant l’écriture GPIO et protégées 60 secondes.
- Le cache compare aussi destination, MAIN, ID local, action et valeur. Même ID avec une autre consigne : refus, sans réexécution ni faux ACK de succès.
- Pendant la protection, aucune insertion, même historique V5, ne peut évincer une entrée. Une saturation refuse la nouvelle exécution. Une entrée devenue remplaçable peut ensuite être recyclée.
- Un doublon identique renvoie le résultat précédent sans nouvelle écriture GPIO.

La protection couvre les retransmissions de ce tracker pendant une session active de la lampe. Elle n’est pas persistante : reboot LAMP, effacement NVS, collision d’ID ou trame retardée au-delà de la rétention ne bénéficient pas d’une garantie durable de non-réexécution. Les ordres ON/OFF restent idempotents, mais un journal persistant serait nécessaire pour garantir une seule exécution entre redémarrages.

## Compiler et tester sur PC

```powershell
pio run -e core_wifi_c6
pio run -e core_zigbee_c6
pio run -e main_light_c6
pio run -e lamp_c6
pio test -e native
```

Si `pio` n’est pas dans PATH :

```powershell
$Pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
& $Pio run -e core_wifi_c6 -e core_zigbee_c6 -e main_light_c6 -e lamp_c6
# Le compilateur natif de cette machine est MSYS2 UCRT64.
$env:Path = "C:\msys64\ucrt64\bin;" + $env:Path
& $Pio test -e native
```

La suite V7.2 fait traverser les vrais codecs UART et Zigbee aux messages échangés par les services CORE/bridge/MAIN et le routeur LAMP. Elle compte les passages dans le setter de puissance, plutôt que de vérifier seulement un état ON idempotent. Les pertes couvrent commande, ACK LAMP, retour MAIN et retour UART. Le matériel et les drivers radio/GPIO ne sont pas simulés comme preuves physiques.

## Procédure de validation matérielle

### 1. Préparer les quatre cartes

Conserver le montage V7.1 déjà validé : CORE-WIFI GPIO4 TX → CORE-ZIGBEE GPIO5 RX, CORE-WIFI GPIO5 RX ← CORE-ZIGBEE GPIO4 TX, GND commun, UART1 à 115200 bauds. Chaque carte reste alimentée par son USB. Vérifier la charge de test reliée à la sortie GPIO18 de LAMP_C6.

Identifier les ports; les exemples `COM...` doivent être remplacés par les ports réels :

```powershell
pio device list
pio run -e core_wifi_c6   -t upload --upload-port COM_WIFI
pio run -e core_zigbee_c6 -t upload --upload-port COM_ZIGBEE
pio run -e main_light_c6 -t upload --upload-port COM_MAIN
pio run -e lamp_c6       -t upload --upload-port COM_LAMP
```

Fermer un moniteur avant d’uploader sur le même port. Pour reprendre l’incident de routage, compiler puis reflasher **les quatre profils corrigés sans effacement de NVS ni de `zb_storage`** : le pairing V7.1 est réutilisé. La correction du cache s’applique au code des cartes. Ces commandes flashent aussi la partition du profil; CORE-WIFI conserve 1 Mio et les trois profils Zigbee conservent leur partition. Vérifier le rôle et la date de compilation de chaque carte dans son journal de démarrage. En cas de PAN ancien incohérent distinct de cet incident, suivre le dépannage du [guide CORE](V7_CORE_DOUBLE_C6.md), puis rétablir le pairing et vérifier les IDs.

### 2. Ouvrir les moniteurs et vérifier l’inventaire

Dans quatre terminaux distincts :

```powershell
pio device monitor -p COM_WIFI   -b 115200 --filter time --filter send_on_enter
pio device monitor -p COM_ZIGBEE -b 115200 --filter time --filter send_on_enter
pio device monitor -p COM_MAIN   -b 115200 --filter time --filter send_on_enter
pio device monitor -p COM_LAMP   -b 115200 --filter time --filter send_on_enter
```

Attendre Wi-Fi connecté avec IP, UART peer ready, réseau Zigbee prêt, MAIN avec ID CORE attribué et lampe paired. Dans un cinquième terminal PowerShell :

```powershell
$CoreIp = "IP_AFFICHEE_PAR_CORE_WIFI"
curl.exe -i "http://$CoreIp/api/v1/health"
curl.exe -i "http://$CoreIp/api/v1/core"
curl.exe -i "http://$CoreIp/api/v1/modules"
curl.exe -i "http://$CoreIp/api/v1/devices"
```

Attendre MAIN et LAMP `online=true`. Relever l’ID V7 de la lampe dans `devices`, pas son ID local V5 ni son adresse radio. La capture V7.1 donnait `3559985816`; utiliser ce nombre seulement s’il est toujours présent.

### 3. Tester ON puis OFF depuis CORE-WIFI

Saisir dans le **moniteur CORE-WIFI**, puis Entrée :

```text
help
power 3559985816 on
command <message_id_affiche>
power 3559985816 off
command <nouveau_message_id_affiche>
```

Résultat attendu pour chaque ordre : ID non nul, `sent`, acceptation MAIN (`accepted (waiting execution)`), puis `executed`. LAMP doit afficher `Power -> ON` ou `OFF`, et la sortie doit changer réellement. MAIN ne doit pas exécuter sa propre sortie. Les deux ordres distincts ont deux IDs; l’ID d’un ACK final est différent, son `value2` référence l’ordre original.

### 4. Perdre un ACK et prouver la déduplication

Sur le **moniteur LAMP**, saisir :

```text
drop_ack
```

Puis, sur CORE-WIFI, envoyer une commande ON/OFF. Attendre au moins 6 secondes et interroger son ID avec `command <id>`.

Résultat attendu : la lampe exécute une fois, affiche `ACK volontairement perdu pour commande CORE`, CORE affiche `RETRY #1` au bout d’environ 5 secondes avec le même ID, LAMP affiche `duplicate command, no execution`, puis CORE termine `executed` avec `retries=1`. Pour cet ID, un seul passage `Power -> ...` doit être visible. `drop_ack` est un diagnostic explicite, désarmé après un ACK; la radio utilisée reste Zigbee réel.

### 5. Refus offline et destination inconnue

Débrancher LAMP_C6, attendre plus de 10 secondes et vérifier `/api/v1/devices` jusqu’à `online=false`. Envoyer `power <id V7 lampe> on` depuis CORE-WIFI : attendre `refused: offline`, aucun nouvel ACK d’exécution. Rebrancher la lampe et attendre son retour online.

Choisir ensuite un ID non nul absent de l’inventaire, par exemple `999999` après vérification. `power 999999 on` doit produire `refused: unknown_destination`, sans émission ni tracker créé. Refaire le refus offline avec MAIN débranché. L’utilisateur a confirmé le comportement offline et inconnu; les IDs et logs restent à archiver.

### 6. Expiration et retour à l’état normal

Pour provoquer une perte de commande contrôlée, débrancher LAMP et envoyer immédiatement un ordre pendant que CORE et MAIN la considèrent encore online. La propagation de l’état offline peut entraîner un refus explicite avant l’horizon total : noter ce résultat sans exiger artificiellement une expiration.

Pour isoler une expiration sans dépendre du délai offline, vérifie d’abord que les modules sont online, puis sépare temporairement **uniquement le fil de retour CORE-ZIGBEE TX GPIO4 → CORE-WIFI RX GPIO5**. Conserve le fil aller CORE-WIFI TX GPIO4 → CORE-ZIGBEE RX GPIO5, le GND commun et les alimentations. Depuis CORE-WIFI, envoie une nouvelle commande opposée à l’état actuel, par exemple `power 3559985816 off` si la lampe est ON. LAMP doit la recevoir et l’exécuter une fois, mais son ACK ne pourra pas rejoindre CORE-WIFI. CORE retente le même ID à environ 5 et 10 secondes; vers 15 secondes, `command <id>` doit indiquer `expired (execution unknown)`, jamais `executed`. Rebranche ensuite le fil de retour et attends que MAIN et LAMP redeviennent online. Envoie une nouvelle commande avec un nouvel ID et vérifie son ACK final normal.

### 7. Conserver les preuves

Relever pour chaque essai : firmware/commit, quatre rôles et ports, IDs MAIN/LAMP, ID de commande, valeur, logs des quatre cartes, nombre d’exécutions LAMP, nombre de retries, état final CORE et état réel GPIO. Archivés : ON sous l’ID `1364364413` avec zéro retry; perte d’ACK ON sous `1406382499`, une exécution, doublon ignoré et fin `executed retries=1`; expiration CORE sous `2731802491`, deux retries et `execution unknown`. OFF, refus offline/inconnu et récupération après reconnexion sont confirmés par l’utilisateur, sans logs détaillés.

Selon les logs fournis et la confirmation de l’utilisateur, la validation fonctionnelle matérielle V7.2 est réussie sur quatre cartes pour `SET_POWER`, ses ACK, sa déduplication, les refus et la récupération après expiration. Le test d’expiration laisse explicitement l’état d’exécution de cette commande inconnu au CORE. Les IDs/logs OFF et refus sont rapportés par l’utilisateur mais n’ont pas été archivés.

## Résultats de développement et flash

**Référence avant correction du cache de routage, mesurée le 7 octobre 2026.** `pio test -e native` avait réussi **80 tests sur 80** : API 10, UART/topologie 6, DeviceRegistry 8, stabilisation 9, pairing 14, commandes V7.2 28 et codec Zigbee 5. Après la correction et les tests de collision de routage, le total natif est de **85 tests réussis sur 85**. Les quatre profils `core_wifi_c6`, `core_zigbee_c6`, `main_light_c6` et `lamp_c6` ont également compilé.

Après correction, `pio test -e native` réussit **85 tests sur 85** dans les sept suites, dont les 52 tests existants et 33 scénarios V7.2. Les cinq tests ajoutés font exercer à la régression le cache Zigbee réellement utilisé par le firmware : CORE `1` et appareil local `1` gardent des adresses distinctes dans les deux ordres d’apprentissage, les messages d’attribution apprennent la route CORE, une route CORE manquante n’envoie pas vers la lampe, l’adresse connue par `hardwareId` survit à l’attribution locale et la table pleine conserve les deux routes.

La compilation post-correction `pio run -e core_wifi_c6 -e core_zigbee_c6 -e main_light_c6 -e lamp_c6` retourne **4 SUCCESS**, sans modification des dépendances. Images `firmware.bin` et marges mesurées dans les partitions :

| Profil | `firmware.bin` (octets) | Partition applicative | Occupation réelle | Marge |
| --- | ---: | ---: | ---: | ---: |
| `core_wifi_c6` | 989 488 | 1 048 576 | 94,36 % | 59 088 |
| `core_zigbee_c6` | 548 336 | 1 966 080 | 27,89 % | 1 417 744 |
| `main_light_c6` | 523 808 | 1 966 080 | 26,64 % | 1 442 272 |
| `lamp_c6` | 526 672 | 1 966 080 | 26,79 % | 1 439 408 |

Ces tailles post-correction incluent l’en-tête et l’alignement de l’image flashée. Les partitions Zigbee ont toujours une marge supérieure à 1,4 Mio. CORE-WIFI conserve une marge de 59 088 octets, soit 5,64 % de sa partition. Pour référence, les mesures précédentes donnaient une RAM statique de 74 308 octets sur Wi-Fi, 34 124 sur CORE-ZIGBEE, 33 820 sur MAIN et 33 836 sur LAMP; elles ne comprennent pas toutes les allocations dynamiques des tâches et des drivers.

CORE-WIFI ajoute 6 336 octets à l’image V7.1 de référence (983 152 octets). La marge reste faible, mais V7.2 tient dans le budget conservé. Avant l’API de commande suivante, mesurer à nouveau la taille et auditer la map du linker; si nécessaire, envisager une optimisation de compilation/constantes, ou une partition applicative plus grande avec un plan de migration explicite. Aucun agrandissement n’a été appliqué ici.

La partition CORE-WIFI est explicitement figée dans `partitions_core_wifi.csv` à **1 048 576 octets**, budget V7.1 conservé. Le sdkconfig partagé mentionnait la partition Zigbee alors que l’artefact Wi-Fi de référence utilisait 1 Mio; le profil explicite évite une augmentation implicite lors d’une reconfiguration. Les partitions Zigbee restent à **1 966 080 octets**. Aucune dépendance externe n’est ajoutée.

## Limites d’architecture et essais complémentaires

- UART réel, routage APS, radio/coexistence et délais de bout en bout sous charge.
- Un ACK logiciel confirme l’écriture GPIO, pas une mesure électrique de la charge.
- Essai d’endurance radio et coexistence sous charge.
- Cache en RAM et rétention bornée; absence de garantie durable entre reboot LAMP ou pertes dépassant 60 secondes.
- Limite actuelle d’un MAIN mappé par CORE-ZIGBEE; aucun support multi-MAIN supplémentaire n’est introduit.
- Vingt résultats protégés par minute peuvent saturer la lampe; le refus est explicite et doit être observé sous charge.
- Marge flash CORE-WIFI à contrôler avant tout ajout futur, notamment l’API de commande.

SET_BRIGHTNESS et SET_AUTOMATIC ne sont pas implémentés sur ce nouveau trajet. L’API V7.1 et les structures du DeviceRegistry sont conservées. L’application mobile et l’API HTTP de commande restent des étapes ultérieures.

## Fichiers de cette étape

### Nouveaux fichiers

- `include/core_command_protocol.h`, `src/core_command_protocol.cpp` : modèle de commande/ACK, erreurs, résolution LAMP et diagnostic ACK perdu.
- `include/core_command_service.h`, `src/core_command_service.cpp` : coordination CORE, bridge et relais MAIN.
- `include/core_command_uart.h`, `src/core_command_uart.cpp` : adaptateur Message sur UART existant.
- `include/core_console.h`, `src/core_console.cpp` : saisie série et file de demandes CORE.
- `partitions_core_wifi.csv` : budget Wi-Fi 1 Mio explicite.
- `include/zigbee_route_table.h`, `src/zigbee_route_table.cpp` : cache portable de routes physiques, avec espaces CORE et appareil local, utilisé par le transport Zigbee et les tests natifs.
- `test/test_v7_commands/test_main.cpp` : 33 scénarios natifs, dont la régression de collision d’ID.
- `docs/V7_2_COMMANDS.md` : ce guide et la procédure matérielle.

### Fichiers modifiés

- Transport : `include/communication.h`, `include/communication_transport.h`, `include/zigbee_transport.h`, `src/zigbee_transport.cpp`, `include/core_link_codec.h`, `src/core_link_codec.cpp`.
- Runtimes : `include/core_wifi_runtime.h`, `src/core_wifi_runtime.cpp`, `include/core_zigbee_runtime.h`, `src/core_zigbee_runtime.cpp`, `include/core_main_runtime.h`, `src/core_main_runtime.cpp`, `src/main.cpp`, `src/v5_console.cpp`.
- Fiabilité : `include/message_tracker.h`, `src/message_tracker.cpp`, `include/message_deduplicator.h`, `src/message_deduplicaor.cpp`, `src/message_router.cpp`, `include/message_id_generator.h`, `src/message_id_generator.cpp`.
- Exécution : `include/lamp_hardware.h`, `src/lamp_hardware_espidf.cpp`, `include/lamp_controller.h`, `src/lamp_controller.cpp`, `src/action_executor.cpp`.
- Build/tests/docs : `platformio.ini`, `test/fakes/Arduino.h`, `test/README`, `README.md`, `docs/README.md`.

Les fichiers de l’API HTTP, du DeviceRegistry, du codec Zigbee V5 et des dépendances ne sont pas modifiés.
