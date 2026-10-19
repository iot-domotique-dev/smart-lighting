# V7.8.0 — Luminosité PWM

## Périmètre et statut

V7.8.0 regroupe la commande `SET_LAMP_BRIGHTNESS` de CORE-WIFI jusqu’à LAMP_C6, la route HTTP authentifiée, le contrôle depuis Android et le profil unique `lamp_c6` avec LEDC. La commande accepte un pourcentage entier de 0 à 100. Elle réutilise `MessageType::COMMAND`, `ActionType::SET_LAMP_BRIGHTNESS`, le champ `Message::value`, le tracker et les ACK existants. Les formats UART et Zigbee restent inchangés.

Le profil `lamp_c6` active LEDC sur le banc actuel : GPIO18 → résistance de 220 Ω → LED témoin → GND, sortie active-haut. À 3,3 V, en supposant une chute nulle sur la LED, le courant de crête théorique maximal est 3,3/220 = 15 mA; la chute directe réelle le réduit. Le plafond PWM est réglé à 100 % (duty 1023/1024), correspondant au niveau haut déjà utilisé par ON/OFF sur ce montage. Le duty réduit le courant moyen, pas le courant instantané dans chaque impulsion. La valeur nominale et la caractéristique électrique exacte de la LED n’étant pas documentées, ce calcul ne constitue pas une preuve absolue de sûreté; vérifier la polarité et, si possible, mesurer le courant avant un essai prolongé. Le montage sert uniquement à une LED basse tension, jamais à un relais, une lampe secteur ou une charge de puissance. Les essais de PWM sur ESP32-C6 et le contrôle HTTP de la plage 0–100 % ont été rapportés réussis par l’utilisateur.

La vérification native de cette révision compte 128 tests réussis sur les huit suites, y compris les scénarios ajoutés pour le banc LED; ils n’ont pas été relancés lors de la vérification de compilation ci-dessous. Le tableau historique ci-dessous conserve les mesures des profils matériels par défaut compilés les 15 et 16 octobre 2026, avant ce complément matériel. La taille comptabilisée par PlatformIO dans la partition applicative est distinguée de la taille réelle du fichier `.bin`.

| Profil | Flash PIO / partition | Marge PIO | RAM statique / totale | `.bin` / marge brute |
|---|---:|---:|---:|---:|
| `core_wifi_c6` | 1 016 788 / 1 048 576 octets | 31 788 octets | 75 556 / 327 680 octets | 1 017 168 / 31 408 octets |
| `core_zigbee_c6` | 554 138 / 1 966 080 octets | 1 411 942 octets | 34 316 / 327 680 octets | 554 512 / 1 411 568 octets |
| `main_light_c6` | 539 278 / 1 966 080 octets | 1 426 802 octets | 33 988 / 327 680 octets | 539 664 / 1 426 416 octets |
| `lamp_c6` avant fusion (digital) | 542 176 / 1 966 080 octets | 1 423 904 octets | 33 996 / 327 680 octets | 542 560 / 1 423 520 octets |

### Recompilation complète du 16 octobre 2026

Avant la fusion décrite ici, les builds mesuraient séparément la variante numérique et la variante PWM. Ces valeurs historiques ne décrivent pas le binaire courant après fusion. Le build final du profil unique est consigné ci-dessous après compilation.

| Profil | Résultat | Flash PIO / partition | Marge PIO | RAM statique / totale | `.bin` / marge brute |
|---|---|---:|---:|---:|---:|
| `lamp_c6` avec LEDC | réussi | 554 362 / 1 966 080 octets | 1 411 718 octets | 34 204 / 327 680 octets | 554 784 / 1 411 296 octets |

Le profil `lamp_c6` fixe `SMART_LIGHTING_LED_PWM_MAX_DUTY_PERCENT=100` uniquement pour ce circuit LED documenté. La compilation complète du 16 octobre 2026 a lié les symboles LEDC et respecté la partition. Le staging temporaire court a contourné la limite de longueur de commande Windows; aucun fichier de source n’a été déplacé et aucun flash ni effacement n’a été effectué. Le SHA-256 du `.bin` produit est `4B4C415C1C8A5F433246CF8D3145477F2BE68EE61614FCC05D509F7CD816AB45`.

## Route HTTP

La nouvelle route est :

```http
POST /api/v1/devices/{id}/commands/brightness
Authorization: Bearer <jeton local>
Content-Type: application/json

{"percent":50}
```

