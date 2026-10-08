# V7.5 — Application mobile Smart Lighting

## Objectif et statut

V7.5 apporte le premier client mobile de Smart Lighting. L’application Android, construite avec React Native, TypeScript et Expo SDK 57, est l’interface d’une plateforme domotique modulaire. À ce stade, seul le domaine de l’éclairage est implémenté.

- **V7.5.1 — consultation :** connexion au CORE-WIFI, inventaire des MAIN et des lampes, disponibilité et dernier état confirmé.
- **V7.5.2 — pilotage :** commandes ON/OFF, suivi de leur exécution et présentation prudente des résultats incertains.

## Architecture

```text
Application Android
        │ HTTP local
        ▼
CORE-WIFI ── UART ── CORE-ZIGBEE ── Zigbee ── MAIN_LIGHTING ── Zigbee ── LAMP_C6 ── GPIO18
```

L’application parle à CORE-WIFI en HTTP sur le réseau local. Elle ne communique pas directement avec les cartes Zigbee. Le CORE-WIFI relaie les commandes par UART au CORE-ZIGBEE, qui les transmet sur Zigbee au MAIN puis à la lampe.

## V7.5.1 — Consultation

L’application permet de configurer l’adresse IPv4 locale de CORE-WIFI et le jeton Bearer. Le jeton est conservé sur le téléphone avec `expo-secure-store`; il n’est pas intégré au code ni distribué avec l’application.

Après connexion, l’écran consulte l’état du CORE, puis les modules MAIN et les lampes. Il affiche leur disponibilité (`online` / `offline`) et `last_confirmed_state`. L’inventaire peut être actualisé manuellement et périodiquement lorsque l’application est au premier plan.

## V7.5.2 — Pilotage ON/OFF

Pour chaque lampe disponible, l’application envoie une commande à la route existante `POST /api/v1/devices/{id}/commands/power`, avec authentification Bearer. Elle conserve l’ID de commande et suit `GET /api/v1/commands/{id}` par polling à délai progressif. Les états suivis sont `sent`, `accepted`, `executed`, `failed` et `expired`.

Après un état terminal, l’application relit l’équipement. Une seule commande à la fois peut être suivie par lampe; des lampes différentes peuvent être commandées en parallèle. Si la réponse au POST est perdue ou si le résultat devient incertain, l’application ne renvoie pas automatiquement la commande. Elle reprend les lectures de suivi quand elle revient au premier plan, tant que la session et l’ID sont encore disponibles; elle ne restaure pas un suivi après fermeture complète.

L’affichage ON/OFF reste fondé sur `last_confirmed_state`, et non sur l’appui ou la valeur demandée. `executed` est un ACK d’exécution logicielle; il ne constitue pas une mesure électrique. Une expiration ou une ambiguïté peut laisser un état `stale` même si une autre commande s’exécute ensuite.

## Validation logicielle consignée

Les vérifications consignées pour V7.5.2 sont :

- vérification TypeScript réussie;
- lint réussi;
- **15/15 tests mobiles simulés réussis**;
- vérification de compatibilité des dépendances Expo SDK 57 réussie;
- export du bundle Android réussi;
- `git diff --check` réussi.

Ces résultats logiciels sont rapportés tels qu’ils ont été consignés; ils n’ont pas été relancés pour cette clôture documentaire.

## Validation matérielle Android — 9 octobre 2026

Sur la base du retour utilisateur, l’application Android Expo SDK 57 a commandé ON et OFF via CORE-WIFI sur le réseau local. Le montage rapporté comprenait CORE-WIFI, CORE-ZIGBEE, un MAIN_LIGHTING et une LAMP_C6; une LED de test raccordée à GPIO18 sur LAMP_C6 a réagi aux commandes. Le trajet nominal application → HTTP → CORE-WIFI → UART → CORE-ZIGBEE → Zigbee → MAIN_LIGHTING → Zigbee → LAMP_C6 → GPIO18 est donc **validé fonctionnellement sur une sortie matérielle de test, selon le retour utilisateur**.

Aucun ID de commande, journal matériel détaillé, mesure électrique ni temps de réponse mesuré n’est archivé pour cet essai. La réaction décrite comme rapide est un retour qualitatif, pas une mesure chronométrée.

## Limites restant à vérifier

- Deux lampes physiques commandées simultanément derrière le même MAIN n’ont pas été validées.
- Les déconnexions réseau depuis l’application n’ont pas été vérifiées sur matériel.
- L’expiration et le résultat incertain n’ont pas été vérifiés de bout en bout depuis Android.
- Les commandes après redémarrage du CORE n’ont pas été vérifiées depuis Android.
- Aucun accès distant sécurisé ni accès Internet au CORE n’est implémenté.
- Aucun support multi-MAIN matériel n’est validé.
- Le contrôle physique de luminosité n’est pas implémenté.
- `last_confirmed_state` reste un historique logiciel; aucune mesure de l’état électrique de la lampe n’est disponible.

## Sécurité et sortie matérielle

Les échanges HTTP sont locaux et ne sont pas chiffrés par TLS. Le jeton est gardé localement avec SecureStore. Le port HTTP du CORE ne doit pas être publié sur Internet; utiliser uniquement un réseau de confiance.

GPIO18 est une sortie logique. Elle ne doit pas alimenter directement une charge secteur ou de puissance; employer une interface de puissance adaptée au matériel commandé.

## Références

- [README du projet](../README.md)
- [Guide de démarrage mobile](../apps/mobile/README.md)
- [V7.4 — routage multi-lampes et état confirmé](V7_4_RELIABILITY.md)
- [V7.3 — commande HTTP locale](V7_3_HTTP_COMMANDS.md)
