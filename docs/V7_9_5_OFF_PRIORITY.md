# Smart Lighting — V7.9.5 : OFF prioritaire et retrait sûr pendant un fade

## Portée

V7.9.5 complète les transitions progressives V7.9.4 sans route HTTP ni changement de format UART/Zigbee. Les chemins existants `SET_POWER`, `SET_BRIGHTNESS` et le retrait sécurisé sont réutilisés. Pour un OFF prioritaire, le champ `value2` déjà présent dans le message porte l’ID de la commande de sortie qu’il supplante; la disposition des trames reste inchangée. La sortie concernée reste celle du profil `lamp_c6` de test LED basse tension sur GPIO18.

Cette version n’a pas encore été validée sur matériel. `EXECUTED` représente une confirmation du pilote LEDC/GPIO; ce n’est ni une mesure de courant ni une preuve que la LED émet de la lumière.

## OFF pendant un fade

Le CORE accepte un OFF prioritaire lorsqu’une seule commande d’état de sortie est non résolue pour cette lampe, qu’elle soit `sent`, `accepted` ou arrivée à expiration avec résultat inconnu. Une commande OFF déjà en cours, plusieurs commandes non résolues ou une autre commande concurrente ne sont pas supplantées. Les commandes ordinaires restent sérialisées par lampe; chaque opération conserve son propre ID.

À la réception du nouvel OFF, LAMP :

1. Vérifie l’identité et la destination de la commande par les mécanismes existants.
2. Arrête le fade LEDC actif.
3. Invalide sa génération locale pour qu’un callback ancien ne finalise pas une autre transition.
4. Écrit le duty OFF à zéro.
5. Envoie le résultat de la commande interrompue, puis le résultat de l’OFF avec leurs ID existants.

Avant l’écriture OFF, LAMP réserve dans le déduplicateur existant un résultat final pour l’ID antérieur. Si son premier paquet n’était pas encore arrivé, cette entrée sert de tombstone : un paquet retardé portant cet ID est rejeté et ne peut pas rallumer la sortie après OFF. CORE suspend les retries de l’ancienne commande dès que l’OFF prioritaire est envoyé; si OFF échoue, les retries de l’ancienne commande peuvent reprendre. Si le déduplicateur ne peut pas réserver le tombstone, la sortie est mise au mieux à OFF, mais le résultat reste `FAILED` (`dedup_full`) et ne peut pas être annoncé comme confirmé.

Si le fade avait déjà atteint sa cible avant l’intervention, sa commande conserve `EXECUTED`; l’OFF reçoit ensuite son propre résultat. Si le fade est arrêté avant la cible, sa commande reçoit `FAILED` et l’OFF reçoit `EXECUTED` seulement si l’écriture OFF réussit. Une retransmission de la commande interrompue rejoue son résultat FAILED depuis la déduplication, sans relancer le fade.

Si l’arrêt ou l’écriture OFF échoue, l’OFF reçoit FAILED et aucun faux succès n’est envoyé. Une erreur après l’arrêt marque la sortie locale incertaine et bloque les commandes d’état jusqu’à réinitialisation/récupération existante. Un arrêt LEDC échoué peut laisser le fade d’origine continuer; l’application ne prétend pas que la sortie est éteinte.

## ACK et état confirmé au CORE

Le suivi reste indépendant par ID de commande et destination. Le résultat d’une ancienne commande ne remplace jamais celui de l’OFF prioritaire. À la confirmation de l’OFF, CORE clôt l’ancienne entrée non résolue comme supplantée et ignore ses ACK tardifs; il n’envoie plus ses retries. Avec un ID antérieur unique, le tombstone LAMP empêche aussi l’exécution d’un paquet qui serait encore en transit, ce qui permet à l’ACK `EXECUTED` de l’OFF de confirmer l’état logiciel OFF.

Si plusieurs commandes sont non résolues, si aucun ID antérieur exploitable n’est connu, si la réservation du tombstone échoue, ou si OFF échoue/expire, l’état reste incertain et n’est pas présenté comme confirmé. Les refus locaux `command_in_progress` journalisent désormais le nombre d’opérations non résolues et l’ID/état du bloqueur afin de distinguer une version CORE-WIFI obsolète d’un suivi réellement encombré.

## Retrait sécurisé pendant un fade

