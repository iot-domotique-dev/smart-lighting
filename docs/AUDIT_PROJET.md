> Archive: cet audit décrit un état antérieur au nettoyage pré-V7. Les profils `lamp`, `lamp_a` et `lamp_b` et la simulation Wokwi cités ci-dessous ont depuis été supprimés; `lamp_c6` est le firmware de lampe maintenu.

﻿# Audit du projet Smart Lighting

**Date :** 25 septembre 2026  
**Périmètre :** sources C++, en-têtes, configuration PlatformIO, simulation Wokwi, tests et documentation du dépôt.

## Synthèse

Le dépôt contient une base logicielle modulaire pour un prototype ESP32 : registres à capacité fixe, commandes de lampes et groupes, scènes, évaluateur d’automatisations, bus d’événements, suivi des messages et transport simulé. Les responsabilités sont réparties dans des modules séparés, et l’interface de transport prépare un remplacement futur.

**État après corrections : V3.9 stabilisée en simulation.** Les corrections ciblées pour les profils, le suivi des messages, la déduplication, la luminosité et les capacités d’automatisation sont appliquées. Les six profils ESP32 compilent et les neuf tests PlatformIO natifs passent. Le raccordement des événements reste partiel pour les automatisations sans capteurs. Le transport reste `SimulationTransport` ; aucun réseau Zigbee, ESP32-C6 ni pilotage physique n’a été validé.

Les builds sont : `pio run -e core -e main -e lamp -e relay -e lamp_a -e lamp_b`. Les tests sont : `pio test -e native` (9/9). La machine ne fournissait pas `gcc`/`g++` sur le PATH ; l’exécution native a utilisé des wrappers temporaires vers le Clang C++ déjà installé avec Visual Studio. Le profil natif standard requiert donc un compilateur hôte compatible GCC.

## Constats initiaux et état après correction

Les problèmes ci-dessous sont conservés comme historique de l’audit ; chaque section indique leur état actuel.

### A-01 — Profils `lamp_a` et `lamp_b` sans rôle reconnu — corrigé côté configuration

**Priorité : élevée — compilation bloquée pour ces profils.**

Les environnements `lamp_a` et `lamp_b` ne définissent respectivement que `DEVICE_LAMP_A` et `DEVICE_LAMP_B` dans [platformio.ini](../platformio.ini). Or [roles.h](../include/roles.h) exige l’un des symboles `DEVICE_ROLE_CORE`, `DEVICE_ROLE_MAIN`, `DEVICE_ROLE_LAMP` ou `DEVICE_ROLE_RELAY` et déclenche une erreur de préprocesseur sinon. Ces deux environnements échouent donc à la compilation dès qu’un fichier inclut `device.h`.

En outre, `DEVICE_ROLE` n’est pas consommé dans les autres sources : les profils `core`, `main`, `lamp` et `relay` sélectionnent une constante de rôle, mais ne sélectionnent pas de comportement de firmware distinct. Tous héritent du même `board = esp32dev`.

**Correction à l'époque :** les deux profils définissaient `DEVICE_ROLE_LAMP`. `DEVICE_LAMP_A` et `DEVICE_LAMP_B` déterminaient les identifiants locaux 1 et 2. Ces profils et leur logique d'identité ont été retirés au nettoyage pré-V7; `lamp_c6` est la seule cible lampe actuelle.

### A-02 — Saturation définitive du suivi après vingt messages — corrigé

**Priorité : élevée — limite d’exploitation.**

Le tracker dispose de vingt entrées fixes. [trackMessage()](../src/message_tracker.cpp) incrémente `tracker.count`, tandis que le traitement ACK et les timeouts marquent les entrées terminées sans réduire ce compteur. Aucune fonction de retrait ou de réutilisation d’une entrée terminée n’existe. Après vingt messages suivis au cours d’un démarrage, le message suivant est refusé, même si les précédents ont tous reçu leur ACK. De plus, [sendCommand()](../src/main.cpp) enregistre l’entrée avant l’envoi et ne l’annule pas si l’envoi échoue.

**Correction :** lorsqu’il est plein, le tracker réutilise une entrée qui n’attend plus d’ACK. `untrackMessage()` permet de libérer l’entrée si l’envoi initial échoue. La limite de vingt messages simultanément en attente est conservée ; les tests couvrent le rejet lorsque les vingt entrées attendent encore, l’ACK, les retries, le timeout et la réutilisation.

### A-03 — Boucle applicative sans événements ni surveillance d’appareils — partiellement corrigé

**Priorité : élevée — les fonctionnalités ne sont pas actives dans l’application lancée.**