`percent` doit être un entier JSON compris entre 0 et 100 inclus. Les valeurs négatives, supérieures à 100, décimales, chaînes, booléens, clés supplémentaires ou dupliquées sont refusées. La route exige le jeton Bearer existant, une lampe connue, en ligne, et annoncée avec `DEVICE_CAP_BRIGHTNESS`.

Une requête valide retourne HTTP `202 Accepted` avec l’ID du tracker existant, par exemple :

```json
{"command":{"id":123456,"state":"sent"}}
```

`202`, `sent` et l’ACK `accepted` n’attestent pas l’exécution. Consulter le résultat avec la route existante :

```http
GET /api/v1/commands/{id}
Authorization: Bearer <jeton local>
```

Les erreurs importantes sont `400` pour une valeur invalide, `401` sans jeton valide, `404` pour une destination inconnue et `409` si la lampe est hors ligne, ne supporte pas la luminosité ou a déjà une commande de sortie en cours. Après une perte de réponse HTTP, le client ne doit pas renvoyer automatiquement une nouvelle commande : la première peut déjà avoir été exécutée. Il doit interroger l’ID reçu si disponible, ou signaler que le résultat est inconnu.

## Capacité et compatibilité V7.7

Les lampes continuent d’annoncer `DEVICE_CAP_POWER`; `DEVICE_CAP_BRIGHTNESS` n’est annoncé que si le firmware LAMP a été construit avec l’option explicite `SMART_LIGHTING_LAMP_PWM_ENABLED` et que l’initialisation LEDC a réussi. Sans cette capacité, CORE-WIFI et MAIN refusent la commande brightness, et `SET_POWER` conserve son chemin et son comportement existants.

La route ON/OFF reste `POST /api/v1/devices/{id}/commands/power`. L’application Android V7.7 reste compatible : aucune route existante ni le contrat de `state:null` n’a été supprimé ou modifié. Il n’y a pas de changement du provisioning, du retrait, des partitions ou des credentials.

Une seule commande qui change la sortie peut attendre un résultat final par lampe, que ce soit `SET_POWER` ou `SET_LAMP_BRIGHTNESS`. Une seconde commande sur cette lampe reçoit HTTP `409` (`command_in_progress`). Les commandes de lampes différentes restent indépendantes. Le tracker conserve le message d’origine pour corréler ACK, destination, action et valeur; les retries et la déduplication existants sont réutilisés.

## Sortie PWM et sémantique

Le profil unique `lamp_c6` active LEDC basse vitesse, 5 kHz et résolution 10 bits sur GPIO18, uniquement pour le montage LED décrit ci-dessus. Les profils CORE et MAIN ne l’activent pas. La polarité active-haut (`SMART_LIGHTING_LED_PWM_ACTIVE_LOW=0`) correspond au branchement GPIO18 → résistance → anode LED → cathode vers GND. Chaque appel LEDC retourne les erreurs du pilote; celles-ci deviennent un résultat d’exécution `failed`, jamais un ACK `EXECUTED`. Si l’initialisation LEDC échoue, la sortie conserve le repli numérique ON/OFF et la lampe n’annonce pas `DEVICE_CAP_BRIGHTNESS`.

Lorsque l’option PWM est activée sur un matériel vérifié :

- `0` applique un duty de 0 et éteint la LED.
- `100` applique le plafond configuré comme maximum autorisé. Les valeurs intermédiaires sont mises à l’échelle jusqu’à ce plafond; ce n’est pas nécessairement 100 % de duty électrique.
- `SET_POWER OFF` applique 0 sans effacer le dernier niveau positif gardé en RAM.
- `SET_POWER ON` restaure ce niveau RAM; la valeur par défaut après démarrage à froid est 100 %.
- Un niveau positif reçu alors que la lampe est éteinte l’allume.
- Le niveau n’est pas écrit en NVS; une variation ne provoque aucune écriture flash.
- Au démarrage, GPIO est placé au niveau inactif correspondant à la polarité avant l’initialisation PWM. L’état RAM commence éteint; le dernier niveau non nul revient au défaut logiciel 100 %, qui ne dépasse pas le plafond configuré.

Pour un profil non PWM, brightness est refusé comme non pris en charge; le code ne simule pas une réussite et ne modifie pas la sortie digitale.

**Contrôle électrique préalable obligatoire :** relever la valeur de la résistance, vérifier le sens de la LED hors tension, puis estimer le courant direct à partir de la tension GPIO, de la tension directe de la LED et de la résistance. Vérifier que le courant de crête reste dans les limites recommandées pour GPIO/ESP32-C6 et la carte; le rapport cyclique ne réduit que le courant moyen, pas le courant instantané dans chaque impulsion. Une résistance série ne prouve pas à elle seule que le courant est acceptable. Ce profil est réservé à la LED du banc et ne doit pas piloter directement une lampe secteur ni une charge de puissance.

