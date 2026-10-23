# V7.10.4 — commandes collectives ON/OFF des groupes

## Périmètre

V7.10.4 ajoute le contrôle ON/OFF d'un groupe existant. Android envoie une seule opération au CORE-WIFI. CORE-WIFI la relaie à CORE-ZIGBEE, puis MAIN_LIGHTING résout les membres courants du groupe et envoie une commande Zigbee `SET_LAMP_POWER` indépendante à chaque lampe.

Cette étape ne modifie pas la définition persistée des groupes de V7.10.2/3. Elle n'ajoute ni scènes, ni automatisations, ni API multi-MAIN. Le parcours logiciel reste limité au MAIN unique supporté par l'application.

## Requête HTTP

Route authentifiée :

```http
POST /api/v1/modules/{main_id}/groups/{group_id}/commands/power
Authorization: Bearer <token>
Content-Type: application/json

{"operation_id": 123456, "power": "on"}
```

`power` vaut `on` ou `off`. Le CORE répond `202` après l'envoi à MAIN, puis l'application lit :

```http
GET /api/v1/group-operations/{operation_id}
```

Une réponse `sent` ou `accepted` n'atteste pas l'exécution des lampes. Le suivi fournit le résultat agrégé et les résultats individuels, avec les IDs V7 des lampes et les IDs enfants de commande. Les états par lampe distinguent `in_progress`, `executed`, `failed`, `offline`, `unsupported` et `uncertain`. Un résultat `superseded` est exposé avec un état `uncertain` et un résultat `superseded`.

Exemple de réponse de suivi :

```json
{
  "operation": {
    "id": 123456,
    "state": "completed",
    "result": "completed",
    "group_id": 9001,
    "revision": 0,
    "members": [
      {"lamp_id": 1000000001, "child_id": 711, "state": "executed", "result": "executed"},
      {"lamp_id": 1000000002, "child_id": 712, "state": "executed", "result": "executed"}
    ]
  }
}
```

La réussite agrégée `completed` n'est déclarée que si toutes les lampes membres ont un résultat terminal confirmé. `partial` conserve les résultats de chaque lampe. `uncertain` indique qu'une ou plusieurs exécutions n'ont pas pu être confirmées; un timeout ne prouve pas que la lampe n'a pas agi.

## IDs, fanout et retries

- L'`operation_id` parent identifie une demande Android→MAIN.
- MAIN crée un ID enfant distinct par lampe et conserve l'association entre enfant, ID local, ID V7 et groupe.
- MAIN vérifie que chaque membre existe toujours, appartient à ce MAIN, est appairé et en ligne, et annonce `DEVICE_CAP_POWER`.
- Les retries d'une lampe réutilisent le même ID enfant. Une lampe déjà confirmée ne reçoit pas de retry.
- Chaque enfant a un délai total borné de 15 secondes (envoi initial et deux retries). Une absence d'ACK devient `uncertain`.
- MAIN et CORE-WIFI conservent chacun jusqu'à seize opérations en RAM pendant dix minutes.

L'extension utilise la version 2 du protocole de groupe : `SET_POWER` (action 13), `MEMBER_RESULT` (14) et `QUERY_OPERATION` (15). La requête de statut est distincte d'une commande : elle renvoie l'état conservé, sans créer de commandes enfants. Si l'historique d'opération a disparu lors d'un redémarrage MAIN, MAIN répond `uncertain`; il ne lance pas le groupe. Un marqueur temporaire garde ce résultat incertain pendant la durée de rétention afin qu'un POST répété avec le même ID ne relance pas le groupe pendant ce démarrage.

La sécurité de déduplication après un redémarrage reste limitée par l'historique RAM : ni l'opération parent ni ses IDs enfants ne sont persistés. Si CORE-WIFI redémarre, le suivi HTTP peut répondre group_operation_not_found; l'application reprend uniquement par GET et ne renvoie pas de POST. Si MAIN redémarre, il faut considérer le résultat incertain et vérifier les lampes avant une nouvelle opération. Le marqueur RAM n'est pas une garantie après la rétention de dix minutes ou un redémarrage.

