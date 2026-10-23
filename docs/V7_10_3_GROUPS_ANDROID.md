# V7.10.3 — configuration des groupes depuis Android

## Périmètre

V7.10.3 ajoute la consultation et la configuration des groupes existants de MAIN_LIGHTING depuis l’application Android. MAIN conserve la source de vérité et persiste chaque mutation dans l’espace NVS `main_groups` décrit dans le [dossier V7.10.2](V7_10_2_GROUPS.md).

Le parcours couvre un CORE-WIFI, un CORE-ZIGBEE, un MAIN et les lampes appairées à ce MAIN. L’application ne pilote pas les groupes comme une unité : aucune commande collective n’est ajoutée. Elle permet de créer/supprimer un groupe, le renommer et ajouter/retirer ses lampes.

## Routes HTTP

Toutes les routes utilisent le Bearer token déjà configuré pour le CORE. Elles sont limitées à un MAIN en ligne qui annonce la capacité `groups`.

| Route | Fonction |
| --- | --- |
| `GET /api/v1/modules/{main_id}/groups` | Lire la liste complète des groupes |
| `POST /api/v1/modules/{main_id}/groups` | Créer un groupe |
| `GET /api/v1/modules/{main_id}/groups/{group_id}` | Lire un groupe |
| `PATCH /api/v1/modules/{main_id}/groups/{group_id}` | Renommer ou modifier un membre |
| `DELETE /api/v1/modules/{main_id}/groups/{group_id}` | Supprimer un groupe |
| `GET /api/v1/group-operations/{operation_id}` | Lire l’état d’une mutation |

Les IDs fournis ou retournés par HTTP sont des IDs V7. CORE-WIFI vérifie que chaque `lamp_id` appartient au `main_id` demandé, puis transmet au MAIN son ID local. La réponse convertit les IDs locaux persistés en IDs V7. MAIN revérifie le rattachement, l’état d’appairage et l’absence de retrait en cours avant d’ajouter une lampe.

Exemples de corps JSON :

```json
{"operation_id": 123456, "name": "Salon"}
```

```json
{"operation_id": 123457, "expected_revision": 1, "name": "Séjour"}
```

```json
{"operation_id": 123458, "expected_revision": 1, "member_action": "add", "lamp_id": 3559985816}
```

```json
{"operation_id": 123459, "expected_revision": 2, "member_action": "remove", "lamp_id": 3559985816}
```

Les corps sont bornés et n’acceptent que les champs attendus. `operation_id` est un identifiant non nul sur 32 bits. Une mutation répond normalement `202` et l’application interroge ensuite la route de suivi par GET. Les changements de groupe portent `expected_revision` : une révision dépassée renvoie `409 revision_conflict` et l’écran relit le groupe avant de permettre une nouvelle action.

Une réponse `persisted` signifie que MAIN a confirmé l’écriture avant d’appliquer le nouvel état en RAM. `sent` ou `accepted` ne signifient pas que la modification est enregistrée. Si la réponse HTTP ou le suivi est perdu, l’application n’envoie pas un second POST automatiquement; elle consulte le même `operation_id` ou signale que le résultat doit être vérifié.

La liste et le détail de groupe ne sont renvoyés qu’après réception et validation du snapshot complet provenant de MAIN. Les lectures peuvent prendre plusieurs secondes. Le tracker CORE est en RAM et limité à quatre opérations récentes; une opération inconnue/évincée ne prouve pas que la mutation n’a pas eu lieu.

## Application Android

Depuis l’inventaire des lampes, le bouton **Groupes** apparaît lorsqu’un seul MAIN en ligne annonce `groups`. L’écran permet de créer un groupe, modifier son nom, gérer les membres par ID V7 et supprimer le groupe. Il n’affiche que les lampes enregistrées sous le MAIN sélectionné. Un verrou évite deux mutations simultanées depuis cet écran.

