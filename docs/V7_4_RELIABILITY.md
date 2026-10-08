# V7.4 — Routage multi-lampes et état confirmé

## Périmètre et statut

V7.4 conserve le transport et le protocole V7.3. V7.4.1 ajoute une couverture native du routage `SET_POWER` indépendant vers deux lampes ayant le même MAIN. V7.4.2 expose dans l’API l’historique logiciel de la dernière commande ON/OFF confirmée pour chaque lampe.

Le **8 octobre 2026**, l’utilisateur a clôturé la validation fonctionnelle matérielle V7.4.2 sur le montage de quatre ESP32-C6 : CORE-WIFI, CORE-ZIGBEE, un MAIN_LIGHTING et une LAMP_C6. La validation multi-lampes V7.4.1 est native; deux lampes physiques derrière le même MAIN n’ont pas été testées sur ce montage.

## Contrat de `last_confirmed_state`

`GET /api/v1/devices` et `GET /api/v1/devices/{id}` ajoutent `last_confirmed_state`. Le champ préexistant `state:null` reste inchangé. Les réponses `/modules` ne sont pas modifiées.

- `{"power":null,"status":"unknown"}` : aucune confirmation utilisable depuis le démarrage du CORE.
- `{"power":"on","status":"confirmed"}` ou `off` : dernière commande `EXECUTED` corrélée et validée.
- `{"power":"on","status":"stale"}` ou `off` : valeur historique conservée, mais disponibilité ou ordre d’exécution incertain.

Cette donnée est un résultat logiciel d’ACK, **pas une mesure électrique de la lampe**. Elle est conservée en RAM dans le registre CORE; après redémarrage du CORE-WIFI, elle repart à `unknown`. Aucun nouvel espace NVS ni protocole de resynchronisation n’a été ajouté.

## Tests natifs et compilations

- V7.4.1 : **86/86 tests natifs**, dont le scénario de deux lampes, leur routage indépendant et la corrélation de commandes/ACK. Le code de production n’avait pas été modifié pour cette étape.
- V7.4.2 : **92/92 tests natifs**. Les tests couvrent l’état initial, ACK accepté ou invalide, ACK final, expiration, indépendance des lampes, ordre d’ACK ambigu, réannonce, sérialisation JSON et remise à zéro du registre.
- Compilation finale : les quatre profils C6 ont réussi.

| Profil | Flash utilisée / partition | RAM utilisée |
|---|---:|---:|
| `core_wifi_c6` | 995 810 / 1 048 576 octets (95,0 %) | 74 612 / 327 680 (22,8 %) |
| `core_zigbee_c6` | 547 996 / 1 966 080 (27,9 %) | 34 156 / 327 680 (10,4 %) |
| `main_light_c6` | 523 446 / 1 966 080 (26,6 %) | 33 852 / 327 680 (10,3 %) |
| `lamp_c6` | 526 304 / 1 966 080 (26,8 %) | 33 868 / 327 680 (10,3 %) |

## Résultats matériels rapportés

La lampe testée avait l’ID V7 `3559985816`. Les commandes ci-dessous ont terminé avec l’état API `executed` et zéro retry; les vues liste et détail concordaient, avec `state:null` inchangé.

| ID de commande | Demande | État API / retries | État HTTP confirmé observé |
|---:|---|---|---|
| `268555531` | ON | `executed`, 0 | `on / confirmed` |
| `268555532` | OFF | `executed`, 0 | `off / confirmed` |
| `268555533` / `268555535` | ON | `executed`, 0 | `on / confirmed` |
| `268555534` / `268555536` | OFF | `executed`, 0 | `off / confirmed` |
| `268555537` | ON après reconnexion | `executed`, 0 | `on / confirmed` |
| `268555538` | OFF après reconnexion | `executed`, 0 | `off / confirmed` |

Lors de la déconnexion de LAMP, l’API a rapporté `online=false` et `off / stale`. Après reconnexion, elle a rapporté `online=true`, en conservant `off / stale`; un nouvel ACK `EXECUTED` a ensuite rétabli `confirmed`.

Deux commandes d’essai d’expiration, `1401910411` et `1401910412`, ont terminé en `expired`, avec deux retries et `error=execution_unknown`. Les réponses devices conservaient `state:null` et le même `last_confirmed_state` dans les deux routes. Les traces montrent `on / stale`; la capture précédant la commande `1401910412` affichait déjà `on / stale` et la demande était ON. Cette capture confirme l’expiration et le marquage `stale`, mais ne démontre pas à elle seule la conservation d’une valeur antérieure opposée.

## Limites observées

- Une expiration laisse l’exécution réelle inconnue. Le CORE garde alors son indicateur d’incertitude pendant ce démarrage; des ACK `EXECUTED` reçus ensuite n’ont pas promu l’historique stale dans cette session. Après redémarrage, l’état est redevenu `unknown`, puis les commandes `268555531` et `268555532` ont établi de nouveaux états confirmés.
- Une lampe online n’est pas nécessairement dans un état électrique mesuré; aucun capteur de retour électrique n’est présent.
- Le test physique portait sur une seule lampe. L’indépendance de deux lampes est validée par simulation native, pas par deux LAMP_C6 physiques.
- Une requête de commande alors que la cible était offline a répondu `409 offline`. Dans un essai, le script PowerShell a poursuivi avec un ID vide et généré des `400 bad_request` lors du GET de suivi; ces erreurs secondaires venaient du script après le refus, pas d’une commande créée.

Le détail de l’API HTTP V7.3 reste dans [la documentation V7.3](V7_3_HTTP_COMMANDS.md); les commandes, retries et expirations V7.2 restent dans [le dossier V7.2](V7_2_COMMANDS.md).
