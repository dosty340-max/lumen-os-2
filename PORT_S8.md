# Préparation du portage Galaxy S8 dreamlte

Le profil ARM64 se compile sur l’hôte. Le démarrage, l’affichage et les entrées sur le Galaxy S8 restent à vérifier. Les caractéristiques ci-dessous proviennent du relevé fourni.

| Élément | Relevé |
| --- | --- |
| Modèle et SoC | Samsung Galaxy S8 SM-G950F, dreamlte, Exynos 8895 ARM64 |
| Système | Android 9, Linux 4.4.111, firmware G950FXXUCDZE7 |
| Écran | 1440 × 2960 physiques, override Android de 1080 × 2220 |
| Tactile | `sec_touchscreen`, `/dev/input/event0`, dix contacts, axes 0 à 4095 |
| Boutons | `/dev/input/event6` |
| Affichage Linux | `fb0` à `fb2` signalés, aucun DRM visible dans le relevé |
| Démarrage | Bootloader verrouillé |

## Architecture et choix

- `src/drm.c` utilisait directement `/dev/dri/card0`, les dumb buffers et les page flips. QEMU conserve ce backend. `src/main.c` appelle désormais l’interface de présentation choisie à l’édition de liens.
- `src/gfx.c` dessine en ARGB8888 dans un canevas compact de 720 × 1560. Le backend expérimental `src/fbdev.c` prend la résolution visible, le stride et les offsets des ioctls fbdev. Il met le canevas à l’échelle sur toute la surface visible, avec un léger étirement dû à la différence de proportions. L’override Android n’entre pas dans ce calcul. Le backend accepte uniquement le XRGB8888 ou l’ARGB8888 packed true-color dans une orientation non tournée, et ne change pas le mode ni la page affichée. Il essaie `/dev/fb0`, puis `/dev/graphics/fb0` si le premier chemin est absent. La copie directe peut produire du tearing.
- `src/input.c` lit les plages et le nombre de slots avec les ioctls evdev. La détection se fait par capacités parmi les nœuds event0 à event15. Le décodeur `src/evdev_mt.c` suit le protocole B et publie les mises à jour au `SYN_REPORT`. Une fois le contact principal choisi, l’interface reçoit ses mouvements jusqu’au relâchement. Un doigt secondaire déjà posé ne se transforme pas en nouveau clic. Après `SYN_DROPPED`, le décodeur relâche le pointeur, ignore le reste de la trame et réinterroge le slot courant et les coordonnées. Il attend un nouvel identifiant de contact pour reprendre. L’annulation libère le geste en cours sans générer de tap. Les codes des boutons Samsung restent à relever avant de les associer à des actions.
- `src/main.c` conserve son chemin PID 1 avec montage de procfs, sysfs, devtmpfs et tmpfs. Le profil dreamlte désactive le stockage brut sur `/dev/vda`. Notes et préférences vivent alors en mémoire. Le profil QEMU conserve la persistance.
- Les deux profils compilent avec GCC/musl dans Alpine ARM64. Le build QEMU conserve son noyau `virt` 6.12.113 et son initramfs. Le profil dreamlte compile un ELF Linux statique et prépare les polices. Il utilise les en-têtes Linux d’Alpine, ce qui ne valide pas l’ABI du noyau Samsung 4.4.111.

