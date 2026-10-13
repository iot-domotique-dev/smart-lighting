# V7.6 — Appairage contrôlé des lampes

## Périmètre et statut

V7.6.1 audite le provisioning V5, V7.6.2 réserve durablement les IDs locaux, V7.6.3 ajoute le pilotage du pairing depuis CORE-WIFI et V7.6.4 fournit le parcours dans l’application mobile. Le parcours réutilise `MainProvisioning`, les confirmations V5, le registre CORE existant et les transports UART/Zigbee déjà en place.

**Le 13 octobre 2026, l’utilisateur a confirmé la réussite de l’ajout d’une lampe depuis l’application sur son téléphone.** L’essai concernait un seul MAIN_LIGHTING et une LAMP_C6. Les détails d’identification et les journaux de l’opération n’ont pas été communiqués.

## Validation logicielle V7.6.3

La suite native complète passe **110/110 tests**. Les profils PlatformIO `core_wifi_c6`, `core_zigbee_c6`, `main_light_c6` et `lamp_c6` ont tous été compilés intégralement; les quatre éditions de liens et créations d’images ont réussi. Aucun firmware n’a été flashé et le nouveau parcours n’est donc pas validé sur matériel.

| Profil | RAM statique utilisée | Taille appli PlatformIO | Image `.bin` | Marge dans la partition appli |
|---|---:|---:|---:|---:|
| `core_wifi_c6` | 75 236 / 327 680 octets (23,0 %) | 1 009 378 / 1 048 576 octets (96,3 %) | 1 009 760 octets | 38 816 octets |
| `core_zigbee_c6` | 34 268 / 327 680 octets (10,5 %) | 551 966 / 1 966 080 octets (28,1 %) | 552 352 octets | 1 413 728 octets |
| `main_light_c6` | 33 940 / 327 680 octets (10,4 %) | 530 340 / 1 966 080 octets (27,0 %) | 530 720 octets | 1 435 360 octets |
| `lamp_c6` | 33 948 / 327 680 octets (10,4 %) | 533 066 / 1 966 080 octets (27,1 %) | 533 456 octets | 1 432 624 octets |

CORE-WIFI a la marge flash la plus faible. Toute évolution HTTP future devra contrôler à nouveau la taille de son image avant déploiement.

Le périmètre comprend un CORE-WIFI, son CORE-ZIGBEE associé et un seul MAIN_LIGHTING. Il ne comprend ni suppression/désappairage, ni multi-MAIN, ni cloud, ni modification du protocole MAIN↔LAMP. Les commandes ON/OFF et `last_confirmed_state` restent inchangés.

## Fonctionnement

```text
Client mobile
    │ HTTP local + Bearer
    ▼
CORE-WIFI ─ UART ─ CORE-ZIGBEE ─ Zigbee ─ MAIN_LIGHTING
                                            │
                               fenêtre radio temporaire
                                            │
                                            ▼
                                         LAMP_C6
```

Le MAIN ouvre la fenêtre d’admission Zigbee à la demande et ferme celle-ci explicitement ou à son expiration. La fenêtre de démarrage de 180 secondes déjà configurée avant V7.6.3 reste inchangée. Une fermeture radio dont le succès n’est pas confirmé reste `uncertain`; le MAIN conserve l’identifiant de session pour permettre une nouvelle demande de fermeture. Une session n’est pas une association applicative V5.

Le MAIN transmet les annonces `UNPAIRED` à CORE-WIFI comme candidates temporaires. Elles ne sont pas ajoutées à `DeviceRegistry`, ne reçoivent pas d’ID V7 actif et ne figurent pas dans `/api/v1/devices`. Les annonces répétées rafraîchissent la candidate. Une candidate disparaît après 30 secondes sans annonce ou après fermeture/expiration de la session.

Le client choisit explicitement son `hardware_id` et fournit un nom. MAIN réutilise `PAIR_REQUEST`, `PAIR_ACCEPT`, les réservations V7.6.2 et la validation stricte de `PAIR_CONFIRM`. L’état d’opération ne devient `succeeded` qu’après validation de cette confirmation par MAIN; l’envoi de `PAIR_REQUEST`/`PAIR_ACCEPT` n’est pas un succès.

Les IDs locaux historiques 1 à 10 et les réservations existantes ne sont pas modifiés. Les prochaines attributions continuent à partir du high-watermark persistant V7.6.2. Aucun stockage NVS n’est ajouté par V7.6.3.

