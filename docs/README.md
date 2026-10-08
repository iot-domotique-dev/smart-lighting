# Documentation

Le [README du dépôt](../README.md) donne l’état du projet et les commandes courantes. Les documents ci-dessous sont les références techniques actives; chaque sujet a une seule page de détail.

**Statut au 8 octobre 2026 : V7.4.2 est validée fonctionnellement sur le montage à quatre ESP32-C6**, avec un CORE logique, un MAIN et une LAMP. Les ON/OFF et états HTTP confirmés, les transitions offline/reconnexion et l’expiration avec `execution_unknown` ont été observés sur matériel. **92 tests natifs passent et les quatre profils C6 compilent.** V7.4.1 couvre le routage de deux lampes en simulation native; le banc matériel ne contenait qu’une lampe.

## Références actives

- [V5 — Zigbee MAIN_LIGHTING ↔ LAMP_C6](V5_ZIGBEE.md) : comportement terrain et mise en service de référence.
- [V7 — CORE double ESP32-C6](V7_CORE_DOUBLE_C6.md) : rôles, UART, identités et validation matérielle.
- [V7.1 — API locale CORE-WIFI](V7_1_CORE_API.md) : routes HTTP, JSON, erreurs, sécurité et capture finale de l’inventaire en ligne.
- [V7.1.1 — Configuration Wi-Fi](V7_1_1_WIFI_SETUP.md) : credentials locaux, compilation, flash, logs et essais PowerShell.
- [V7.2 — Commandes CORE → MAIN → LAMP](V7_2_COMMANDS.md) : SET_POWER, résultats de validation matérielle, ACK d’acceptation et d’exécution, retries, déduplication, expiration et procédure reproductible.
- [V7.3 — Commande HTTP locale](V7_3_HTTP_COMMANDS.md) : authentification Bearer, soumission de SET_POWER, consultation d’état, procédure et résultats de validation matérielle.
- [V7.4 — Routage multi-lampes et état confirmé](V7_4_RELIABILITY.md) : validation native V7.4.1, résultat matériel V7.4.2, IDs des commandes et limites de cohérence.

## Tests

Les suites natives et leur périmètre sont décrits dans le [README des tests](../test/README).

Les 92 tests natifs couvrent la logique et les codecs, dont le routage en présence d’identifiants locaux identiques, le suivi d’état confirmé et les réponses HTTP additives. Ils ne remplacent pas les essais radio, UART, Wi-Fi et HTTP sur carte; le dossier V7.4 distingue les preuves matérielles rapportées et les limites qui restent.

## V7.3 — Commande HTTP locale

La validation V7.3 demeure la référence de l’API de commande locale et de ses erreurs HTTP. Son empreinte de 994 816 octets et sa marge de 53 760 octets sont historiques; la mesure V7.4.2 est dans le [dossier V7.4](V7_4_RELIABILITY.md). L’application mobile et `SET_BRIGHTNESS` / `SET_AUTOMATIC` restent à planifier.

## Historique

Les contextes V2, l’audit pré-V7 et la conception V4 sont dans [l’archive](archive/README.md). Ils sont conservés comme historique et ne décrivent pas le firmware actuel.