Le retrait peut démarrer pendant le suivi d’une commande de sortie, mais pas pendant son POST, une relecture en cours ou un résultat incertain. CORE autorise au plus une commande de sortie connue en attente et refuse le retrait si l’exécution est déjà inconnue.

MAIN transmet la demande de retrait existante. LAMP vérifie l’identité, le parent, le transaction ID et les extrémités d’enveloppe avant d’agir. Avant d’appeler le traitement existant qui persiste le reçu de retrait, LAMP arrête un fade éventuel et écrit OFF. En cas d’échec, il répond `UNPAIR_REJECT` et préserve l’association NVS. Si l’arrêt réussit mais que l’écriture OFF échoue, le fade est invalidé, l’état est signalé comme défectueux et aucun reçu de retrait n’est créé. Après redémarrage avec un reçu de retrait déjà existant, la reprise réaffirme OFF avant de rejouer la confirmation.

Le résultat « OFF appliqué » confirme le retour du pilote lors de l’écriture, pas la tension réellement présente sur une charge ni l’absence de courant.

## Application mobile

- OFF reste activable pendant le suivi d’un ON ou d’un niveau brightness.
- Le statut affiché devient celui de la commande prioritaire après son POST confirmé.
- Le suivi GET de la commande précédente est annulé côté téléphone après réception de l’ID de la commande OFF; aucun POST n’est rejoué.
- Si le POST OFF est refusé sans ambiguïté, le suivi initial reprend et l’erreur OFF est affichée.
- Si la réponse du POST est perdue, l’interface conserve un résultat incertain sans renvoyer le POST.
- Le retrait peut être demandé pendant un suivi connu; l’ancien suivi continue de recevoir son résultat tandis que l’opération de retrait suit son ID séparé.

## Vérification et limites

Vérifications exécutées le 19 octobre 2026 :

- **Tests natifs : 157/157 réussis**, exécutés par suite : stabilisation 40, commandes V7 44, pairing/retrait V4 31, API CORE 12, pairing CORE 6, lien CORE 9, registre 9, codec Zigbee 6. Les nouveaux cas de stabilisation couvrent l’interruption d’une montée, d’une descente et l’extinction avant retrait, l’absence d’écriture NVS pour les niveaux intermédiaires/zéro, l’invalidation des callbacks et l’absence de reprise après retrait.
- **Mobile :** 72/72 tests API simulés, vérification TypeScript et lint réussis.
- **Android :** export Expo 57 réussi avec Node 24.19.0; bundle Android produit (1,7 MB). Le Node système est 20.19.4, inférieur au minimum `>=22.13` du projet; l’export a donc utilisé le runtime Node 24 déjà disponible, sans changer les dépendances.
- **Firmware :** les quatre builds PlatformIO ont réussi intégralement et produit ELF/BIN. ESP-IDF 6.0.0 / PlatformIO Espressif 32 7.0.0.

| Profil | Application / partition | Marge flash | RAM statique / 320 Kio | BIN |
|---|---:|---:|---:|---:|
| `lamp_c6` | 568 078 / 1 966 080 octets | 1 398 002 octets | 34 620 octets (10,6 %) | 568 512 octets |
| `main_light_c6` | 543 720 / 1 966 080 octets | 1 422 360 octets | 34 388 octets (10,5 %) | 544 112 octets |
| `core_zigbee_c6` | 557 280 / 1 966 080 octets | 1 408 800 octets | 34 716 octets (10,6 %) | 557 664 octets |
| `core_wifi_c6` | 1 017 178 / 1 048 576 octets | 31 398 octets | 75 956 octets (23,2 %) | 1 017 552 octets |

Le build natif PlatformIO a remplacé son dossier `.pio/build` partagé après la génération des firmwares; les ELF/BIN des quatre profils ne sont donc plus présents dans le cache local. Les builds avaient alors terminé avec `SUCCESS`, vérifié leur taille et créé leur `.bin`. Les tailles ci-dessus sont celles relevées pendant ces builds; aucun binaire n’est conservé dans Git.

Limites des tests : les scénarios de transition et le garde-fou de retrait sont simulés; `v5_provisioning_runtime.cpp` (qui impose l’ordre effectif arrêt fade → sortie LOW → reçu durable) n’est pas inclus dans le filtre natif. Le build firmware valide sa compilation, mais aucun test natif n’exécute une transaction `UNPAIR_REQUEST` pendant un fade. Les tests pairing vérifient séparément le protocole et la persistance. Une validation matérielle reste requise pour l’ordre d’interruption réel, les ACK radio, les callbacks ISR et les erreurs LEDC.

