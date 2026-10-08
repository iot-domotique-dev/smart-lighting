# Documentation

Le [README du dépôt](../README.md) donne l’état du projet et les commandes courantes. Les documents ci-dessous sont les références techniques actives; chaque sujet a une seule page de détail.

**Statut au 7 octobre 2026 : V7.2 est validée fonctionnellement sur le montage à quatre ESP32-C6**, avec un CORE logique, un MAIN et une lampe. L’inventaire API est alimenté; SET_POWER, les ACK, le retry, la déduplication et l’expiration ont été vérifiés sur matériel. **85 tests natifs passent** et les quatre profils matériels compilent. Les résultats V7.1 et le problème d’ancien réseau Zigbee sont consignés dans [CORE double C6](V7_CORE_DOUBLE_C6.md#validation-du-7-octobre-2026), et les essais V7.2 dans le [dossier V7.2](V7_2_COMMANDS.md).

La validation fonctionnelle V7.2 est terminée. Il reste des essais complémentaires d’endurance et de coexistence radio; ils ne remettent pas en cause les essais fonctionnels documentés.

## Références actives

- [V5 — Zigbee MAIN_LIGHTING ↔ LAMP_C6](V5_ZIGBEE.md) : comportement terrain et mise en service de référence.
- [V7 — CORE double ESP32-C6](V7_CORE_DOUBLE_C6.md) : rôles, UART, identités et validation matérielle.
- [V7.1 — API locale CORE-WIFI](V7_1_CORE_API.md) : routes HTTP, JSON, erreurs, sécurité et capture finale de l’inventaire en ligne.
- [V7.1.1 — Configuration Wi-Fi](V7_1_1_WIFI_SETUP.md) : credentials locaux, compilation, flash, logs et essais PowerShell.
- [V7.2 — Commandes CORE → MAIN → LAMP](V7_2_COMMANDS.md) : SET_POWER, résultats de validation matérielle, ACK d’acceptation et d’exécution, retries, déduplication, expiration et procédure reproductible.
- [V7.3 — Commande HTTP locale](V7_3_HTTP_COMMANDS.md) : authentification Bearer, soumission de SET_POWER, consultation d’état, procédure et résultats de validation matérielle.

## Tests

Les suites natives et leur périmètre sont décrits dans le [README des tests](../test/README).

Les 85 tests natifs couvrent la logique et les codecs, dont le routage en présence d’identifiants locaux identiques. La radio, l’UART physique, le Wi-Fi/HTTP et le trajet de commande jusqu’à la lampe ont aussi été vérifiés sur les cartes; le dossier V7.2 distingue les essais accompagnés de logs des résultats confirmés par l’utilisateur.

## V7.3 — Commande HTTP locale

La validation fonctionnelle V7.3 est confirmée par l’utilisateur. ON/OFF, le suivi terminal, les refus d’accès (`401`), les corps invalides (`400`), les lampes inconnues (`404`) et les cibles offline (`409`) sont documentés avec leurs réponses; l’utilisateur confirme globalement les essais de reprise et d’expiration. L’image CORE-WIFI laisse 53 760 octets dans la partition. Voir la [procédure et les résultats V7.3](V7_3_HTTP_COMMANDS.md). L’application mobile et `SET_BRIGHTNESS` / `SET_AUTOMATIC` restent à planifier.

## Historique

Les contextes V2, l’audit pré-V7 et la conception V4 sont dans [l’archive](archive/README.md). Ils sont conservés comme historique et ne décrivent pas le firmware actuel.