## V7.10.4.1 — lectures transitoires et historique des changements

Avant ce correctif, les GET et les mutations/commandes partageaient les mêmes 16 enregistrements CORE-WIFI. Une lecture terminée pouvait donc occuper une entrée pendant la rétention, puis une nouvelle commande collective échouait avec HTTP 409 lorsque le journal ne trouvait plus de place réutilisable. Le chemin des mutations de configuration utilisait aussi une réutilisation circulaire qui pouvait remplacer prématurément un résultat encore retenu; il est maintenant soumis à la même règle de conservation stricte que les commandes collectives.

Les lectures GET /api/v1/modules/{main_id}/groups et GET /api/v1/modules/{main_id}/groups/{group_id} utilisent désormais un contexte transitoire unique dans CORE-WIFI. Il conserve la corrélation UART pendant la lecture asynchrone, puis est libéré à la réponse terminale ou au timeout. Les GET ne consomment aucun des 16 emplacements réservés aux opérations de changement et n'écrivent pas en NVS. Une seule lecture de groupes peut être en vol; une lecture pendant une opération de groupe active ou une autre lecture est refusée en HTTP 409 group_operation_in_progress.

Si le GET ne peut pas être envoyé, CORE-WIFI répond HTTP 503 group_read_unavailable. Si MAIN ne fournit pas un snapshot complet dans le délai de huit secondes, la réponse est HTTP 503 group_result_uncertain; cette lecture peut être répétée sans risque. Le contexte est libéré au timeout et une réponse UART tardive est ignorée. Les réponses à un GET terminé n'ont pas d'ID de suivi durable.

Les 16 entrées CORE-WIFI restent réservées aux changements réels : création, renommage, ajout/retrait d'un membre, suppression et commandes collectives ON/OFF. Les opérations non résolues ne sont pas réutilisables. Un résultat terminal reste retenu pendant dix minutes à partir du début de l'opération; l'espace est ensuite réutilisable. Tant que les 16 entrées contiennent des résultats encore retenus ou des opérations non résolues, une nouvelle opération reçoit HTTP 409 group_operation_history_full. Cette limite est légitime et demeure indépendante du nombre de lectures effectuées. Un retry avec le même ID et le même contenu, pendant que son résultat est retenu, restitue l'opération existante sans nouvel envoi; un même ID associé à un autre contenu est rejeté.

Le contexte transitoire est une seule CoreGroupOperationRecord allouée statiquement (environ 1,1 Kio selon l’écart de RAM PlatformIO avant/après); aucune allocation mémoire par GET n’est nécessaire.

### Validation V7.10.4.1

Le 22 octobre 2026, les neuf suites natives complètes passent à 183/183, dont les trois nouveaux scénarios de handler HTTP CORE-WIFI. Le build complet core_wifi_c6 réussit. PlatformIO rapporte 1 037 208 / 1 048 576 octets d'application (11 368 octets de marge, 1,08 %) et 106 308 / 327 680 octets de RAM statique (221 372 octets libres). Le fichier firmware.bin généré fait 1 037 584 octets. Les autres profils et l'application mobile n'ont pas été reconstruits/retestés pour ce correctif; les validations mobiles 81/81 citées plus haut sont les résultats V7.10.4 antérieurs.
Les résultats du journal CORE-WIFI et du dédoublonnage MAIN restent volatils en RAM. Après redémarrage, une opération peut être introuvable ou incertaine. Une réponse HTTP perdue doit donc être suivie par GET du même ID; l'application ne renvoie pas automatiquement un POST.

### Validation matérielle V7.10.4.1 — lectures et capacité des POST

