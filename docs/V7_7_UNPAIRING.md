# V7.7 — Désappairage applicatif sécurisé

## Statut

V7.7.2 ajoute au firmware un retrait applicatif d’une LAMP_C6 par transaction HTTP authentifiée. V7.7.3 ajoute le parcours correspondant dans l’application mobile en réutilisant ces routes, sans changement du firmware, d’UART, de Zigbee ou de NVS.

Le 15 octobre 2026, l’utilisateur a confirmé le parcours nominal complet sur Android et matériel réel : découverte d’une lampe, ajout et attribution d’un nom, appairage, commandes ON/OFF, retrait depuis Android, réappairage avec un nouvel ID V7 et persistance après redémarrage. Le banc de validation était composé d’un MAIN et d’une LAMP. Aucun ID de commande ou journal complet n’a été transmis pour consignation.

Les 119 tests natifs passent dans les huit environnements de test. Les quatre builds firmware complets ont réussi. Le premier build des profils Zigbee a mis en évidence une erreur C++ dans l’appel de migration NVS (`load const` appelait `save` non-const); elle a été corrigée avec un helper interne de validation/écriture, puis les profils concernés ont été recompilés.

| Profil | Résultat | Flash utilisé / partition | Marge flash | RAM statique / disponible | Marge RAM |
|---|---|---:|---:|---:|---:|
| `core_wifi_c6` | Succès | 1 022 042 / 1 048 576 octets (97,5 %) | 26 534 octets | 75 792 / 327 680 octets (23,1 %) | 251 888 octets |
| `core_zigbee_c6` | Succès | 558 356 / 1 966 080 octets (28,4 %) | 1 407 724 octets | 34 852 / 327 680 octets (10,6 %) | 292 828 octets |
| `main_light_c6` | Succès | 543 022 / 1 966 080 octets (27,6 %) | 1 423 058 octets | 34 524 / 327 680 octets (10,5 %) | 293 156 octets |
| `lamp_c6` | Succès | 545 868 / 1 966 080 octets (27,8 %) | 1 420 212 octets | 34 548 / 327 680 octets (10,5 %) | 293 132 octets |

CORE-WIFI tient dans sa partition, avec une marge modeste d’environ 2,5 %. Ne pas accroître ses fonctionnalités firmware sans recontrôler cette limite. Les 119 tests couvrent les états de retrait, les pertes/redémarrages, la migration, les IDs, les codecs et la non-régression des commandes. Il n’existe pas encore de test natif dédié exécutant le serveur HTTP V7.7; le test utilitaire Bearer existant est exécuté.

## Portée

Le retrait enlève l’association applicative entre une lampe et son MAIN, invalide sa route applicative et enlève l’équipement des inventaires CORE après confirmation persistée. Il ne fait pas quitter à la carte LAMP le PAN Zigbee. Il n’efface ni toute la NVS ni la partition Zigbee `zb_storage`.

Le système reste limité à un seul MAIN. La capacité du registre MAIN est de 10 lampes et la table de routes physiques Zigbee a une capacité de 10 entrées. Aucun changement n’a été apporté à l’application mobile, `SET_POWER`, au multi-MAIN ou aux commandes de niveau supérieur.

## Déroulement de retrait

```text
Client HTTP        CORE-WIFI        CORE-ZIGBEE          MAIN              LAMP
    | POST id/op       |                  |                 |                 |
    |---------------->| validation       |                 |                 |
    |                 |--- UART REMOVE ->|--- Zigbee ----->|                 |
    |                 |                  |                 | journal REQUESTED
    | 202 processing  |                  |                 | bloque cette lampe
    |<----------------|                  |                 |--- UNPAIR_REQ -->|
    |                 |                  |                 |                 | journal PREPARED
    |                 |                  |                 |                 | efface lamp_pairing
    |                 |                  |                 |<-- CONFIRM ------|
    |                 |                  |                 | journal SUCCESS,
    |                 |                  |                 | retire registre et route
    |                 |<-- UART résultat-|<-- Zigbee -------|--- COMMIT ------>|
    |                 | enlève l’appareil|                 |                 | efface receipt
    | GET état        |                  |                 |                 |
    |---------------->|--- QUERY ------->|---------------->|                 |
    |<----------------| résultat durable |                 |                 |
```

Les messages de retrait portent le même identifiant d’opération et les identités de la lampe : hardware ID, ID local, parent MAIN et ID CORE/V7 lorsqu’il est disponible. Les requêtes et réponses Zigbee vérifient aussi les adresses logiques source et destination. Les nouveaux types de message sont ajoutés à la fin des énumérations; la taille du message Zigbee et la version du format UART restent inchangées. Le transport UART réutilise les actions de pairing étendues.

