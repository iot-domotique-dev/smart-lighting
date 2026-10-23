# Documentation

**V7.7.2/V7.7.3 — clôture nominale, 15 octobre 2026 :** l’utilisateur confirme sur Android et matériel réel la découverte, le nommage, l’appairage, ON/OFF, le retrait depuis l’application, le réappairage avec un nouvel ID V7 et la persistance après redémarrage. Ce résultat couvre un MAIN et une LAMP. Les 119 tests firmware natifs et 56 tests mobiles simulés, lint, typage et export Android avaient réussi. Les scénarios de panne, concurrence et plusieurs lampes simultanées restent hors validation. Voir [le protocole, la persistance, les routes, le client mobile et les limites V7.7](V7_7_UNPAIRING.md).

**V7.8.0 — validation fonctionnelle sur matériel, 16 octobre 2026 :** l’utilisateur confirme le réglage de luminosité depuis Android jusqu’à une LED physique sur GPIO18, via HTTP, UART et Zigbee. Le profil unique `lamp_c6` utilise LEDC et conserve ON/OFF. L’essai porte sur un MAIN et une LAMP. La persistance du dernier niveau non nul a été ajoutée ensuite en V7.9.2. Le multi-lampes simultané et les scénarios de panne/récupération ne sont pas validés. L’appairage, le retrait, le réappairage et ON/OFF restent couverts par la validation matérielle nominale V7.7 séparée. Voir [le dossier V7.8 et le complément V7.9.2](V7_8_BRIGHTNESS.md#v792--persistance-du-dernier-niveau).

**V7.9.2 — persistance du dernier niveau LAMP, validée sur matériel le 19 octobre 2026 :** l’association Zigbee a été conservée; 35 % a été appliqué; au redémarrage de LAMP la LED est restée éteinte, puis une commande ON a restauré 35 %, sans réappairage. Les essais de panne pendant une écriture NVS, multi-lampes simultané et mesure électrique restent non couverts. Voir [le bilan V7.9.2](V7_8_BRIGHTNESS.md#v792--persistance-du-dernier-niveau).

**V7.9.5 — OFF prioritaire et retrait sûr pendant un fade :** 157/157 tests natifs passent; les 72 tests mobiles simulés, TypeScript, lint, l’export Android Expo 57 et les quatre builds firmware passent aussi. OFF interrompt un fade actif; le retrait force la sortie à LOW avant le reçu persistant. L’ordre réel reste incertain quand les commandes se chevauchent. Les cas V7.9.5 n’ont pas encore été validés sur matériel; la transaction `UNPAIR_REQUEST` pendant un fade n’est pas exercée par les tests natifs. Voir [le dossier V7.9.5](V7_9_5_OFF_PRIORITY.md).

**V7.10.2 — groupes locaux persistants :** les 174 tests natifs passent et les quatre profils affectés compilent intégralement. Les groupes résident sur MAIN dans un espace NVS versionné et séparé des réservations de pairing. Cette étape n’ajoute pas d’API, d’écran Android ou de commande collective. Les écritures NVS réelles et le comportement matériel restent à valider. Voir [le dossier V7.10.2](V7_10_2_GROUPS.md).

**V7.10.3.1 — configuration et persistance des groupes validées sur matériel en 13/13 essais conformes.** Le contrôle collectif ON/OFF a ensuite aussi été confirmé sur matériel par l’utilisateur; le nombre de lampes utilisées simultanément n’est pas connu. Les scénarios de panne et de concurrence restent à valider. Voir [V7.10.3](V7_10_3_GROUPS_ANDROID.md) et [V7.10.4](V7_10_4_GROUP_POWER.md).

Le [README du dépôt](../README.md) donne l’état du projet et les commandes courantes. Les documents ci-dessous sont les références techniques actives; chaque sujet a une seule page de détail.

**Bilan matériel V7.4.2 du 9 octobre 2026 :** cette version a été validée fonctionnellement sur le montage à quatre ESP32-C6, avec un CORE logique, un MAIN et une LAMP. Les ON/OFF et états HTTP confirmés, les transitions offline/reconnexion et l’expiration avec `execution_unknown` ont été observés sur matériel. À cette étape, 92 tests natifs passaient et les quatre profils C6 compilaient. V7.4.1 couvre le routage de deux lampes en simulation native; le banc matériel ne contenait qu’une lampe. V7.5.1 et V7.5.2 sont documentées, avec un premier essai nominal de commande depuis Android rapporté par l’utilisateur.

## Références actives

- [V5 — Zigbee MAIN_LIGHTING ↔ LAMP_C6](V5_ZIGBEE.md) : comportement terrain et mise en service de référence.
- [V7 — CORE double ESP32-C6](V7_CORE_DOUBLE_C6.md) : rôles, UART, identités et validation matérielle.
- [V7.1 — API locale CORE-WIFI](V7_1_CORE_API.md) : routes HTTP, JSON, erreurs, sécurité et capture finale de l’inventaire en ligne.
- [V7.1.1 — Configuration Wi-Fi](V7_1_1_WIFI_SETUP.md) : credentials locaux, compilation, flash, logs et essais PowerShell.
- [V7.2 — Commandes CORE → MAIN → LAMP](V7_2_COMMANDS.md) : SET_POWER, résultats de validation matérielle, ACK d’acceptation et d’exécution, retries, déduplication, expiration et procédure reproductible.
- [V7.3 — Commande HTTP locale](V7_3_HTTP_COMMANDS.md) : authentification Bearer, soumission de SET_POWER, consultation d’état, procédure et résultats de validation matérielle.
- [V7.4 — Routage multi-lampes et état confirmé](V7_4_RELIABILITY.md) : validation native V7.4.1, résultat matériel V7.4.2, IDs des commandes et limites de cohérence.
- [V7.5 — Application mobile](V7_5_MOBILE_APP.md) : consultation, commandes ON/OFF, validation logicielle et premier essai nominal sur sortie matérielle depuis Android.
- [V7.6 — Appairage contrôlé des lampes](V7_6_PAIRING.md) : réservation V7.6.2, sessions pilotées par HTTP, candidates temporaires, ajout depuis l’application V7.6.4, correctif de routage et résultat matériel rapporté.
- [V7.7 — Désappairage sécurisé](V7_7_UNPAIRING.md) : transaction persistante et réconciliable, migration additive du journal MAIN, routes HTTP, client Android V7.7.3, validation nominale et essais restant à faire.

- [V7.8.0, V7.9.2, V7.9.4 et V7.9.5 — variation, persistance, transitions et OFF prioritaire](V7_8_BRIGHTNESS.md) : API brightness, conservation NVS, fades LEDC, interruption prioritaire et retrait sûr.
- [V7.9.5 — OFF prioritaire et retrait sécurisé pendant un fade](V7_9_5_OFF_PRIORITY.md) : résultats d’interruption, déduplication, séquencement incertain, retrait et limites de validation.
- [V7.10.2 — Groupes persistants sur MAIN_LIGHTING](V7_10_2_GROUPS.md) : modèle, limites, révisions, sauvegarde/restauration NVS, nettoyage lors du retrait et fonctionnalités encore exclues.
- [V7.10.3 — Configuration des groupes depuis Android](V7_10_3_GROUPS_ANDROID.md) : routes HTTP, protocole borné, UI Android, validations logicielles et limites matérielles.

## Tests

Les suites natives et leur périmètre sont décrits dans le [README des tests](../test/README).

Les tests natifs couvrent la logique d’appairage/retrait, les codecs et l’API CORE existante. Les routes HTTP V7.7 de retrait ne disposent pas d’un test de serveur HTTP natif dédié; l’authentification Bearer commune est couverte par ses tests utilitaires. Les tests natifs ne remplacent pas les essais radio, UART, Wi-Fi et HTTP sur carte.

## V7.3 — Commande HTTP locale

La validation V7.3 demeure la référence de l’API de commande locale et de ses erreurs HTTP. Son empreinte de 994 816 octets et sa marge de 53 760 octets sont historiques; la mesure V7.4.2 est dans le [dossier V7.4](V7_4_RELIABILITY.md). L’application mobile est décrite dans le [dossier V7.5](V7_5_MOBILE_APP.md). V7.8.0 ajoute `SET_BRIGHTNESS`; V7.9.2 conserve le dernier niveau non nul en NVS LAMP et a été validée sur le parcours de redémarrage décrit ci-dessus. Le profil `lamp_c6` utilise LEDC pour le prototype LED GPIO18 basse tension. `SET_AUTOMATIC` reste à planifier.

## Historique

Les contextes V2, l’audit pré-V7 et la conception V4 sont dans [l’archive](archive/README.md). Ils sont conservés comme historique et ne décrivent pas le firmware actuel.

## V7.10.4 et V7.10.4.1 — commandes collectives et suivi

- [V7.10.4 — commandes collectives ON/OFF des groupes](V7_10_4_GROUP_POWER.md) : fanout MAIN, résultats individuels, idempotence et limites matérielles.
- V7.10.4.1 sépare les lectures GET des 16 entrées de changements CORE-WIFI, garde un contexte transitoire borné pour une lecture asynchrone et conserve la rétention/idempotence des mutations et commandes. Les neuf suites natives passent à 183/183; le handler HTTP de production est exercé dans la simulation native. CORE-WIFI compile avec 11 368 octets de marge flash et 106 308 octets de RAM statique utilisée.
- Le 22 octobre 2026, sur un groupe avec une lampe, l’utilisateur a validé 20 GET liste et 20 GET détail, puis un POST collectif OFF confirmé. Quatorze POST OFF supplémentaires ont abouti en `completed`; le POST suivant a reçu le HTTP 409 `group_operation_history_full` attendu à la limite du journal global. Les essais multi-lampes, de panne et de concurrence ne sont pas couverts. Détails des IDs et des limites dans [le dossier V7.10.4](V7_10_4_GROUP_POWER.md).

## V7.10.4.2 — fiabilisation du journal collectif

- Le journal passe de 16 à 32 entrées sur CORE-WIFI et MAIN. Les résultats terminaux connus sont protégés au moins deux minutes après confirmation, puis réutilisables sous pression; les opérations incertaines gardent leur corrélation pour les ACK tardifs. Les GET liste/détail restent transitoires.
- La suite native passe à 187/187, dont 22 tests groupes. `core_wifi_c6` et `main_light_c6` compilent; CORE-WIFI garde 11 074 octets de marge flash et MAIN 1 404 892 octets. La validation matérielle V7.10.4.2 reste à faire. Voir [le dossier V7.10.4](V7_10_4_GROUP_POWER.md).