Le 22 octobre 2026, l'utilisateur a vérifié le correctif sur le CORE-WIFI réel avec le groupe `salon` (ID `1961450959`) et une seule lampe membre (ID V7 `3796524253`). Vingt GET de la liste des groupes et vingt GET du détail ont réussi. Un POST OFF ensuite (opération `1078092413`) a terminé en `completed`; son enfant `904` a été confirmé `executed` par la lampe. Cela valide que cette série de lectures ne consomme pas les entrées réservées aux changements.

Un essai distinct a soumis rapidement quatorze POST OFF avec des IDs uniques. Les quatorze POST ont d'abord répondu `sent`; une lecture ultérieure de leurs suivis a montré les quatorze opérations en `completed`. Le POST suivant, ID `1540256466`, a reçu HTTP 409 `group_operation_history_full`. Les opérations suivies étaient `1214326857`, `1075224952`, `1434136300`, `1394981970`, `1679152006`, `1775265154`, `1479398353`, `1119351997`, `2104312575`, `1548972566`, `1618519948`, `1001050535`, `1040097160` et `1720290465`. Des opérations antérieures retenues contribuaient également au journal; l'essai ne permet donc pas d'attribuer la saturation aux quatorze seules.

## V7.10.4.2 — rétention du journal et résultats incertains

L'audit du 22 octobre a confirmé la cause de la saturation : CORE-WIFI et MAIN avaient chacun 16 entrées. Sur CORE-WIFI, la fenêtre terminale de dix minutes était calculée depuis le début de l'opération; sur MAIN, elle commençait à la confirmation finale. Une série rapide pouvait donc occuper simultanément les entrées des deux journaux et provoquer `group_operation_history_full`. Les GET liste/détail utilisent un contexte transitoire; la lecture d'un résultat par GET ne crée aucune entrée supplémentaire.

La correction augmente à 32 les entrées de journal sur CORE-WIFI et MAIN, et sépare le contexte de lecture complet des enregistrements de commande CORE-WIFI pour limiter la RAM. Les résultats terminaux déterminés (`completed`, `failed`, `persisted`) ne sont pas évincables pendant au moins deux minutes après leur confirmation finale. Après ce délai, ils deviennent réutilisables uniquement si une nouvelle opération a besoin d'une place; ils ne sont pas effacés automatiquement et les GET ne prolongent pas ce délai. Tant qu'un résultat est présent, un POST répété avec le même ID et le même contenu le restitue sans renvoyer la commande; un contenu différent avec cet ID est rejeté. Les clients ne doivent jamais réutiliser un ancien ID, car la protection est en RAM et peut disparaître sous pression ou au redémarrage.

Une opération active n'est jamais recyclée. Sur CORE-WIFI, tout résultat `uncertain` reste épinglé tant qu'un résultat MAIN tardif peut le résoudre, y compris si la mutation concernait la configuration d'un groupe. Sur MAIN, un fanout `uncertain` avec des enfants garde ses IDs enfants tant qu'un ACK tardif peut arriver; ils ne sont pas sacrifiés pour libérer une place. Lorsqu'un ACK tardif rend tous les membres déterminés, le résultat final est mis à jour et son délai minimal de protection de deux minutes commence alors. Les marqueurs `uncertain` sans enfants, créés après une requête de statut inconnue sur MAIN, sont protégés au moins dix minutes; ils deviennent ensuite réutilisables si une place est nécessaire. En cas de saturation par des opérations réellement actives ou incertaines, le système peut encore refuser une nouvelle opération : c'est un refus de sécurité, pas une éviction silencieuse. Ces journaux sont volatils et disparaissent au redémarrage.

### Validation logicielle V7.10.4.2

Le test ciblé `test_v710_groups` réussit à **22/22** et la suite native complète à **187/187**. Les scénarios couvrent 40 cycles GET liste/détail, plus de vingt commandes collectives terminées sur chaque journal, protection d'une entrée active, relecture et retry d'un ID retenu sans nouvel envoi, conflit si le même ID reçoit un contenu différent, perte simulée de la réponse HTTP, timeout incertain, ACK tardif, résultats partiels et non-régression du fanout MAIN. Le handler HTTP de production est exercé avec le faux serveur natif. Ces validations ne remplacent pas une validation sur matériel.