Les tests natifs simulent le pilote et les transports. Ils ne prouvent ni le timing d’ISR LEDC sur carte, ni le comportement radio sous charge, ni l’état électrique de la LED. La validation physique reste à faire pour OFF pendant fade, retrait pendant fade, perte d’ACK, commande retardée, et erreurs LEDC.

## V7.9.5.1 — correction et vérification

Le contrôle du code confirme que `command_in_progress` est renvoyé par la soumission CORE-WIFI quand plusieurs sorties de la même lampe restent non résolues, ou qu’un OFF prioritaire est déjà en cours. Une seule commande ON/brightness suivie en `sent` ou `accepted` est admissible comme opération supplantée. Le refus observé sur la carte ne peut pas être attribué avec certitude sans la version de firmware retournée par `/api/v1/health` et la ligne de diagnostic CORE correspondante; une ancienne version CORE-WIFI installée reste une explication possible, mais non démontrée.

Le chemin CORE rend la décision et la réservation atomiques sous le verrou de registre déjà partagé par HTTP, réception UART et suivi des ACK. L’OFF transmet l’ID de la seule commande qu’il supplante dans le champ existant `value2`. CORE suspend les retries de cette commande; LAMP réserve un tombstone dans le déduplicateur avant de confirmer OFF, ce qui interdit l’exécution d’un paquet brightness arrivé en retard. Les ACK des deux ID restent indépendants. En cas d’ambiguïté, d’échec de l’arrêt ou de capacité de déduplication insuffisante, l’état n’est pas confirmé OFF.

Android permet OFF pendant le suivi `sent`/`accepted`, ne lance qu’un seul POST OFF, suit son ID séparément et ignore les résultats de lecture d’état appartenant à une opération remplacée. Les autres commandes restent verrouillées. Une réponse HTTP 409 est conservée comme refus réel et affichée; elle n’entraîne aucune nouvelle tentative automatique.

Vérifications de la correction le 20 octobre 2026 :

- **Tests natifs : 160/160 réussis**; stabilisation ciblée : 41/41 et commandes V7 : 46/46.
- **Mobile : 73/73 tests API simulés**, vérification TypeScript et lint réussis.
- **Builds complets PlatformIO : 4/4 réussis**, avec génération des quatre `firmware.bin` et ELF, ESP-IDF 6.0.0 / PlatformIO Espressif 32 7.0.0.

| Profil | Application / partition | Marge flash | RAM statique / 320 Kio | BIN |
|---|---:|---:|---:|---:|
| `core_wifi_c6` | 1 017 962 / 1 048 576 octets | 30 614 octets | 75 956 octets (23,2 %) | 1 018 336 octets |
| `core_zigbee_c6` | 557 492 / 1 966 080 octets | 1 408 588 octets | 34 716 octets (10,6 %) | 557 872 octets |
| `main_light_c6` | 543 932 / 1 966 080 octets | 1 422 148 octets | 34 388 octets (10,5 %) | 544 320 octets |
| `lamp_c6` | 568 290 / 1 966 080 octets | 1 397 790 octets | 34 620 octets (10,6 %) | 568 720 octets |

Les binaires sont des artefacts locaux sous `.pio/build`; ils ne sont pas suivis par Git. Aucun firmware n’a été flashé et le correctif n’est pas encore validé sur matériel. Les logs d’un prochain refus HTTP 409 doivent être corrélés à la version `/health` et à `[CORE-COMMAND] priority OFF rejected`, qui indique le nombre d’opérations non résolues et l’identité/état du bloqueur.

Pour la validation matérielle, mettre à jour CORE-WIFI, MAIN et LAMP avec les environnements correspondants; CORE-ZIGBEE a aussi été compilé comme contrôle global, mais son pont UART ne modifie pas le contenu de commande. Vérifier ensuite que `/api/v1/health` retourne le nouveau firmware, démarrer un fade, soumettre OFF une fois pendant `sent`, puis répéter pendant `accepted` et pendant le fade. Chaque OFF doit obtenir son propre ID et finir `executed`; le niveau confirmé doit rester OFF et aucune retransmission tardive de l’ancien ID ne doit réallumer la LED. Si un HTTP 409 persiste, conserver la version `/health` et le diagnostic série associé avant toute nouvelle correction.