Dans [main.cpp](../src/main.cpp), les registres d’automatisations et le bus d’événements sont initialisés, mais aucun scénario d’automatisation n’est ajouté. La boucle [loop()](../src/main.cpp) traite la communication et les timeouts des messages ; elle n’appelle ni `processEvents()` ni `updateDeviceStatus()`. La revue des références montre que `processEvents()`, `updateDeviceStatus()` et `updateDeviceSeen()` ne sont appelés nulle part dans `src/`. Le bus d’événements reste donc vide et le statut ONLINE/OFFLINE n’évolue pas en fonctionnement normal.

Les implémentations métier existent dans des modules séparés, mais l’exécutable courant ne les orchestre pas. Les commandes de lampe modifient l’état en mémoire et écrivent sur `Serial` ; aucune commande GPIO/PWM n’est présente.

**Correction :** les scénarios et les dispositifs factices sont maintenant compilés uniquement pour le profil `lamp` de Wokwi (`SMART_LIGHTING_DEMO`). La boucle applicative traite la communication, les timeouts, les statuts de présence et `processEvents()`. La réception d’un message d’un appareil connu appelle `updateDeviceSeen()` ; les transitions ONLINE/OFFLINE publient des événements. **Limite restante :** l’EventBus ne reçoit actuellement que les événements de statut créés par le gestionnaire d’appareils. Le processeur les journalise, mais ne lance pas le moteur d’automatisation pour ces types ; aucun pilote ne publie encore les événements de capteur et ne met à jour le contexte d’automatisation. L’orchestration est donc raccordée, mais les automatisations déclenchées par des capteurs ne sont pas encore utilisables en service autonome. Aucun événement artificiel n’a été ajouté.

### A-04 — Déduplication sans identité d’expéditeur — corrigé

**Priorité : élevée avant une communication multi-appareils.**

Le générateur [generateMessageId()](../src/message_id_generator.cpp) utilise un compteur local qui commence à 1 dans chaque firmware. Le déduplicateur [recherche uniquement `messageId`](../src/message_deduplicaor.cpp), sans inclure `sourceId`. Deux appareils qui envoient chacun un message d’identifiant 1 au même récepteur peuvent donc être confondus : le deuxième message sera traité comme un doublon et sa commande ne sera pas exécutée. Le cache remplace aussi les identifiants les plus anciens après vingt entrées ([message_deduplicator.cpp](../src/message_deduplicaor.cpp)), ce qui permet à une retransmission tardive d’être rejouée après éviction.

**Correction :** le cache utilise maintenant la paire `(sourceId, messageId)` et le routeur transmet l’expéditeur aux fonctions de recherche et d’enregistrement. Le cache circulaire conserve vingt entrées et évince la plus ancienne par ordre d’insertion. Les tests vérifient que deux sources peuvent employer le même ID, qu’un doublon de la même source est ignoré et que l’éviction libère bien l’ancienne paire. Le cache reste volatil et borné à vingt entrées.

### A-05 — Réduction à 8 bits avant validation de la luminosité — corrigé

**Priorité : moyenne — commandes hors plage appliquées de façon inattendue.**

Le protocole stocke la valeur en `int32_t`, mais [setLampBrightness()](../src/lamp_controller.cpp) reçoit un `uint8_t` et ne borne qu’ensuite la valeur à 100. Les appels depuis [action_executor.cpp](../src/action_executor.cpp) et [group_command_handler.cpp](../src/group_command_handler.cpp) transmettent un `int32_t` sans validation préalable. La conversion peut donc changer la valeur avant le contrôle : `-1` devient typiquement `255`, puis `100` ; `256` devient `0`.

**Correction :** `setLampBrightness()` reçoit maintenant `int32_t` et applique une saturation explicite : toute valeur négative devient 0 %, toute valeur supérieure à 100 devient 100 %. La conversion vers `uint8_t` survient après ce contrôle. Les tests couvrent les deux bornes hors plage et un chemin via `executeAction()`.

### A-06 — Compteurs d’automatisation non vérifiés — corrigé

**Priorité : moyenne — accès hors limites si un enregistrement mal formé est fourni.**

[addAutomation()](../src/automation_manager.cpp) vérifie la capacité du registre et l’unicité de l’identifiant, puis copie la structure sans vérifier `conditionCount` et `actionCount`. Ces champs indexent des tableaux fixes de cinq éléments définis dans [automation.h](../include/automation.h). [processAutomations()](../src/automation_engine.cpp) et sa boucle d’actions parcourent directement les compteurs fournis. Une structure construite avec un compteur supérieur à la taille du tableau peut donc entraîner une lecture hors limites.

**Correction :** `addAutomation()` refuse les compteurs dépassant les tableaux fixes et remet `conditionState`, `conditionSince` et `lastTriggered` à leur état initial avant copie. Les tests couvrent les deux limites et la remise à zéro.

### A-07 — Absence de suite de tests reproductibles — partiellement corrigé