Références d’interface. [Protocole multitouch Linux](https://kernel.org/doc/html/v5.10/input/multi-touch-protocol.html), [codes evdev](https://www.kernel.org/doc/html/v4.15/input/event-codes.html), [API fbdev](https://kernel.org/doc/html/v5.7/fb/api.html).

## Compilation et tests sur l’hôte

```sh
./build.sh s8-dreamlte
file build/s8-dreamlte/lumen
sh test/evdev_mt_check.sh
sh test/inflate_check.sh
./build.sh qemu
python3 test/e2e.py
```

Le profil S8 produit `build/s8-dreamlte/lumen` et `build/s8-dreamlte/fonts/`. Le programme recherche ses polices sous `/fonts` à l’exécution. Le profil par défaut produit `build/Image` et `build/initramfs.cpio` pour `run.sh`.

Le test evdev utilise de vrais types `struct input_event` synthétiques. Il couvre dix slots simultanés, les trames, la priorité du premier geste, le relâchement, les positions mémorisées, les bornes non nulles, la saturation, les slots invalides et la perte de synchronisation.

Les deux builds, le test evdev et les trois cas inflate ont réussi. Le profil S8 a aussi été compilé dans un volume de build vide, sans télécharger de noyau. `readelf` confirme un ELF64 AArch64 sans interpréteur dynamique.

La suite QEMU utilise `test/video_fixture.py` pour la recherche et la lecture vidéo. FFmpeg génère localement une mire animée de 120 secondes et une piste audio. Le serveur fournit un résultat au titre long et transmet le flux MJPEG/PCM par le protocole du pont vidéo habituel. Le contrôle de lecture exige un relevé positif avant le seek. Le test de plein écran repère le panneau de commandes dans la capture, puis vérifie le mouvement entre deux images. Les contrôles Web de la suite utilisent toujours leurs services distants.

Le dernier passage complet sur `s8-dreamlte` a réussi **32 contrôles sur 32 en 203 secondes**. Avant le seek, la lecture a atteint 28,9 images par seconde sans image perdue. Le seek a atteint 59,8 secondes. Avec le titre long, le bouton de plein écran a été trouvé à Y = 542 points et 18,8 % des pixels ont changé entre les deux captures. La capture audio contenait du son et a enregistré 28 secondes pendant le scénario vidéo.

## Démarrage dreamlte hors téléphone

Le firmware de référence déclaré est `G950FXXUCDZE7` pour le SM-G950F. Le dépôt ne contient actuellement ni image de démarrage d’origine pour cette version, ni ses DTB, ni un arbre source Samsung dont la correspondance avec ce firmware a été vérifiée. Le catalogue Samsung Open Source a répondu HTTP 403 depuis l’hôte lors du relevé. L’identifiant exact de l’archive source reste à confirmer.

Android 9 décrit la version 1 de l’en-tête de démarrage pour les appareils lancés avec cette version d’Android. La version d’Android installée sur ce Galaxy S8 ne suffit donc pas à identifier le format de son image. Il faut relever l’en-tête, la disposition des sections et le format des DTB à partir du fichier d’origine. [En-têtes Android](https://source.android.com/docs/core/architecture/bootloader/boot-image-header) · [Images DTB](https://source.android.com/docs/core/architecture/bootloader/dtb-images)

1. Obtenir une copie locale du paquet de firmware stock correspondant exactement au modèle et à la version. Conserver la provenance et l’empreinte SHA-256 de l’archive, puis inventorier son contenu sur l’hôte. Si le paquet AP contient `boot.img.lz4`, extraire et décompresser cette copie locale. Conserver aussi l’empreinte de l’image décompressée.
2. Examiner l’image stock en lecture seule. Relever le format et la taille de page de l’en-tête, les adresses, la ligne de commande, les tailles du noyau et du ramdisk, la position des données de device tree et les éventuelles données finales de vérification. Utiliser l’outil AOSP `unpack_bootimg` si le format est reconnu. Si l’outil signale une erreur ou des données inexpliquées, conserver le diagnostic et vérifier la disposition Samsung avant de conclure. Identifier ensuite les DTB et relever leurs chaînes `model` et `compatible` avec `dtc` pour les comparer à l’arbre source. [Outil AOSP](https://android.googlesource.com/platform/system/tools/mkbootimg/+/refs/heads/main/unpack_bootimg.py)
3. Quand l’archive source Samsung est identifiée, comparer sa version de noyau, son defconfig, ses DTS et sa chaîne de compilation avec le noyau stock et son relevé. Si la configuration stock a été intégrée au noyau, tenter de l’extraire hors ligne avec `scripts/extract-ikconfig`. Vérifier ensuite qu’une compilation ARM64 du noyau et des DTB produit les artefacts attendus, et consigner la version du compilateur ainsi que les empreintes. Toute divergence avec l’image stock doit rester explicite.
4. Préparer sur l’hôte l’arborescence du futur ramdisk et vérifier son manifeste. Lumen attend un `/init` exécutable si lancé en PID 1, les polices sous `/fonts`, ainsi que les points de montage `/proc`, `/sys`, `/dev` et `/tmp`. Comparer ces attentes avec le ramdisk stock et la configuration de son noyau. Le build du profil dreamlte fournit déjà l’ELF et les polices pour cette comparaison.

Les résultats attendus de cette phase sont un inventaire stock et une compilation reproductible, avec les hypothèses de format levées par inspection des fichiers. Le démarrage sur la dalle réelle et l’acceptation d’un chargement par le bootloader verrouillé restent des validations distinctes. Cette phase s’effectue exclusivement sur des fichiers de l’hôte.

## Blocages matériels

- **Démarrage.** Le bootloader est verrouillé et aucune voie de démarrage autorisée pour Lumen n’a été établie. Le noyau QEMU cible une autre machine. Il reste à établir la provenance et la configuration du noyau Samsung, le DTB et les pilotes Exynos nécessaires, puis les contraintes de démarrage propres au firmware. Les fichiers de ce profil sont des artefacts userspace.
- **Écran.** Le rôle de chacun des trois fb, le framebuffer relié à la dalle, le format, le stride, les offsets et la capacité de mmap restent à vérifier. Android peut contrôler la dalle via sa pile graphique et restreindre les nœuds via les permissions et SELinux. La seule présence de fb0 ne prouve pas que les écritures seront visibles.
- **Entrées.** Le flux brut du tactile, les plages effectives, l’orientation, les permissions et les codes des boutons restent à confirmer. Le mapping suppose des axes directs et un flux à slots de type B. Les plages lues par ioctl sont prioritaires sur les valeurs fournies.
- **Système.** L’audio vise un nœud ALSA utilisé par virtio-sound. Le réseau dépend de la détection d’interface, de DHCP et, pour la vidéo, du pont QEMU à `10.0.2.2`. Audio, réseau, gestion d’énergie et usage sous Android restent hors validation.

Les opérations de cette branche se limitent à la compilation et aux essais sur l’hôte ou en VM. La contrainte du portage exclut tout flash, déverrouillage ou changement de partition.