## Authentification et règles d’appel

Toutes les routes ci-dessous exigent `Authorization: Bearer <token>`, y compris les lectures de session, de candidates et d’opération. Le token configuré doit avoir entre 32 et 128 caractères. Les requêtes JSON utilisent `Content-Type: application/json`.

Les erreurs gardent la forme existante :

```json
{"error":{"code":"unauthorized","message":"A valid Bearer token is required."}}
```

Ne publier le port HTTP du CORE ni sur Internet ni par redirection de port. Ce service reste HTTP non chiffré sur le réseau local de confiance.

## Routes HTTP

### Ouvrir une session

`POST /api/v1/pairing/session`

```json
{"duration_seconds":60}
```

La durée doit être comprise entre 15 et 180 secondes. CORE-WIFI génère un `id` non nul et répond `202 Accepted` pendant que le MAIN ouvre la radio :

```json
{"session":{"id":123456789,"state":"opening","duration_seconds":60}}
```

L’état passe à `active` après confirmation du MAIN. Un seul MAIN et une seule session sont acceptés. Une session déjà active, une opération en cours ou un MAIN absent/hors ligne donne une erreur HTTP explicite.

### Lire ou fermer la session

- `GET /api/v1/pairing/session` retourne `{"session":null}` si CORE-WIFI n’a pas de session en mémoire. Sinon, l’objet expose `id`, `state`, `duration_seconds`, `candidate_count` et `candidate_limit_reached`.
- `DELETE /api/v1/pairing/session` demande la fermeture radio et répond `202 Accepted` avec `state: "closing"`. La réponse finale devient `closed`, `expired` ou `uncertain` dans le GET. La fermeture est refusée pendant une opération de pairing en cours.

États de session : `opening`, `active`, `closing`, `closed`, `expired`, `failed`, `uncertain`. Après redémarrage du CORE-WIFI, l’état RAM est vide; le client doit lire la session avant d’en créer une nouvelle. Si la session est `uncertain`, demander sa fermeture et attendre le résultat au lieu d’ouvrir une autre session.

### Lire les candidates

`GET /api/v1/pairing/candidates`

```json
{
  "session_id":123456789,
  "capacity_reached":false,
  "candidates":[
    {"hardware_id":"C6-0123456789ABCDEF","capabilities":0,"last_seen_ms":12345}
  ],
  "count":1
}
```

`last_seen_ms` est une valeur de temps écoulé du firmware, pas une date UTC. Il y a au plus 10 candidates. Une annonce répétée actualise l’entrée existante.

### Demander le pairing d’une candidate

`POST /api/v1/pairing/operations`

```json
{
  "session_id":123456789,
  "operation_id":987654321,
  "hardware_id":"C6-0123456789ABCDEF",
  "name":"Lampe bureau"
}
```

Le client fournit un `operation_id` non nul sur 32 bits afin de pouvoir retrouver la requête après une réponse HTTP perdue. Le nom doit contenir de 1 à 31 caractères ASCII parmi lettres, chiffres, espace, `_`, `-`, `.`, `'`. `hardware_id` doit être un identifiant C6 `C6-` suivi de 16 chiffres hexadécimaux.

Une nouvelle opération répond `202 Accepted` avec `state: "pending"`. Une répétition identique avec le même ID retourne l’opération déjà retenue en mémoire et ne renvoie pas une seconde demande au MAIN. Une réutilisation du même ID pour une autre requête donne `409 operation_id_conflict`. Le client mobile ne doit pas relancer automatiquement le POST après une coupure; il doit d’abord consulter l’ID d’opération.

### Lire le résultat

`GET /api/v1/pairing/operations/{operation_id}`

```json
{
  "operation":{
    "id":987654321,
    "session_id":123456789,
    "hardware_id":"C6-0123456789ABCDEF",
    "name":"Lampe bureau",
    "state":"succeeded",
    "result_code":"succeeded",
    "local_device_id":11
  }
}
```

États d’opération : `pending`, `processing`, `succeeded`, `failed`, `rejected`, `expired`, `uncertain`. `result_code` précise notamment `capacity`, `route_unavailable`, `not_found`, `busy`, `failed`, `rejected`, `expired` ou `uncertain`. Les IDs, candidates et opérations sont conservés uniquement en RAM CORE-WIFI; une opération inconnue ou remplacée par une opération suivante répond `404 operation_not_found`. L’application doit rafraîchir `/api/v1/devices` après le succès avant d’afficher la lampe dans l’inventaire opérationnel.