Le profil `lamp_c6` active `SMART_LIGHTING_LAMP_PWM_ENABLED` et la dépendance CMake `esp_driver_ledc`. À la requête brightness `100`, le duty vaut le plafond choisi, 1023/1024 (environ 99,9 %); `0` impose LOW et duty zéro. À 5 kHz, la résolution 10 bits est prise en charge par LEDC et la valeur de duty reste au plus à 1023, évitant la borne de débordement. Construire avec `pio run -e lamp_c6` ne flashe pas la carte. Les profils `main_light_c6`, `core_zigbee_c6` et `core_wifi_c6` n’activent pas LEDC ni la capacité brightness.

Les **128 tests natifs** avaient réussi dans la validation V7.8 antérieure. Après la fusion du profil, les résultats ciblés consignés le 16 octobre 2026 sont `pio test -e native -f test_stabilization` (12/12) et `pio test -e native -f test_v7_commands` (43/43). Ils couvrent notamment le mapping 0–100 vers le plafond, le repli sans PWM, l’échec d’écriture, SET_POWER, les ACK, les retries et l’indépendance des commandes. La suite native complète n’a pas été relancée pour cette clôture. Les tests natifs ne remplacent pas une mesure électrique sur le banc.

## État HTTP confirmé

Les réponses liste et détail des lampes conservent `"state":null` et ajoutent les propriétés suivantes dans `last_confirmed_state` :

```json
{
  "power":"on",
  "status":"confirmed",
  "brightness_percent":50,
  "brightness_status":"confirmed"
}
```

`brightness_percent` vaut `null` avec `brightness_status:"unknown"` si aucune confirmation exploitable n’existe. Le statut `stale` conserve une ancienne valeur mais signale qu’elle ne peut pas être considérée actuelle. Une commande envoyée ou acceptée ne modifie pas cette valeur. Seul un ACK final `EXECUTED` valide, corrélé au tracker, peut enregistrer la consigne comme confirmée. Un ACK invalide ne modifie pas l’historique. Une expiration avec exécution inconnue ou un ordre d’exécution ambigu conserve l’ancienne valeur en `stale` au lieu de prétendre que la nouvelle valeur a été appliquée.

Hors ligne, l’historique peut rester visible mais est `stale`. Après redémarrage de CORE-WIFI, son registre en RAM est vide : brightness revient à `unknown` jusqu’à une nouvelle confirmation. L’ACK confirme l’exécution logicielle de la commande par LAMP, pas la tension, le courant, le flux lumineux ou l’état électrique de la charge.

Le protocole actuel ne porte pas d’identifiant de démarrage LAMP ni de lecture de retour PWM. Si une LAMP redémarre sans que CORE-WIFI observe son passage hors ligne, il ne peut pas distinguer cette séquence d’une annonce périodique normale; la dernière confirmation peut alors rester historique. Cette limite n’est pas corrigée par un compteur local et ne constitue pas une mesure de la sortie. Une resynchronisation démontrable après redémarrage LAMP nécessiterait une évolution de protocole, hors périmètre V7.8.0.

## Validation du banc et limites

Le 16 octobre 2026, l’utilisateur a confirmé le contrôle de la luminosité depuis Android jusqu’à la LED physique sur GPIO18, via HTTP, UART et Zigbee. CORE reconnaît la capacité `brightness`; le profil `lamp_c6` utilise LEDC et accepte les niveaux 0–100 %. Cette validation fonctionnelle porte sur le prototype à LED basse tension et résistance série de 220 Ω, avec un MAIN et une LAMP. Le courant exact n’a pas été mesuré dans cette validation. À 3,3 V, la borne théorique avec une chute LED nulle reste 15 mA; le duty limite le courant moyen, pas les pics. Vérifier le montage si la LED ou l’alimentation change. Ne jamais utiliser GPIO18 directement pour un relais, une lampe secteur ou une charge de puissance.

À chaque démarrage, contrôler le log `[LAMP] LEDC output ready` et la présence de `brightness` dans les capacités. Si l’initialisation LEDC échoue, le firmware garde le chemin ON/OFF numérique et n’annonce pas `DEVICE_CAP_BRIGHTNESS`. La référence V7.7 pour pairing, retrait et persistance reste inchangée. L’ACK `EXECUTED` atteste l’exécution logicielle de la consigne; il ne mesure ni le courant ni la lumière émise.