Les builds ESP-IDF complets de `core_wifi_c6` et `main_light_c6` réussissent. CORE-WIFI utilise **1 037 502 / 1 048 576 octets** dans sa partition application (98,9 %, marge **11 074 octets**) et **102 484 / 327 680 octets** de RAM statique (31,3 %). MAIN utilise **561 188 / 1 966 080 octets** (28,5 %, marge **1 404 892 octets**) et **51 732 / 327 680 octets** de RAM statique (15,8 %). CORE-WIFI reste très proche de sa limite flash. Les compilations ont généré `firmware.bin`; les artefacts doivent être régénérés avant flash si le répertoire de build a été nettoyé.

La validation matérielle V7.10.4.2 n'est pas effectuée; aucun flash n'a été lancé. Les sorties de build ont indiqué la génération des deux `firmware.bin`, mais aucun binaire CORE-WIFI ou MAIN n'est présent dans le répertoire `.pio/build` du workspace au contrôle final; les reconstruire avant tout flash. Après installation contrôlée des nouveaux CORE-WIFI et MAIN, vérifier au minimum plus de vingt POST collectifs OFF avec IDs distincts, le suivi GET de résultats récents et un retry volontaire du même ID encore retenu sans deuxième fanout. Après une perte de réponse HTTP, faire uniquement un GET du même ID; ne jamais envoyer automatiquement un autre POST. Un cas de perte d'ACK peut rester validé par simulation si aucun mécanisme matériel contrôlé n'est disponible.

Le 409 observé correspond à la limite globale des seize entrées de changements CORE-WIFI, partagées entre groupes et entre mutations de configuration et commandes collectives. Il ne signale pas un échec des GET. Le suivi ultérieur n'a plus retrouvé les IDs antérieurs `1297279465` et `1941627382`, ce qui est cohérent avec leur expiration hors de la fenêtre de rétention; cela n'altère pas leurs résultats `completed` observés plus tôt. Cet essai a utilisé une seule lampe et des commandes OFF répétées; il ne valide pas les courses ON/OFF, le fanout multi-lampes ni la concurrence générale. Chaque POST accepté est une vraie commande et doit être suivi par GET avant de décider de la suite; un POST refusé avec 409 ne doit pas être répété avant libération d'une entrée.

## Concurrence et OFF prioritaire

MAIN n'accepte qu'une opération collective active à la fois. La modification d'un groupe, l'appairage/retrait ou une commande individuelle incompatible est refusée pendant l'opération. Une commande `OFF` individuelle garde sa priorité V7.9 sur la lampe concernée : l'enfant collectif est signalé `superseded`, son retry est arrêté et son ACK tardif ne remplace pas le résultat prioritaire.

`OFF` collectif n'est pas une préemption prioritaire : il est distribué comme un ON collectif, lampe par lampe. Aucun succès de groupe n'est annoncé avant les confirmations attendues. Les résultats partiels et incertains restent visibles.

## Android

Depuis l'écran **Groupes**, ON ou OFF déclenche un seul POST pour le groupe, jamais un POST individuel par lampe. L'application enregistre d'abord l'ID parent et les informations nécessaires dans `expo-secure-store`, puis suit le même ID par GET. En cas de perte de réponse HTTP, fermeture de l'application ou redémarrage du téléphone, elle reprend le suivi GET; elle ne renvoie pas automatiquement la commande. Un résultat incertain demande une vérification avant de poursuivre.

## Validation logicielle et compilations