**Priorité : moyenne — régressions difficiles à détecter.**

La section V3.9 du [README de `docs`](README.md) indique que les tests de transmission, retransmission et déduplication sont validés. Le dossier `test/` ne contient toutefois que son fichier explicatif `README`, sans cas de test. Les scénarios sont codés en dur dans le `setup()` et le `loop()` de [main.cpp](../src/main.cpp) et [main.cpp](../src/main.cpp) ; ils ne forment pas une suite PlatformIO isolée. Aucun workflow CI n’est présent dans le dépôt.

**Correction :** `test/test_stabilization/test_main.cpp` ajoute neuf tests Unity exécutés par `pio test -e native`. Ils couvrent registres, luminosité, ACK/retry/timeout, tracker, déduplication et éviction, compteurs d’automatisation, `ONCE`/`REPEAT` et événements de statut. Résultat vérifié : 9/9. Les démonstrations Wokwi restent dans `lamp`. **Reste à faire :** le dépôt ne contient toujours pas de workflow CI. Le profil natif PlatformIO attend un compilateur C/C++ hôte ; sur cette machine, les tests ont été exécutés avec des wrappers temporaires vers Clang pour compenser l’absence de `gcc`/`g++` sur le PATH.

### A-08 — Document de reprise principal périmé — corrigé

**Priorité : faible — risque de reprise sur un état périmé.**

[docs/README.md](README.md) décrit V3.9, tandis que [PROJECT_CONTEXT.md](PROJECT_CONTEXT.md) présente une dernière fonctionnalité et un dernier test validés en V2.7. [PROJECT_CONTEXT_V2.9.md](PROJECT_CONTEXT_V2.9.md) est bien identifié comme un instantané V2.9 ; le fichier `PROJECT_CONTEXT.md`, lui, ne prévient pas dans son titre qu’il s’agit d’un contexte historique.

**Correction :** [PROJECT_CONTEXT.md](PROJECT_CONTEXT.md) signale maintenant explicitement qu’il s’agit de l’archive V2.7 et renvoie vers le README courant et cet audit. Le README indique V3.9 stabilisée en simulation ainsi que les builds et tests réellement exécutés.

## Limites actuelles connues du prototype

Ces éléments sont annoncés comme futurs dans la documentation ; ils ne sont pas traités ici comme des fonctionnalités promises déjà livrées.

- [ZigbeeTransport](../src/zigbee_transport.cpp) ne démarre pas de réseau : `begin()` retourne `false` et `send()`/`receive()` ne sont pas implémentés.
- La configuration cible actuellement `esp32dev` dans [platformio.ini](../platformio.ini), tandis que la documentation prévoit un ESP32-C6 pour l’étape matérielle suivante.
- Le diagramme Wokwi (supprimé au nettoyage pré-V7) ne contenait qu’une carte ESP32 ; il ne modélisait ni lampe commandée ni capteur.
- Aucune validation de source ou de destination, protection contre la falsification au niveau applicatif, gestion de clés ou politique d’autorisation n’est visible dans le routeur de commandes. Le transport réel et ses mécanismes de sécurité restent à concevoir et à vérifier avant toute commande sur réseau.
- En dehors de `SMART_LIGHTING_DEMO`, `lamp_a` et `lamp_b` ne créent pas encore d’entrée locale dans `lampRegistry`. Leurs identifiants de communication sont distincts, mais l’état de lampe local et son pilotage matériel ne sont pas implémentés.
- Les événements ONLINE/OFFLINE sont produits et traités comme des journaux. Les événements de capteurs qui déclencheraient `processAutomations()` n’ont pas encore de producteur dans le firmware applicatif.

## Étapes restant avant l’intégration matérielle

1. Définir les sources physiques et le format de mise à jour de `AutomationContext`.
2. Choisir la carte ESP32-C6 et les circuits de sortie avant d’ajouter le pilotage matériel.
3. Concevoir le commissioning, les autorisations et la sécurité réseau pour Zigbee.
4. Ajouter un workflow CI qui installe un compilateur hôte, lance `pio test -e native` et compile les six profils ESP32.

## Conclusion

La version proposée après correction est **V3.9 stabilisée (simulation)**. Les corrections ciblées pour les profils, le tracker, la déduplication, la validation de luminosité et les limites d’automatisation sont vérifiées par build et tests. La surveillance des statuts de présence est raccordée, mais les entrées de capteurs et les déclenchements d’automatisations en service autonome restent à intégrer. Le projet reste un prototype local : `ZigbeeTransport` ne démarre pas de réseau, les firmwares ciblent actuellement `esp32dev`, et les lampes n’ont pas de pilotage électrique. Ne pas présenter cette version comme intégration ESP32-C6/Zigbee.
