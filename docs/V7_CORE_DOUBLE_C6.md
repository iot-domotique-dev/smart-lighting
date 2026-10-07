# V7 — CORE à deux ESP32-C6

## Statut

**Validation matérielle V7 clôturée le 7 octobre 2026**, d’après les captures HTTP et les confirmations de l’utilisateur. Le montage de référence comprend C6-WIFI, C6-ZIGBEE, un MAIN_LIGHTING et une LAMP_C6. L’utilisateur rapporte également **52 tests natifs réussis**. L’API reste en lecture seule.

## Répartition des rôles

Le CORE est une seule entité logique répartie sur deux cartes :

~~~text
Appareil client ── Wi-Fi/IP ── C6-WIFI
                                  │ UART local
                                  ▼
MAIN_LIGHTING ── Zigbee ── C6-ZIGBEE
      │
      └────────── Zigbee V5 ── LAMP_C6
~~~

- **C6-WIFI** possède le registre global, la racine CORE ID 1, l’attribution des IDs, la station Wi-Fi et l’API HTTP.
- **C6-ZIGBEE** participe à un seul PAN, relaie les annonces et les réponses d’identité par UART. Il ne possède pas le registre global et ne lance pas HTTP.
- **MAIN_LIGHTING ↔ LAMP_C6** conserve le chemin fonctionnel V5.
- Le VPN éventuel reliera un téléphone au réseau du domicile; il ne transporte pas CORE ↔ MAIN.

## Identités et annonces

Le contrat V7 est conservé :

- La racine CORE a l’ID logique 1.
- Le CORE calcule les IDs depuis {parentId, localId}. Ils sont stables au sein d’un CORE, mais ne sont pas globaux entre foyers.
- Une annonce répétée met à jour l’entrée existante.
- Une identité incohérente ou une collision est rejetée.
- Un MAIN reçoit son ID CORE avant d’annoncer ses propres modules avec cet ID comme parent.

C6-ZIGBEE reçoit l’annonce Zigbee du MAIN et la transmet par UART à C6-WIFI. C6-WIFI valide l’identité, actualise le registre et renvoie l’attribution ou une erreur. C6-ZIGBEE transmet la réponse au MAIN.

## UART entre les C6

Le protocole utilise des trames versionnées avec synchronisation, type, séquence, longueur explicite, payload sérialisé champ par champ et CRC-16 CCITT-FALSE. Il ne transmet aucune structure C++ en mémoire. Les types couvrent notamment HELLO, MODULE_ANNOUNCEMENT, ID_ASSIGNMENT, ACK, STATE et ERROR.

Brochage prototype par défaut, TX/RX croisés et masse commune :

| C6-WIFI | C6-ZIGBEE |
| --- | --- |
| GPIO4 TX | GPIO5 RX |
| GPIO5 RX | GPIO4 TX |
| GND | GND |

Les paramètres par défaut sont UART1 et 115200 bauds. Les broches, le port et le débit sont configurables via CORE_LINK_UART_PORT, CORE_LINK_UART_TX_PIN, CORE_LINK_UART_RX_PIN et CORE_LINK_UART_BAUD dans PlatformIO. Ne pas relier les alimentations 3,3 V des cartes entre elles.

## Profils

- core_wifi_c6
- core_zigbee_c6
- main_light_c6
- lamp_c6

C6-WIFI n’active pas le provisioning Zigbee V5. Les deux profils CORE et les profils V5 restent compilés séparément.

## Validation du 7 octobre 2026

Les relevés intermédiaires comportaient des listes vides ou des entrées offline. Après nettoyage des données d’une ancienne connexion Zigbee, l’utilisateur a confirmé le fonctionnement du montage complet et la poursuite du fonctionnement lors des contrôles finaux. La méthode exacte d’effacement n’a pas été consignée.

Depuis un PC du même Wi-Fi, les routes `/api/v1/health`, `/api/v1/core`, `/api/v1/modules` et `/api/v1/devices` ont toutes répondu HTTP 200. La capture finale montre :

| Entrée | ID global CORE | Parent | Présence |
| --- | --- | --- | --- |
| MAIN_LIGHTING | `4074601247` | CORE `1` | `online: true`, `status: online` |
| lamp001 | `3559985816` | MAIN `4074601247` | `online: true`, `status: online` |