## Capacités et erreurs

Les limites restent celles du firmware : 10 lampes dans le registre MAIN, 10 candidates simultanées, 10 réservations persistantes d’identités et 10 routes Zigbee au total. La table de routes est partagée avec les autres échanges Zigbee. Une candidate sans route radio exploitable reçoit `route_unavailable`; les limites d’équipements ou de réservations donnent `capacity`. Aucune table et aucun identifiant existants ne sont agrandis par V7.6.3.

Autres erreurs attendues : `400 bad_request` (JSON, IDs ou nom invalides), `401 unauthorized`, `404 candidate_not_found` / `operation_not_found`, `409 session_not_active` / `pairing_in_progress` / `main_offline`, `413 payload_too_large`, `415 unsupported_media_type`, `503 pairing_transport_uncertain`.

Une interruption entre CORE-WIFI, CORE-ZIGBEE et MAIN peut laisser l’opération `uncertain`. Ce statut ne signifie ni échec ni succès. Après redémarrage de CORE-WIFI, l’état en RAM est perdu et aucune opération n’est automatiquement renvoyée. Le résultat durable du pairing reste l’association V5/NVS et l’annonce ultérieure de la lampe dans `/api/v1/devices`.

## Compatibilité et firmwares

Le protocole UART CORE-WIFI↔CORE-ZIGBEE reçoit un nouveau type de message de pairing dans sa trame existante; la taille maximale de charge utile passe à 80 octets. Le format radio MAIN↔LAMP, les commandes `SET_POWER`, les réponses d’exécution et la table de réservations V7.6.2 ne sont pas réécrits.

Pour utiliser les nouvelles routes, mettre à jour ensemble CORE-WIFI, CORE-ZIGBEE et MAIN_LIGHTING. LAMP_C6 conserve son firmware : le parcours V5 et ses messages radio ne changent pas. Aucune carte n’a été flashée dans le cadre du développement logiciel V7.6.3.

## V7.6.4 — ajout depuis l’application mobile

L’application Expo SDK 57 propose un parcours mobile qui ouvre explicitement une session, sélectionne une candidate par `hardware_id`, valide son nom, demande confirmation avant le POST de pairing, suit l’opération jusqu’à un état final et relit `/api/v1/devices` après succès. Elle ferme la session par DELETE puis attend la confirmation GET. Toutes les routes de pairing utilisent le Bearer token déjà conservé dans SecureStore. Aucune route ni aucun firmware ne sont modifiés par V7.6.4.

Le typage, le lint, les **34/34 tests simulés**, `expo install --check` et l’export du bundle Android réussissent. La campagne automatisée avait utilisé Node 20.19, inférieur au minimum 22.13 requis, et npm avait signalé `EBADENGINE`; cette réserve concerne la reproductibilité des vérifications logicielles.

### Résultat matériel rapporté — 13 octobre 2026

L’utilisateur confirme avoir ajouté une LAMP_C6 depuis l’application mobile sur son téléphone après avoir rendu la lampe disponible pour un nouvel appairage. Cela valide le parcours nominal d’ajout depuis l’interface sur le matériel utilisé, avec un MAIN et une lampe. Le système a donc accepté la candidate et l’opération de pairing a abouti selon le retour utilisateur.

Aucun `hardware_id`, `operation_id`, ID local attribué, capture d’écran ou journal série n’a été fourni pour cet essai; ces valeurs ne sont pas consignées. L’essai ne valide pas le retrait d’une lampe depuis l’application (fonctionnalité absente), l’utilisation iOS, le multi-MAIN, ni le pairing physique simultané de plusieurs lampes. Les identifiants d’opération restent en mémoire et peuvent être perdus si Android arrête complètement l’application. Le parcours détaillé est dans le [README mobile](../apps/mobile/README.md).

## Références

- [V7.5 — Application mobile existante](V7_5_MOBILE_APP.md)
- [V7.4 — routage multi-lampes et état confirmé](V7_4_RELIABILITY.md)
- [V5 — provisioning Zigbee](V5_ZIGBEE.md)
- [README du dépôt](../README.md)
