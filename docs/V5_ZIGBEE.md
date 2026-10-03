# V5 — Zigbee réel ESP32-C6

## Portée actuelle

Ce jalon remplace le stub Zigbee par un adaptateur utilisant l’ESP-Zigbee SDK officiel. Le MAIN forme le réseau; le LAMP est un routeur alimenté sur secteur qui rejoint le réseau. Le protocole métier V4 est transporté dans une trame APS dédiée. La simulation reste disponible dans les environnements existants.

```text
Application V4 (annonce, pairing, commande, ACK)
                    |
        CommunicationTransportInterface
             /                 \
 SimulationTransport      ZigbeeTransport
                                  |
                      ESP-Zigbee SDK 2.x / APS
                                  |
                             802.15.4

MAIN_LIGHT (C6, coordinator) <----> LAMP01 (C6, router)
```

Le SDK est intégré comme composant ESP-IDF via `idf_component.yml`. PlatformIO utilise `framework = espidf` sur les deux environnements C6; les autres environnements Arduino et `native` restent séparés. Un petit shim conserve `Serial`, `millis()` et `delay()` pour le code applicatif existant. La référence ESP-IDF Zigbee décrit le SDK comme la stack officielle et expose le transport de données APS, ses confirmations et ses indications [ESP-Zigbee SDK](https://docs.espressif.com/projects/esp-zigbee-sdk/en/latest/esp32c6/index.html), [guide APS](https://docs.espressif.com/projects/esp-zigbee-sdk/en/latest/esp32c6/user-guide/aps.html).

La dépendance est fixée à `espressif/esp-zigbee-lib ~2.0.4`. Le profil PlatformIO est `espressif32@7.0.0`, qui a ajouté le support ESP-IDF 6.0 [notes PlatformIO 7.0.0](https://github.com/platformio/platform-espressif32/releases/tag/v7.0.0). L’ESP-Zigbee SDK actuel recommande ESP-IDF 5.5.4 et maintient une compatibilité avec IDF 6.0 dans sa branche 2.x [guide de développement](https://docs.espressif.com/projects/esp-zigbee-sdk/en/latest/esp32c6/developing.html), [versions du SDK](https://github.com/espressif/esp-zigbee-sdk).

## Carte PlatformIO

Les deux cartes de test sont des Waveshare ESP32-C6-DEV-KIT-N8 avec module ESP32-C6-WROOM-1-N8 et 8 Mo de flash. PlatformIO n’expose pas de board ID Waveshare dédié pour ce modèle; le projet utilise l’identifiant officiel `esp32-c6-devkitc-1`, dont le profil C6 est à 160 MHz avec 8 Mo de flash. Waveshare indique que le brochage est compatible avec l’ESP32-C6-DevKitC-1 [fiche Waveshare](https://www.waveshare.com/esp32-c6-dev-kit-n8.htm), [profil PlatformIO](https://docs.platformio.org/en/latest/boards/espressif32/esp32-c6-devkitc-1.html). Les ports série seront identifiés au branchement; le GPIO de la LED externe reste à sélectionner à partir du pinout de la carte.

## Rôles Zigbee

- `main_light_c6`: Coordinator. Au premier démarrage, initialise ESP-Zigbee, forme le réseau et ouvre l’association pendant 180 secondes. Au redémarrage sur un réseau persisté, il rejoint le réseau puis rouvre cette fenêtre.
- `lamp_c6`: Router, adapté à une lampe alimentée en continu. Au premier démarrage, lance Network Steering; le SDK conserve les informations réseau pour le redémarrage.
- Les signaux BDB du SDK distinguent l’initialisation, la formation et l’association. Un échec de formation/steering déclenche une nouvelle tentative.

Le transport utilise l’APS du SDK, le profil Home Automation `0x0104`, l’endpoint `1` et le cluster applicatif `0xFC01`. Les trames sont encodées par `zigbee_message_codec.cpp`; le programme n’envoie jamais le layout mémoire C++ de `Message`. Le format comporte une version, des champs little-endian et des chaînes à longueur bornée. La requête autorise l’ACK APS et la fragmentation.

`ZigbeeTransport::isReady()` ne devient vrai qu’après le signal de réseau formé ou rejoint. Une compilation réussie ne marque donc pas le réseau prêt. Les routes courtes Zigbee apprises à la réception sont associées au Device ID logique et au hardwareId V4; le reste du code ne manipule pas d’adresses radio.

## Découverte et pairing applicatif

Après son association, LAMP envoie `DEVICE_ANNOUNCE` toutes les cinq secondes. L’annonce transporte hardwareId, rôle, état de pairing et, si présent, Device ID, parentMainId et nom. MAIN l’ajoute aux appareils découverts sans démarrer de pairing.

Le pairing nécessite une action explicite dans la console série du MAIN:

```text
pair <hardwareId> <nom>
```

Le runtime s’appuie sur les classes V4: MAIN émet `PAIR_REQUEST`, puis `PAIR_ACCEPT`; LAMP persiste son identité, puis émet `PAIR_CONFIRM`; MAIN marque l’appareil comme paired. Cette direction de `PAIR_REQUEST` conserve le comportement V4 actuellement implémenté dans `MainProvisioning`. Une annonce seule n’appelle jamais cette séquence.

Le Device ID est logique et attribué par MAIN. Le hardwareId n’est pas un compteur: il est lu depuis l’adresse IEEE 802.15.4 de l’ESP32-C6 et formaté `C6-<16 chiffres hexadécimaux>`. Le nom utilisateur, le rôle, le parentMainId et l’état de pairing restent des champs distincts.

## Persistance

`NvsProvisioningStore` implémente l’abstraction V4 `ProvisioningStore` avec des clés NVS versionnées. Il persiste Device ID, hardwareId, nom, parentMainId et pairingState. Au démarrage, `ProvisioningLamp` relit la fiche; le LAMP annonce alors son Device ID existant et ne demande pas une nouvelle attribution.

Le dataset Zigbee utilise la partition NVS `zb_storage`, initialisée par `nvs_flash_init_partition()` avant la pile Zigbee. Les données de pairing de l’application utilisent la partition NVS standard. Une erreur NVS est affichée; le firmware ne fait pas d’effacement automatique qui risquerait de supprimer un pairing existant.

MAIN reconstruit le registry à partir des annonces des lampes paired qui reviennent sur le réseau. Le dépôt V4 n’avait pas de store de registry MAIN; sa persistance NVS séparée n’est pas ajoutée dans ce jalon.

## Commande initiale

Après pairing confirmé, la console du MAIN accepte:

```text
power <deviceId> on
power <deviceId> off
```

Cette commande utilise l’action V4 `SET_LAMP_POWER`, `MessageTracker`, l’ACK, les retries et la détection de doublons existants. `SET_LAMP_BRIGHTNESS` et `SET_LAMP_AUTOMATIC` ne sont pas exposées dans cette console à ce stade.

Le contrôleur de lampe appelle un adaptateur GPIO distinct de Zigbee. Pour configurer une broche après identification de la carte, ajouter `SMART_LIGHTING_LED_GPIO` aux flags de `lamp_c6`. Sans ce flag, le firmware compile mais désactive la sortie physique et le signale explicitement. Le PWM de brightness reste à faire.

## Configuration et flash

Environnements conservés:

```text
pio test -e native
pio run -e main_light_c6
pio run -e lamp_c6
```

Après confirmation du profil de carte et des ports série:

```text
pio run -e main_light_c6 -t upload --upload-port <PORT_MAIN>
pio device monitor -b 115200 --port <PORT_MAIN>
pio run -e lamp_c6 -t upload --upload-port <PORT_LAMP>
pio device monitor -b 115200 --port <PORT_LAMP>
```

Pour un premier réseau, démarrer MAIN, puis démarrer LAMP dans la fenêtre d’association de 180 secondes. La console série du MAIN doit afficher le hardwareId de LAMP; lancer ensuite `pair ...`, attendre le Device ID, puis exécuter `power <id> on` et `power <id> off`.

## Logs attendus

Selon le rôle et l’état persistant, les lignes principales sont:

```text
[ZIGBEE] initializing
[ZIGBEE] starting
[ZIGBEE] network formed       # premier démarrage MAIN
[ZIGBEE] network joined       # LAMP associé ou redémarrage sur réseau persistant
[ZIGBEE] device connected     # MAIN reçoit l’annonce réseau d’un appareil
[DISCOVERY] DEVICE_ANNOUNCE hardwareId=...
[PAIRING] PAIRED deviceId=...
[ZIGBEE] message received
[ZIGBEE] message sent
```

## Vérification et limites

**Implémenté dans le dépôt:** intégration réelle ESP-Zigbee SDK/APS dans la cible C6; formation/steering; codecs de `Message`; callback de réception et confirmations APS; discovery et pairing branchés aux classes V4; NVS pour l’identité pairing de LAMP; console de commissioning explicite; commande `SET_LAMP_POWER` sur le chemin ACK/retry existant; GPIO configurable et encapsulée.

**Vérifié dans cet environnement:** `pio test -e native` passe avec 28 tests, dont les 23 tests V3/V4 déjà présents et les 5 tests du codec. Les builds `pio run -e main_light_c6` et `pio run -e lamp_c6` passent; les deux tables générées déclarent `zb_storage` en NVS et placent l’application à `0x20000`. Aucun test radio n’a été simulé comme test matériel. Le test radio nécessite deux ESP32-C6 physiques.

**Pas encore vérifié:** formation/join radio; réception APS; pairing et reconnexion après coupure; ACK/retry sur radio; comportement réel du port console; GPIO et LED; PWM. La communication radio et le GPIO de test restent à valider sur les deux cartes reçues. Il ne faut donc pas conclure que « Zigbee fonctionne » avant ces essais.

## Prochain test matériel

1. Brancher les deux Waveshare ESP32-C6-DEV-KIT-N8 et relever leurs ports série.
2. Pour la première mise en service, effacer la flash de chaque carte puis flasher MAIN et LAMP avec leurs environnements C6 respectifs.
3. Capturer les logs des deux côtés et vérifier `network formed`, `network joined` et `device connected`.
4. Sur MAIN, lancer explicitement `pair <hardwareId> LAMP01`; vérifier `PAIR_CONFIRM` et Device ID.
5. Redémarrer LAMP et vérifier que le même Device ID est annoncé.
6. Envoyer `power <id> on/off`; vérifier ACK/retry et, après sélection d’une broche sûre, GPIO/LED.