Les collections contiennent chacune une entrée (`count: 1`). Ces IDs sont ceux du montage testé; ils ne sont pas à imposer à d’autres cartes.

| Contrôle | Résultat consigné |
| --- | --- |
| Wi-Fi et API CORE-WIFI | Connexion DHCP et quatre routes GET en HTTP 200. |
| UART entre les C6 | `[CORE-ZIGBEE] UART peer ready` observé. |
| Annonces Zigbee et attribution globale | MAIN et lampe enregistrés, avec la hiérarchie CORE → MAIN → lampe correcte. |
| Présence dans le registre | MAIN et lampe online dans la capture finale. |
| Conservation du lien V5 | Commandes `power` testées depuis MAIN, ACK reçu et fonctionnement de la lampe confirmé par l’utilisateur. |
| Poursuite du fonctionnement | L’utilisateur confirme que le montage fonctionne toujours lors des contrôles finaux. |

Les réponses détaillées sont dans [API CORE V7.1](V7_1_CORE_API.md#résultats-des-tests-réseau-sur-carte). Les champs `state: null` sont attendus : l’état électrique de la lampe n’est pas transmis au registre global. La commande `power` de la console MAIN utilise le **deviceId V5 local** de la lampe, distinct de son ID global dans l’API.

### Tests natifs

L’utilisateur rapporte **52 tests natifs réussis** avec :

~~~powershell
pio test -e native
~~~

La suite `test_core_link` couvre notamment l’attribution d’ID, les annonces répétées sans doublon et le rejet d’une identité incohérente. Le rejet d’identité est documenté comme une validation logicielle; aucune injection d’identité incohérente sur les cartes n’a été consignée. Les tests natifs complètent les essais matériels. Leur périmètre est décrit dans le [README des tests](../test/README).

## Diagnostic Zigbee et UART

### Ancienne connexion Zigbee

Le blocage initial de cette validation provenait d’un ancien état réseau Zigbee conservé sur les cartes. Le nettoyage de ces données a permis aux annonces du montage actuel d’alimenter le registre CORE. Un simple redémarrage conserve les données réseau et ne réalise pas cette réinitialisation.

Le message `[ZIGBEE] network joined` est affiché après une association réussie, mais aussi après une initialisation réussie lorsque la carte possède déjà un état réseau enregistré. Il ne vérifie pas explicitement un échange avec MAIN et n’affiche pas l’identifiant du PAN. Il peut donc apparaître lorsque CORE-ZIGBEE est seul sous tension.

En cas de listes API vides malgré un UART opérationnel, vérifier que MAIN utilise `main_light_c6`, qu’il est allumé et que CORE-ZIGBEE rejoint son PAN actuel. Si un ancien réseau est en cause, réinitialiser les données réseau de la carte concernée puis refaire son association au PAN du MAIN. L’effacement complet d’une carte peut aussi supprimer son association applicative V5; la lampe devra alors être appairée de nouveau.

### Logs attendus

- `[CORE-WIFI] UART link: ready` et `uart_driver_ready: true` dans health confirment l’initialisation du pilote UART local.
- `[CORE-ZIGBEE] UART peer ready` confirme la réponse HELLO/ACK de CORE-WIFI. HELLO est envoyé une seule fois au démarrage de CORE-ZIGBEE : démarrer CORE-WIFI d’abord, puis redémarrer CORE-ZIGBEE si nécessaire pour observer cette réponse.
- `[ZIGBEE] message received`, puis `>>> MESSAGE RECU`, indiquent la réception et le traitement d’une trame applicative Zigbee valide.
- `[CORE-WIFI] module id=...` indique l’enregistrement ou la mise à jour d’une annonce dans le registre global.
- `[V7] MAIN CORE id=...` sur MAIN indique qu’il a reçu son attribution d’ID.

CORE-ZIGBEE ne produit pas de log spécifique pour chaque relais UART réussi. Avec cette carte seule, le silence après le démarrage est attendu. Avec MAIN prêt sur le même PAN, ses annonces sont tentées toutes les cinq secondes et leur réception doit produire les logs Zigbee ci-dessus. Certaines trames rejetées par les filtres APS sont ignorées sans détail de diagnostic.