Le jeton reste dans le mécanisme d’authentification existant et n’est pas intégré au code ou à une configuration distribuée. Une perte de réponse n’entraîne jamais le renvoi automatique d’une mutation. Une modification incertaine bloque les nouvelles mutations de l’écran tant que le résultat n’est pas résolu.

## Capacités et compatibilité

- 10 groupes maximum par MAIN et 10 lampes maximum par groupe, sans changement des capacités V7.10.2.
- Les noms acceptent 1 à 31 caractères imprimables selon les validations communes du firmware et de l’application.
- Un seul MAIN est pris en charge dans cette version de l’application.
- Aucun contrôle collectif, scène, automatisation ou évolution de `SET_POWER`/`SET_BRIGHTNESS` n’est ajouté.
- Le protocole de groupe est versionné et borné; le format UART reste dans la limite de 80 octets de payload. Les trames de groupe Zigbee ont un discriminant distinct des trames historiques afin que les IDs V7 de messages classiques ne soient pas interprétés comme des trames de groupe.
- Les protocoles historiques de commande, pairing, retrait et réservations NVS gardent leur comportement. Aucune partition ni aucun namespace NVS existant n’est modifié.

## Validation logicielle V7.10.3

Les neuf suites natives exécutées directement après leur compilation passent : **177/177**. Elles couvrent les groupes persistants, les trames UART/Zigbee bornées et les parcours de commandes historiques. Une régression avait d’abord révélé qu’un ID de message historique pouvait être confondu avec le marqueur de groupe; le discriminant a été rendu non ambigu et un test couvre l’ID correspondant.

Dans `apps/mobile`, les **75/75 tests API simulés**, le contrôle TypeScript et ESLint passent. `expo export --platform android` réussit et vérifie le bundle JavaScript/Android; il ne compile pas un APK natif et ne remplace pas un essai sur téléphone.

Les quatre profils ESP-IDF 6.0.0 compilent intégralement avec PlatformIO 7.0.0. Les tailles finales, marges et binaires issus du build sont consignés ci-dessous après compilation :

| Profil | Flash application | RAM statique | Binaire |
| --- | ---: | ---: | --- |
| `core_wifi_c6` | 1 031 320 / 1 048 576 octets | 84 980 / 327 680 octets | `.pio/b7102/core_wifi_c6/firmware.bin` |
| `core_zigbee_c6` | 561 710 / 1 966 080 octets | 39 852 / 327 680 octets | `.pio/b7102/core_zigbee_c6/firmware.bin` |
| `main_light_c6` | 553 680 / 1 966 080 octets | 39 540 / 327 680 octets | `.pio/b7102/main_light_c6/firmware.bin` |
| `lamp_c6` | 578 020 / 1 966 080 octets | 39 764 / 327 680 octets | `.pio/b7102/lamp_c6/firmware.bin` |

CORE-WIFI dispose d’une marge flash de **17 256 octets** dans sa partition d’application de 1 Mio. Cette marge réduite doit être surveillée avant toute évolution firmware supplémentaire.

## Validation matérielle V7.10.3.1

La configuration des groupes a été validée sur le montage matériel existant en **13/13 essais conformes**. Le parcours vérifié comprend la reconnaissance du MAIN, la configuration depuis Android, la création, le renommage et la suppression de groupes, la gestion des membres, la persistance après redémarrage de MAIN et la non-régression des commandes individuelles. Cette validation concerne un MAIN; elle ne couvre pas l'exécution collective V7.10.4.

## Limites de validation

Les 13 essais matériels de V7.10.3.1 ne remplacent pas la validation automatisée du serveur HTTP ESP-IDF, ni les essais sur plusieurs MAIN. Depuis cette étape, l'utilisateur a confirmé le parcours nominal de contrôle collectif ON/OFF sur matériel; le nombre de lampes simultanément utilisées n'est pas précisé. Les tests natifs et mobiles restent des simulations et ne couvrent pas les pannes ni la capacité maximale. Voir le [dossier V7.10.4](V7_10_4_GROUP_POWER.md) pour l'état détaillé.