### États

Le journal MAIN durable suit `NONE → REQUESTED → SUCCEEDED` ou `NONE → REQUESTED → FAILED`.

- `REQUESTED` est enregistré avant le premier envoi. Seule la lampe cible est marquée en cours de retrait et ses nouvelles commandes ON/OFF sont refusées. Les autres lampes continuent de fonctionner.
- La LAMP écrit d’abord un reçu `PREPARED` dans `lamp_removal`, puis efface uniquement son namespace applicatif `lamp_pairing`, puis persiste le reçu `CLEARED`. Elle ne confirme qu’après ces écritures.
- MAIN n’enlève l’identité de sa table de réservations qu’après une confirmation ou une annonce `UNPAIRING` vérifiée, avec la transaction et l’identité exactes. Il persiste alors `SUCCEEDED`, protège l’ancien ID et libère une place dans le tableau des identités actives.
- MAIN retire ensuite la lampe de son registre et oublie uniquement la route Zigbee locale associée à l’ID et au hardware ID concernés. Il envoie `UNPAIR_COMMIT`; la lampe efface son reçu lorsqu’elle reçoit ce commit valide.
- Un rejet explicite aboutit à `FAILED`, mais conserve la réservation. Une panne d’écriture, un timeout ou la perte de communication ne constitue jamais une preuve de succès; le retrait reste en attente ou incertain et la réservation n’est pas libérée.

Les réponses de suivi utilisent `processing`, `succeeded`, `failed`, `uncertain` ou `not_found`. `not_found` signifie qu’aucune entrée correspondant à cet identifiant n’est présente dans le journal MAIN actuellement conservé. Ce n’est pas une preuve que l’opération n’a jamais été transmise : MAIN ne garde qu’une dernière opération terminale; une opération ancienne peut être remplacée par une opération ultérieure.

## Reprise après perte de confirmation ou redémarrage

Si MAIN reçoit le reçu LAMP mais que CORE perd la réponse, le client consulte le même identifiant d’opération. CORE-WIFI interroge le journal persistant MAIN et peut donc reconstruire le résultat après son redémarrage, tant que MAIN conserve cette opération. Ne pas générer un nouvel identifiant simplement parce que la réponse HTTP a été perdue.

Après redémarrage de MAIN, un journal `REQUESTED` reste actif et les requêtes sont répétées. Après redémarrage LAMP, son reçu `PREPARED` est repris : la lampe achève prudemment l’effacement si l’association correspond encore exactement, puis répète confirmation et annonce jusqu’au commit. Les confirmations perdues ou dupliquées sont donc réconciliables. Une vieille transaction ne peut pas effacer une association plus récente.

Un état `uncertain` doit être traité comme « résultat à vérifier ». Il ne faut ni confirmer la suppression côté client, ni relancer avec un nouvel identifiant, ni réappairer en forçant un effacement. Réinterroger l’opération existante et vérifier les journaux CORE/MAIN/LAMP.

## Identifiants et stockage NVS

- Les IDs locaux historiques `1..10` sont protégés par un masque durable de tombstones après retrait.
- Les nouveaux IDs restent strictement croissants au-dessus de 10; aucun ID déjà attribué n’est réutilisé.
- L’identité active retirée libère une ligne du tableau MAIN. La même carte peut ensuite être appairée à nouveau avec un nouvel ID local.
- Le journal de réservation V7.6 est un blob brut version 1. La migration valide explicitement version, taille, MAIN, compte et identités, puis écrit la version 2 dans la même clé NVS `main_pairing/reservations`. La version 2 conserve les identités et le watermark et ajoute tombstones et journal de retrait. Aucun effacement général ou migration destructive n’est effectué.
- Si l’ancien blob est mal formé ou si l’écriture de migration échoue, le système échoue en mode prudent : il ne démarre pas de nouvelle attribution ni de retrait qui dépendrait d’un journal non fiable. Le blob historique n’est pas effacé.
- Le reçu LAMP vit séparément dans `lamp_removal/receipt`; les données existantes restent dans `lamp_pairing`. Le départ du PAN Zigbee n’est pas demandé.

Le journal MAIN garde une seule transaction de retrait à la fois et conserve le dernier résultat terminal. Une seule transaction en attente empêche le démarrage d’une autre. Le nombre maximal de lampes n’est pas augmenté.

## API HTTP

Toutes les routes exigent un Bearer token configuré sur CORE-WIFI.

### Démarrer une opération

```http
POST /api/v1/devices/{device_id}/removal-operations
Authorization: Bearer <token>
Content-Type: application/json

{"operation_id": 123456789}
```

