# Smart Lighting Mobile — V7.5.2

Application Expo SDK 57 / React Native / TypeScript pour consulter CORE-WIFI et les lampes du réseau local, puis les piloter individuellement en ON/OFF avec le suivi HTTP V7 existant.

Le bilan de clôture, la première validation matérielle Android et les scénarios restant à vérifier sont dans le [dossier V7.5](../../docs/V7_5_MOBILE_APP.md).

## Préparer et démarrer

Prérequis : Node.js 22.13 ou plus récent, npm et un téléphone Android relié au même réseau local que le PC et CORE-WIFI. Expo SDK 57 utilise React Native 0.86 et exige cette version minimale de Node.js.

Depuis la racine du dépôt, dans PowerShell :

```powershell
cd apps/mobile
npm ci
npx expo start --go --lan
```

Expo Go SDK 57 est maintenant disponible dans les magasins officiels. Mets Expo Go a jour sur le telephone. Le projet et Expo Go doivent utiliser le meme SDK.
Sur Android, si le magasin ne propose pas encore cette version sur ton appareil, telecharge-la avec :
```powershell
npx expo-go download android 57
```
Sur iPhone, Expo Go SDK 57 est disponible dans l'App Store. Pour ouvrir un projet en developpement, connecte Expo Go et le terminal au meme compte Expo (commande npx expo login). Le sideload d'une ancienne version d'Expo Go n'est pas pris en charge sur iPhone.
Pour appliquer de façon fiable la configuration native du trafic HTTP local, utiliser un build de développement. Après installation de l’Android SDK et activation du débogage USB sur le téléphone :

```powershell
cd apps/mobile
npm ci
npx expo run:android --device
npx expo start --dev-client --lan
```

Le premier build crée les dossiers natifs ignorés par Git. Le réglage Android `usesCleartextTraffic` et l’exception iOS pour le réseau local sont déclarés dans `app.json`; Expo Go utilise sa propre configuration native. Aucun build de développement n’a été installé sur téléphone pendant V7.5.1.

## Connexion au CORE-WIFI

1. Sur le téléphone, rester sur le Wi-Fi local qui peut joindre CORE-WIFI. Éviter les réseaux invités qui isolent les appareils.
2. Saisir **uniquement l’IPv4 privée** du CORE-WIFI, sans `http://` ni port. L’adresse apparaît dans le moniteur série CORE-WIFI après connexion.
3. Saisir le jeton Bearer configuré localement pour CORE-WIFI. Il est conservé dans `expo-secure-store` sur le téléphone et n’est jamais inclus dans le dépôt ni dans la configuration distribuée.
4. Toucher **Tester la connexion** pour vérifier `/health` et `/core`, puis **Enregistrer et continuer**. Le test HTTP ne vérifie pas le jeton ; les routes de lecture sont publiques. Le jeton stocké dans SecureStore autorise les commandes et leur suivi.
5. La liste charge `/health`, `/modules` et `/devices`. Toucher une lampe pour obtenir `/devices/{id}`. **Actualiser** ou tirer la liste vers le bas pour recharger immédiatement.

L’actualisation automatique se fait toutes les 20 secondes pendant que l’application est active. Chaque requête a un délai de 5 secondes. En cas de perte réseau, la dernière liste reste visible avec un avertissement ; elle n’est plus présentée comme une disponibilité actuelle.

`last_confirmed_state` décrit la dernière commande ON/OFF confirmée par ACK : `unknown`, `confirmed` ou `stale`. Il ne mesure pas l’état électrique de la lampe. Une lampe hors ligne peut conserver un historique `stale`. Le champ API existant `state:null` reste inchangé.

## Piloter les lampes

Chaque carte propose **Allumer** et **Éteindre**. Un seul POST est envoyé par appui à `/devices/{id}/commands/power`; en cas de perte de réponse, l’application affiche un résultat incertain et ne renvoie jamais automatiquement ce POST. Elle conserve l’ID, la lampe, la demande et les états de suivi en mémoire pendant l’exécution.

Après HTTP `202`, le client suit `/commands/{id}` avec un délai progressif, jusqu’à `executed`, `failed`, `expired` ou un délai mobile maximal de 30 secondes. `sent` et `accepted` restent en cours. Après un état terminal, la fiche lampe est relue depuis `/devices/{id}`. L’affichage ON/OFF reste fondé sur `last_confirmed_state` et n’est jamais modifié optimistement par l’appui.

Une commande en cours désactive les deux boutons de cette lampe; une autre lampe reste pilotable en parallèle. Les boutons sont également désactivés si la lampe, son MAIN ou la fraîcheur de l’inventaire ne permettent pas de confirmer sa disponibilité. Modifier l’adresse ou le jeton CORE attend la fin du suivi. Le retour au premier plan reprend les lectures de statut. Après fermeture complète de l’application ou redémarrage du CORE, le résultat peut rester inconnu; aucune nouvelle commande n’est rejouée automatiquement.

`executed` confirme l’exécution logicielle par ACK, pas un état électrique mesuré. `expired`, `command_not_found`, une réponse POST perdue et un délai de suivi sont présentés comme incertains; l’application relit l’état connu, sans prétendre prouver quelle commande l’a produit.

## Structure

- `App.tsx` : écran unique, configuration ou inventaire.
- `src/api/` : types et client HTTP de lecture, commande et suivi.
- `src/settings/` et `src/security/` : adresse et jeton conservés via SecureStore.
- `src/features/core/` : configuration et test du CORE.
- `src/features/lighting/` : liste, détail, commandes d’éclairage et suivi progressif.
- `src/components/` : indicateur visuel partagé.
- `test/` : tests du client API avec réponses simulées.

## Vérifications locales

```powershell
npm run lint
npm run typecheck
npm run test:api
npx expo install --check
npx expo config --type public
npx expo export --platform android
```

Pour V7.5.2, le typage, le lint, les 15/15 tests simulés, la vérification des dépendances SDK 57 et l’export Android ont réussi. Le premier essai nominal ON/OFF depuis Android a aussi été confirmé sur une LED de test raccordée à GPIO18; les scénarios matériels restant à vérifier sont listés dans le [dossier V7.5](../../docs/V7_5_MOBILE_APP.md). L’API actuelle utilise HTTP sans TLS : le port 80 du CORE ne doit pas être publié sur Internet.

La migration SDK 57 réduit les avis rapportés par npm, mais `npm audit fix --force` n’est pas appliqué automatiquement : ses propositions peuvent changer les versions majeures. La mise à niveau vers un SDK ultérieur fera l’objet d’une validation séparée.
