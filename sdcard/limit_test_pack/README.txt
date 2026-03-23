Dead End Inc. - Hyper Mod Limit Test

Ce dossier est un pack de test pour pousser le firmware actuel sans le modifier.

Copie le dossier `deadend` contenu ici a la racine de la carte SD
en fusionnant avec le pack principal existant.

Structure a copier :
/deadend/mods/products/limit_products.txt
/deadend/mods/districts/limit_districts.txt
/deadend/mods/characters/limit_contacts.txt
/deadend/mods/events/limit_events.txt

Ce pack remplit les limites actuelles du moteur :
- produits : 8 total (3 de base + 5 mods)
- districts : 12 total (4 de base + 8 mods)
- contacts : 16 total (5 de base + 11 mods)
- events : 32 total (6 de base + 26 mods)

Objectif :
- verifier que les listes dynamiques et la pagination fonctionnent
- verifier que le moteur charge bien tous les mods
- verifier que les limites ne cassent pas l'UI

Note honnete :
Le firmware actuel accepte bien jusqu'a 8 produits, mais le format des districts
ne decrit explicitement que les 3 colonnes de demande historiques.
Les produits ajoutes restent chargeables et jouables, mais leur demande par district
sera moins nuancee dans cette build.
