# Documentation

Le [README du dépôt](../README.md) donne l’état du projet et les commandes courantes. Les documents ci-dessous sont les références techniques actives; chaque sujet a une seule page de détail.

**Statut au 7 octobre 2026 : V7 validée sur le montage à quatre ESP32-C6**, avec un CORE logique, un MAIN et une lampe. L’inventaire API est alimenté, les commandes V5 avec ACK fonctionnent et l’utilisateur rapporte **52 tests natifs réussis**. Les résultats et le blocage lié à un ancien réseau Zigbee sont consignés dans [CORE double C6](V7_CORE_DOUBLE_C6.md#validation-du-7-octobre-2026).

## Références actives

- [V5 — Zigbee MAIN_LIGHTING ↔ LAMP_C6](V5_ZIGBEE.md) : comportement terrain et mise en service de référence.
- [V7 — CORE double ESP32-C6](V7_CORE_DOUBLE_C6.md) : rôles, UART, identités et validation matérielle.
- [V7.1 — API locale CORE-WIFI](V7_1_CORE_API.md) : routes HTTP, JSON, erreurs, sécurité et capture finale de l’inventaire en ligne.
- [V7.1.1 — Configuration Wi-Fi](V7_1_1_WIFI_SETUP.md) : credentials locaux, compilation, flash, logs et essais PowerShell.

## Tests

Les suites natives et leur périmètre sont décrits dans le [README des tests](../test/README).

Les 52 tests réussis vérifient la logique et les codecs, notamment la mise à jour sans doublon et le rejet d’une identité incohérente. La radio, l’UART physique, le Wi-Fi/HTTP et les commandes de lampe ont été vérifiés séparément sur les cartes.

## Historique

Les contextes V2, l’audit pré-V7 et la conception V4 sont dans [l’archive](archive/README.md). Ils sont conservés comme historique et ne décrivent pas le firmware actuel.
