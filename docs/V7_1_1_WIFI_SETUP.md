# V7.1.1 — Wi-Fi de CORE-WIFI : configuration et essai

## Identifiants locaux

À la racine du dépôt, dans PowerShell :

~~~powershell
Copy-Item include/wifi_credentials.example.h include/wifi_credentials.h
notepad include/wifi_credentials.h
~~~

Renseigner ces deux macros dans include/wifi_credentials.h :

~~~cpp
#define SMART_LIGHTING_WIFI_SSID "NomDuReseau"
#define SMART_LIGHTING_WIFI_PASSWORD "MotDePasseDuReseau"
~~~

Ce fichier local est ignoré par Git. Ne pas mettre les identifiants dans le fichier .example.h, dans platformio.ini ou dans les sources versionnées. Le mot de passe n’apparaît pas dans les logs série.

Le CORE-WIFI utilise le Wi-Fi 2,4 GHz et DHCP. Le serveur HTTP démarre même si le réseau est indisponible ou si les identifiants manquent; le firmware retente automatiquement la connexion.

## Compiler, flasher et ouvrir le moniteur

Fermer d’abord tout moniteur déjà ouvert sur le port de la carte. Remplacer COMx par le port relevé après connexion de CORE-WIFI :

~~~powershell
pio device list
pio run -e core_wifi_c6
pio run -e core_wifi_c6 -t upload --upload-port COMx
pio device monitor -p COMx -b 115200
~~~

Après modification des credentials, recompiler et reflasher.

## Logs attendus

Les messages utiles sont :

~~~text
[CORE-WIFI] Wi-Fi connection attempt, SSID: NomDuReseau
[CORE-WIFI] Wi-Fi connected, SSID: NomDuReseau
[CORE-WIFI] IP address: adresse attribuée par DHCP
[CORE-WIFI] HTTP API: ready on port 80
~~~

L’ordre des logs de connexion et de démarrage HTTP peut varier. En cas de perte du réseau, le moniteur affiche la déconnexion puis une nouvelle tentative. Le mot de passe ne doit jamais apparaître.

Copier l’adresse de la ligne IP address après chaque démarrage ou reconnexion : DHCP peut fournir une autre adresse. Ne pas réutiliser une ancienne IP sans la vérifier.

## Tester l’accès local

Le PC ou le téléphone doit être sur le même réseau local, sans isolation des clients. Dans PowerShell, remplacer l’exemple par l’adresse IP la plus récente affichée par le CORE :

~~~powershell
$CoreIp = "192.168.1.42"
Test-NetConnection -ComputerName $CoreIp -Port 80
curl.exe -i "http://$CoreIp/api/v1/health"
curl.exe -i "http://$CoreIp/api/v1/core"
curl.exe -i "http://$CoreIp/api/v1/modules"
curl.exe -i "http://$CoreIp/api/v1/devices"
~~~

Une valeur TcpTestSucceeded: True confirme l’accès TCP au port 80. Les routes de lecture répondent 200. Une liste vide est normale avant l’enregistrement des MAIN et appareils dans le registre CORE. Les formats détaillés et les erreurs HTTP sont dans [API locale V7.1](V7_1_CORE_API.md).

Lors de la validation complète du 7 octobre 2026, les collections ont retourné un MAIN et une lampe online (`count: 1` chacune). Si les listes restent vides alors que les cartes sont allumées, vérifier le trajet Zigbee/UART et l’appartenance au PAN du MAIN. Le nettoyage d’un ancien état réseau Zigbee a résolu ce blocage pendant l’essai; voir le [diagnostic du CORE double C6](V7_CORE_DOUBLE_C6.md#diagnostic-zigbee-et-uart).

Si la connexion échoue, vérifier l’IP dans les logs série, puis lancer ipconfig sur le PC. Celui-ci doit pouvoir joindre la même plage réseau; un réseau Wi-Fi invité ou l’option d’isolation des clients du routeur peut empêcher la connexion.

Les routes de lecture de l’API restent en HTTP local sans authentification; les routes V7.3 de commande exigent le Bearer token configuré dans `include/wifi_credentials.h`. Les échanges ne sont pas chiffrés. Ne pas rediriger le port 80 du routeur vers Internet. Voir la [configuration et les exemples V7.3](V7_3_HTTP_COMMANDS.md).
