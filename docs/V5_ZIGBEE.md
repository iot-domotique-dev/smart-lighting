# V5 — Référence Zigbee MAIN_LIGHTING ↔ LAMP_C6

## Statut

La liaison V5 entre MAIN_LIGHTING et LAMP_C6 a été validée sur le matériel et reste la référence fonctionnelle du projet. Les évolutions du CORE ne doivent pas modifier le runtime de provisioning V5, son codec Zigbee, ni les firmwares main_light_c6 et lamp_c6 pour contourner un problème V7.

## Architecture

~~~text
MAIN_LIGHTING (coordinateur Zigbee)
        │ Zigbee APS
        ▼
LAMP_C6 (routeur) ── GPIO18 ── lampe
~~~

MAIN_LIGHTING forme ou rejoint le PAN. LAMP_C6 rejoint ce PAN et annonce son identité. Le pairing applicatif reste explicite côté MAIN. Le message applicatif passe par Zigbee APS et le codec V5; aucune structure C++ n’est envoyée directement en mémoire.

Dans l’architecture V7, CORE-ZIGBEE rejoint le PAN existant. CORE-WIFI et l’application ne remplacent pas le lien direct de terrain MAIN_LIGHTING ↔ LAMP_C6.

## Fonction disponible

Après pairing, la console série de MAIN_LIGHTING accepte :

~~~text
power <deviceId> on
power <deviceId> off
~~~

La commande suit le mécanisme V5 d’ACK, de retry et de déduplication. LAMP_C6 applique l’état à la sortie configurée sur GPIO18. La luminosité PWM et les autres commandes d’éclairage ne font pas partie du chemin validé décrit ici.

## Profils et commandes PlatformIO

Les profils matériels sont **main_light_c6** et **lamp_c6**. Pour compiler et flasher séparément :

~~~powershell
pio run -e main_light_c6
pio run -e lamp_c6
pio run -e main_light_c6 -t upload --upload-port COMx
pio run -e lamp_c6 -t upload --upload-port COMy
pio device monitor -p COMx -b 115200
pio device monitor -p COMy -b 115200
~~~

Remplacer COMx et COMy par les ports détectés avec pio device list. Pour un premier commissioning, démarrer MAIN_LIGHTING, puis LAMP_C6 pendant la fenêtre d’association, et terminer l’affectation depuis la console du MAIN.

## Limites

La validation de ce lien ne valide pas automatiquement le transport CORE ↔ MAIN ni l’attribution des IDs V7. Ces essais sont suivis dans [CORE double C6](V7_CORE_DOUBLE_C6.md). La commande d’éclairage depuis l’API reste hors périmètre.