## Contrôle depuis l’application Android/iOS

L’application mobile expose un curseur entier de 0 à 100 uniquement pour les lampes qui annoncent `brightness`. Il est désactivé si la lampe ou son MAIN est hors ligne, si l’inventaire est ancien, si un retrait bloque l’équipement ou si une commande de sortie est active. Les boutons ON/OFF restent disponibles pour les lampes sans capacité brightness.

Le déplacement du curseur ne fait que changer la sélection locale. Un POST unique est envoyé au relâchement, uniquement si le niveau choisi diffère de la dernière valeur `confirmed`. Pour un état `unknown`, l’application affiche le niveau confirmé comme inconnu et n’invente pas de niveau initial; `stale` conserve l’ancienne valeur avec un avertissement. La sélection locale reste distincte de la consigne confirmée.

L’application réutilise `POST /api/v1/devices/{id}/commands/brightness`, le suivi existant `GET /api/v1/commands/{id}` et la relecture `GET /api/v1/devices/{id}`. `sent` et `accepted` restent en cours. Seul `executed` est un ACK d’exécution. Les états `failed`, `expired`, les réponses perdues et les erreurs de suivi ne deviennent pas un succès. Après un résultat incertain, l’application relit l’état par GET; si cette lecture échoue, elle garde la commande bloquée jusqu’à une relecture manuelle. Elle ne répète jamais automatiquement un POST. Les commandes ON/OFF et brightness partagent un seul verrou par lampe, sans bloquer les autres lampes.

L’état de luminosité du firmware n’est gardé qu’en RAM. Après un redémarrage à froid de LAMP, l’application ne suppose pas que le dernier niveau a survécu; elle affiche les valeurs et statuts actuellement renvoyés par CORE. La consigne confirmée est une valeur logicielle, pas une mesure de lumière.

Le Slider natif utilise `@react-native-community/slider` 5.2.0, version recommandée par la documentation Expo SDK 57. L’ajout change les dépendances JavaScript et natives de l’application, pas le firmware. Expo Go compatible SDK 57 inclut le Slider; un development client ou un binaire natif déjà installé doit être reconstruit pour y intégrer le module. Le bundling Android valide uniquement l’export JavaScript et ne remplace pas le test sur téléphone.

### Procédure de validation Android

Avec une LAMP qui annonce `brightness`, son MAIN et CORE-WIFI disponibles sur le réseau local :

1. Ouvrir la carte de la lampe et vérifier que Luminosité apparaît, que l’état confirmé inconnu/stale est correctement étiqueté et que le curseur est désactivé si la lampe ou le MAIN est hors ligne.
2. Déplacer le curseur sans le relâcher : aucun POST ne doit apparaître dans le moniteur CORE-WIFI. Relâcher sur une nouvelle valeur (par exemple 25 %) : une seule requête brightness doit partir.
3. Vérifier les états `sent`/`accepted`, puis l’ACK `executed` et la relecture de `last_confirmed_state.brightness_percent`. Répéter séparément avec 0 %, puis 100 %; ne pas toucher le curseur de nouveau tant que le résultat précédent n’est pas lu.
4. Appuyer ON/OFF pendant un suivi brightness et vérifier que les deux commandes de cette lampe sont bloquées; vérifier qu’une autre lampe reste indépendante.
5. Couper le réseau pendant une soumission : l’application doit afficher un résultat incertain, ne pas renvoyer le POST et demander une relecture avant une nouvelle commande. Vérifier que pairing et retrait V7.7 restent utilisables.

## Clôture matérielle V7.8.0 — 16 octobre 2026

L’utilisateur confirme le parcours nominal Android → API HTTP → CORE-WIFI/UART → CORE-ZIGBEE/Zigbee → MAIN → LAMP et l’effet de variation sur la LED physique de GPIO18. Le profil `lamp_c6` est désormais le seul profil LAMP maintenu; le profil expérimental `lamp_c6_pwm_test` n’est plus dans `platformio.ini`. ON/OFF reste disponible. La validation V7.7 antérieure couvre l’appairage, le retrait, le réappairage et ON/OFF; ces parcours n’ont pas été rejoués pendant l’essai de luminosité.

Cette validation concerne un MAIN et une LAMP. Elle ne couvre pas plusieurs lampes commandées simultanément, les pannes de transport, la concurrence, ni la récupération après coupure pendant une commande. Le dernier niveau PWM reste en RAM seulement : il n’est pas persisté en NVS après un redémarrage à froid de LAMP. `last_confirmed_state` décrit une confirmation logicielle, jamais une mesure électrique ou lumineuse.
