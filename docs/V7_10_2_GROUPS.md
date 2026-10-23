# V7.10.2 — Groupes persistants sur MAIN_LIGHTING

## Périmètre

V7.10.2 ajoute le modèle local de groupes sur MAIN_LIGHTING, les opérations natives de gestion et la sauvegarde/restauration dans la partition NVS existante.

Cette version ne fournit encore **ni route HTTP, ni écran Android, ni commande collective Zigbee**. Les capacités descriptives GROUPS déjà annoncées par MAIN ne signifient pas que le contrôle collectif des lampes est disponible.

## Modèle

- Un MAIN peut conserver jusqu’à 10 groupes; chaque groupe peut contenir jusqu’à 10 lampes.
- L’ID du groupe est propre au MAIN.
- Le nom est possédé par `LampGroup`, borné à 31 octets UTF-8 plus le terminateur NUL. Les pointeurs temporaires ne sont pas conservés.
- `lampIds[]` contient les IDs locaux attribués par ce MAIN au moment du pairing. Il ne contient ni les IDs V7 présentés à l’application/CORE, ni des adresses radio, ni des index de tableaux. Les IDs locaux sont stockés en `uint32_t`.
- Chaque modification effective augmente la révision du groupe. Une opération sans changement ne déclenche pas d’écriture flash.
- Pour ajouter un membre, le MAIN vérifie que la lampe existe dans son registre, qu’elle est appairée à ce même MAIN et qu’elle n’est pas déjà membre.

Le registre restauré conserve les IDs locaux même si les annonces des lampes arrivent plus tard au démarrage. Les opérations qui ajoutent ou modifient des membres valident les rattachements connus dans le registre MAIN.

## Persistance NVS

Les groupes utilisent le namespace `main_groups` dans la partition `nvs` déjà présente (`0x9000`, taille `0x6000`). Les espaces `main_pairing`, `lamp_pairing`, `lamp_removal`, `lamp_settings` et la partition `zb_storage` ne sont pas utilisés ni modifiés par ce stockage.

Le snapshot compact est versionné (format 1), identifié par un marqueur, rattaché à l’ID logique du MAIN et contrôlé par CRC32. Deux emplacements alternés portent un numéro de génération : une nouvelle sauvegarde est écrite dans l’emplacement le plus ancien et `nvs_commit()` doit réussir avant que le registre RAM soit remplacé. Une écriture invalide ne remplace donc pas le snapshot précédent. Le snapshot fait 864 octets dans cette version; les deux emplacements utilisent environ 1,7 Kio de données NVS, hors métadonnées NVS.

Au démarrage :

- aucune sauvegarde signifie registre vide et stockage prêt;
- un snapshot valide restaure les groupes;
- une version inconnue, un stockage illisible ou deux snapshots corrompus laisse la vue RAM vide et bloque les mutations. Les données NVS ne sont pas effacées automatiquement.
- si un emplacement est corrompu mais que l’autre contient un snapshot valide, ce dernier est restauré; une écriture ultérieure peut remplacer l’emplacement invalide.

## Retrait et réappairage

Après validation du reçu `UNPAIR_CONFIRM` (ou de l’annonce équivalente `UNPAIRING`), MAIN persiste d’abord la suppression de toutes les références du membre dans les groupes. Il ne valide ensuite la suppression de l’association dans son journal de provisioning. Si l’écriture des groupes échoue, la transaction de retrait n’est pas confirmée et l’association reste présente.

Si le nettoyage des groupes réussit mais que l’écriture ultérieure du journal de pairing échoue, l’ancienne association reste réservée et aucun groupe ne conserve une référence pendante. Le journal de retrait reste à réconcilier; après redémarrage, le reçu peut être retraité de façon idempotente. Si une lampe est ensuite réappairée, son nouvel ID local ne rejoint aucun ancien groupe automatiquement.

Une erreur persistante ou un format de groupes inconnu peut donc bloquer le retrait de lampes jusqu’à ce que le stockage puisse être lu/écrit correctement. Cette règle est volontaire : elle évite de supprimer une association alors qu’un groupe durable conserve encore l’ID de cette lampe.

## Compatibilité V7.9

Le stockage des groupes est additif. Aucun format UART/Zigbee, partitionnement, ID réservé, mécanisme de réservation, pairing, brightness NVS de LAMP ou retrait radio n’est modifié. Les groupes sont dans un namespace séparé de ceux des lampes et des réservations MAIN.

## Validation V7.10.2

Les tests natifs couvrent les capacités, noms possédés et bornés, ID de lampe `uint32_t` élevé, vérification MAIN parent, révisions, persistance par reconstruction, absence de données, corruption/version inconnue, erreurs d’écriture et de commit, retrait/réappairage, et absence d’interférence avec les réservations. La suite native complète atteint **174/174**.

Le 20 octobre 2026, les quatre profils affectés ont été compilés intégralement avec ESP-IDF 6.0.0 :

| Profil | Application utilisée / partition | RAM statique |
| --- | ---: | ---: |
| `main_light_c6` | 547 238 / 1 966 080 octets (27,8 %) | 34 724 / 327 680 octets (10,6 %) |
| `core_wifi_c6` | 1 017 992 / 1 048 576 octets (97,1 %) | 76 292 / 327 680 octets (23,3 %) |
| `core_zigbee_c6` | 557 528 / 1 966 080 octets (28,4 %) | 35 052 / 327 680 octets (10,7 %) |
| `lamp_c6` | 571 576 / 1 966 080 octets (29,1 %) | 34 948 / 327 680 octets (10,7 %) |

Les quatre `.bin` et `.elf` ont été générés. CORE-WIFI dispose d’environ **30 Kio** de marge flash; toute évolution qui ajoute du code partagé devra surveiller cette limite. Les images compilées sont dans le dossier de build temporaire ignoré par Git. Les tests natifs utilisent un store mémoire et ne remplacent pas un essai sur carte avec de vraies écritures NVS. Aucune validation matérielle V7.10.2 n’est déclarée.

## Étapes suivantes non incluses

L’API, l’application Android et le moteur de commandes collectives nécessiteront une étape ultérieure. L’exécution collective devra envoyer une commande Zigbee individuelle par ID local, suivre chaque ACK et rapporter les échecs partiels. `executeGroupCommand()` n’est pas un chemin de contrôle matériel distant et ne doit pas être utilisé à cette fin.

**Suite historique :** V7.10.3 ajoute ensuite la lecture et la configuration des groupes par HTTP/Android, sans ajouter de commande collective. Voir le [dossier V7.10.3](V7_10_3_GROUPS_ANDROID.md). La persistance réelle sur matériel reste à valider.