Le 20 octobre 2026, les neuf suites natives complètes réussissent : **180/180 tests**. `test_v710_groups` couvre les scénarios existants de groupes, le fanout deux lampes, ON/OFF, IDs enfants distincts, ACK ciblés, retry stable, OFF individuel prioritaire, retrait occupé, timeout, ACK tardif, réponse à un `QUERY_OPERATION` connu sans fanout et requête inconnue qui reste incertaine sans fanout. Les codecs UART et Zigbee valident la sérialisation de l'action de consultation V2.

Dans `apps/mobile`, les **81/81 tests API simulés**, le contrôle TypeScript et ESLint ont été relancés le 22 octobre 2026 avec Node **v24.21.0** et réussissent. La première tentative avec PowerShell a été bloquée par sa politique d'exécution pour `npm.ps1`; l'utilisation de `npm.cmd` a permis d'exécuter les scripts sans modifier cette politique. Aucun export Android V7.10.4 n'est déclaré ici. Les tests simulés ne remplacent pas le vrai serveur HTTP ESP-IDF ni un essai sur téléphone.

La compilation PowerShell a d'abord échoué au lien : `src/native_v5_runtime_test_stubs.cpp`, prévu pour les tests natifs, était inclus par `file(GLOB ...)` dans `src/CMakeLists.txt` et doublonnait notamment `communicationReady`, `readDeviceHardwareId` et des fonctions Zigbee. Après exclusion explicite de ce fichier du composant ESP-IDF, l'utilisateur a confirmé le 22 octobre 2026 la compilation réussie des quatre profils. Les quatre `firmware.bin` sont présents dans `.pio/build` :

| Profil | Taille du fichier `firmware.bin` | Rapport PlatformIO conservé |
|---|---:|---|
| `core_wifi_c6` | 1 037 024 octets | RAM 87 748 / 327 680 (26,8 %); occupation application 1 036 644 / 1 048 576 octets (98,9 %), marge 11 932 octets (1,1 %) |
| `core_zigbee_c6` | 563 088 octets | Build réussi; rapport RAM/flash détaillé non conservé |
| `main_light_c6` | 561 504 octets | Build réussi; rapport RAM/flash détaillé non conservé |
| `lamp_c6` | 585 872 octets | Build réussi; rapport RAM/flash détaillé non conservé |

Les tailles de binaires sont les tailles des fichiers générés, pas l'occupation de la partition application. CORE-WIFI dispose d'une marge flash très faible. Les anciens chiffres de V7.10.3 ne sont pas applicables à cette révision.

## État matériel et limites

L'utilisateur a confirmé sur matériel le parcours nominal de création et modification des groupes, l'ajout/retrait de lampes et les commandes collectives ON/OFF. Le nombre de lampes utilisées simultanément n'a pas été précisé. Cela ne valide pas la capacité de dix lampes par groupe ni les scénarios d'échec.

Les scénarios multi-lampes simultanées, résultat partiel, perte d'ACK, retries radio, ACK tardif, concurrence avec des commandes individuelles, redémarrage pendant une opération et coupure pendant une écriture ne sont pas déclarés validés sur matériel. La configuration des groupes reste validée séparément en 13/13 essais V7.10.3.1.

V7.10.4.1 a été testé nativement par dispatch des requêtes vers le handler HTTP de production avec un faux serveur ESP-IDF; les essais matériels du 22 octobre couvrent le CORE-WIFI réel, un MAIN et une lampe. Dans le code V7.10.4.2, chaque journal CORE-WIFI et MAIN a 32 entrées. Les résultats déterminés sont retenus deux minutes après confirmation; les opérations incertaines restent protégées tant qu'elles peuvent encore recevoir une résolution, et les tombstones inconnus de MAIN dix minutes. Le suivi reste volatil en RAM : aucune donnée d'opération n'est persistée en NVS et aucun changement de partition ou de protocole n'a été effectué. Les scénarios multi-lampes simultanées, pannes, perte d'ACK, concurrence ON/OFF et redémarrage pendant une opération restent hors validation matérielle pour V7.10.4.2.