`operation_id` doit être un entier non nul sur 32 bits. La route exige une lampe et son unique parent MAIN correctement enregistrés, les deux en ligne, ainsi qu’un ID CORE cohérent. Elle refuse un identifiant déjà utilisé pour une autre lampe et refuse le retrait si une commande SET_POWER de cette lampe est en attente ou si son résultat d’exécution reste inconnu.

La réponse nominale est `202 Accepted` avec `state: "processing"`. Cela signifie que CORE a envoyé la demande sur UART, pas que la lampe a été retirée. Une perte de transport peut renvoyer `503` avec `state: "uncertain"`.

### Consulter une opération

```http
GET /api/v1/removal-operations/{operation_id}
Authorization: Bearer <token>
```

Réponse :

```json
{"operation":{"id":123456789,"device_id":3559985816,"state":"processing","result_code":"processing"}}
```

CORE réinterroge MAIN au plus toutes les 2 secondes pendant un état `processing` ou `uncertain`. Une réponse `succeeded` est envoyée à CORE seulement après validation et persistance côté MAIN. Une réponse `failed` indique un rejet explicite; `uncertain` maintient le résultat ouvert; `not_found` indique l’absence dans le journal courant.

La route liste/détail des appareils continue d’exposer l’appareil jusqu’à confirmation fiable et résultat durable. Il est ensuite retiré de l’inventaire. La propriété `removalPending` n’est pas ajoutée au JSON actuel.

## Client Android — V7.7.3

L’application expose **Retirer cette lampe** depuis le détail d’une lampe en ligne. La boîte de confirmation identifie la lampe par son nom et son ID V7 et indique que le retrait ne coupe pas son alimentation. Annuler ne fait aucun appel réseau.

Avant de soumettre, le client vérifie que la lampe et son MAIN sont en ligne, que l’inventaire est frais, qu’aucune commande SET_POWER de cette lampe n’est en cours ou incertaine et qu’aucun autre retrait n’est en cours. Il crée un `operation_id` uint32 non nul à partir de `expo-crypto`, persiste l’ID, la lampe, l’adresse CORE et la phase dans `expo-secure-store`, puis envoie exactement un POST. Le Bearer token n’est pas enregistré avec cette opération.

Après `202 processing`, le client suit la même opération avec GET. Il persiste ses changements d’état et reprend le même ID après interruption ou redémarrage de l’application; il ne répète jamais le POST. `uncertain` et `not_found` restent à vérifier. En particulier, `not_found` n’est pas interprété comme la preuve que le POST n’a pas été reçu, puisque le journal MAIN ne garde pas un historique illimité. Les erreurs transitoires du suivi sont retentées par GET uniquement.

Seul `succeeded` lance une relecture de `/api/v1/devices`. La lampe n’est retirée de l’interface qu’après confirmation de son absence dans la liste. Si elle y figure encore, l’application permet une nouvelle lecture d’inventaire sans retransmettre le retrait. Le verrou empêche un second retrait global et désactive ON/OFF pour la lampe cible; le pairing reste une action manuelle distincte.

La vérification logicielle V7.7.3 avait donné 56/56 tests simulés, lint/typecheck réussis et un export Android réussi. `npx expo install --check` relève `expo@57.0.28` alors que `~57.0.29` est attendu; cet alignement de patch n’a pas été inclus. Les vérifications ont utilisé Node 20.19.4, alors que le projet exige Node 22.13 ou plus récent. Le mode d’installation de l’application sur le téléphone n’a pas été consigné. Voir le bilan matériel ci-dessous et la procédure du [README mobile](../apps/mobile/README.md).

### Validation matérielle V7.7 — 15 octobre 2026

L’utilisateur a confirmé sur son téléphone Android et sur son montage réel les étapes suivantes :

- recherche et découverte d’une lampe;
- ajout et attribution d’un nom, puis appairage;
- commande ON et OFF;
- retrait de la lampe depuis l’application;
- réappairage de la même carte avec un nouvel identifiant V7;
- conservation de l’association après redémarrage.

Ce résultat confirme le parcours nominal de bout en bout avec un MAIN et une LAMP. Il ne démontre pas la tenue sous charge ou en présence de plusieurs équipements simultanés. Les tests firmware 119/119, les tests API mobiles simulés 56/56, le lint, le typecheck et l’export Android étaient déjà passés; ils ne remplacent pas les observations physiques ci-dessus.

Les informations communiquées ne comprennent pas les IDs de commande/opération, des captures d’écran ni les journaux série complets. Aucun résultat ne doit donc être déduit sur les temps de convergence, les nombres de retry ou le comportement exact des états intermédiaires.

### Limites restantes de validation

N’ont pas été rapportés comme testés : plusieurs lampes actives simultanément derrière le même MAIN; retraits ou commandes concurrents; perte Wi-Fi/UART/Zigbee; perte d’un ACK pendant le retrait; arrêt ou redémarrage CORE, MAIN ou LAMP au milieu d’une transaction; reprise de cette transaction après coupure; erreur SecureStore; indisponibilité prolongée de l’API. Ces cas restent à valider séparément avant d’en revendiquer la robustesse.

## Garanties et limites de sécurité

La lampe refuse une demande si l’adresse source n’est pas son MAIN enregistré, si la destination ne correspond pas à son ID local, ou si hardware ID, ID local, parent ou transaction diffèrent de son association actuelle. MAIN vérifie la source LAMP, la destination MAIN, la transaction et les identités enregistrées avant de libérer la réservation. CORE vérifie la cohérence du parent et de l’ID V7 avant de retirer l’entrée de son inventaire. Le retrait d’une lampe n’efface pas les entrées des autres lampes.

La validation des champs et endpoints n’est pas une signature cryptographique des messages radio. Le protocole suppose un PAN Zigbee de confiance et les protections de transport existantes. Le Bearer token protège l’API locale; le CORE ne doit pas être exposé sur Internet. Le système ne fournit pas de preuve d’effacement physique ni de départ du réseau Zigbee.

## Validation logicielle

Les tests ajoutés couvrent le retrait transactionnel et hors ligne, la conservation d’une seconde lampe, le contrôle des identités/adresses/transaction, les confirmations dupliquées, la perte de confirmation puis les redémarrages MAIN/LAMP, l’échec d’effacement LAMP et d’écriture MAIN, la migration V7.6, les tombstones des IDs historiques, l’annonce tardive, le réappairage avec un nouvel ID, les codecs UART/Zigbee et le garde-fou SET_POWER ciblé par lampe. La suite complète compte 119 tests natifs réussis.

Limite des tests : le serveur HTTP V7.7 n’est pas exécuté dans un test natif d’intégration. Le contrôle Bearer commun et les validations de pairing existantes sont testés. Le parcours matériel nominal rapporté exerce le retrait avec un MAIN et une LAMP; il ne constitue pas un test d’intégration exhaustif des réponses HTTP ou de tous les chemins UART en situation de panne.

La compilation seule ne valide pas l’échange radio, la persistance NVS réelle, l’effacement du namespace sur carte ou la reprise après une coupure matérielle. Le parcours nominal V7.7.2/V7.7.3 est rapporté validé sur Android et cartes; aucun ID d’opération ni journal série complet n’a été consigné. Les coupures, pertes de confirmation, commandes concurrentes et reprises après redémarrage au milieu d’une transaction restent à vérifier séparément.

## Protocole matériel de référence et essais complémentaires

Ne pas effacer les cartes ni la NVS existante pour ces essais. Les étapes 1 à 7 décrivent le parcours nominal rapporté comme validé; les rejouer seulement si des preuves détaillées sont nécessaires. Le scénario 8 est un essai de résilience supplémentaire non validé. L’export JavaScript seul ne prouve pas le comportement de reprise après fermeture forcée ou coupure en cours de transaction. Avant de nouveaux essais, sauvegarder les journaux série et relever les IDs CORE/V7, locaux et hardware IDs sans enregistrer le Bearer token.

1. Vérifier `GET /api/v1/devices` et confirmer la présence de la lampe cible et d’une autre lampe si disponible.
2. Envoyer un `POST` pour la cible avec un nouvel `operation_id`; conserver cet ID et la réponse sans enregistrer le Bearer token.
3. Interroger le `GET` de la même opération jusqu’à `succeeded`, `failed` ou `uncertain`. Ne pas confondre `202 processing` avec un retrait achevé.
4. Pour `succeeded`, vérifier que seule la cible a disparu de `GET /devices`, que l’autre lampe reste visible et que MAIN a signalé la persistance avant son résultat.
5. Renvoyer la même requête avec le même `operation_id`; vérifier l’idempotence. Une nouvelle tentative doit utiliser un nouvel ID uniquement après une issue terminale vérifiée.
6. Vérifier qu’une commande ON/OFF vers la lampe retirée est rejetée et que les autres lampes répondent toujours.
7. Appairer à nouveau la même carte par le parcours V7.6; vérifier qu’elle reçoit un nouvel ID local supérieur à 10 et que l’annonce tardive de l’ancien ID n’est pas restaurée.
8. En banc contrôlé, interrompre les retours pendant une transaction puis redémarrer CORE, MAIN ou LAMP séparément pour vérifier la réconciliation. Ne pas provoquer ces coupures sur l’installation utile avant que le test nominal soit conclu.

Si l’API retourne `uncertain`, conserver la transaction, ne pas effacer NVS et recueillir les journaux des quatre rôles avant toute autre opération.
