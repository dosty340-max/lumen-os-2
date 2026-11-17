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

Le firmware de référence déclaré est `G950FXXUCDZE7` pour le SM-G950F. L’image stock et ses DTB ont été analysés hors ligne à partir d’une archive SamFW SFR. Les fichiers binaires restent hors du dépôt. L’archive Samsung exacte de DZE7 reste à confirmer. L’essai ci-dessous utilise un miroir des sources DUD1, dont le firmware porte le même identifiant de noyau.

Android 9 décrit la version 1 de l’en-tête de démarrage pour les appareils lancés avec cette version d’Android. La version d’Android installée sur ce Galaxy S8 ne suffit donc pas à identifier le format de son image. Il faut relever l’en-tête, la disposition des sections et le format des DTB à partir du fichier d’origine. [En-têtes Android](https://source.android.com/docs/core/architecture/bootloader/boot-image-header) · [Images DTB](https://source.android.com/docs/core/architecture/bootloader/dtb-images)

1. Obtenir une copie locale du paquet de firmware stock correspondant exactement au modèle et à la version. Conserver la provenance et l’empreinte SHA-256 de l’archive, puis inventorier son contenu sur l’hôte. Si le paquet AP contient `boot.img.lz4`, extraire et décompresser cette copie locale. Conserver aussi l’empreinte de l’image décompressée.
2. Examiner l’image stock en lecture seule. Relever le format et la taille de page de l’en-tête, les adresses, la ligne de commande, les tailles du noyau et du ramdisk, la position des données de device tree et les éventuelles données finales de vérification. Utiliser l’outil AOSP `unpack_bootimg` si le format est reconnu. Si l’outil signale une erreur ou des données inexpliquées, conserver le diagnostic et vérifier la disposition Samsung avant de conclure. Identifier ensuite les DTB et relever leurs chaînes `model` et `compatible` avec `dtc` pour les comparer à l’arbre source. [Outil AOSP](https://android.googlesource.com/platform/system/tools/mkbootimg/+/refs/heads/main/unpack_bootimg.py)
3. Quand l’archive source Samsung est identifiée, comparer sa version de noyau, son defconfig, ses DTS et sa chaîne de compilation avec le noyau stock et son relevé. Si la configuration stock a été intégrée au noyau, tenter de l’extraire hors ligne avec `scripts/extract-ikconfig`. Vérifier ensuite qu’une compilation ARM64 du noyau et des DTB produit les artefacts attendus, et consigner la version du compilateur ainsi que les empreintes. Toute divergence avec l’image stock doit rester explicite.
4. Préparer sur l’hôte l’arborescence du futur ramdisk et vérifier son manifeste. Lumen attend un `/init` exécutable si lancé en PID 1, les polices sous `/fonts`, ainsi que les points de montage `/proc`, `/sys`, `/dev` et `/tmp`. Comparer ces attentes avec le ramdisk stock et la configuration de son noyau. Le build du profil dreamlte fournit déjà l’ELF et les polices pour cette comparaison.

Les résultats attendus de cette phase sont un inventaire stock et une compilation reproductible, avec les hypothèses de format levées par inspection des fichiers. Le démarrage sur la dalle réelle et l’acceptation d’un chargement par le bootloader verrouillé restent des validations distinctes. Cette phase s’effectue exclusivement sur des fichiers de l’hôte.

### Relevé de l’image stock G950FXXUCDZE7

Samsung confirme la publication pour le SM-G950F sous Android 9. La copie locale correspond au paquet SamFW **SFR**, avec le CSC `G950FOXJCDUD3`. Son paquet AP est nommé `AP_G950FXXUCDZE7_CL21427293_QB110044235_REV00_user_low_ship_meta_OS9.tar.md5`. Le catalogue XEF donne le même nom AP, avec un CSC différent. L’identité binaire entre les deux paquets AP n’a pas été vérifiée. [Publication Samsung](https://doc.samsungmobile.com/SM-G950F/000014170428/eng.html) · [SamFW SFR](https://samfw.com/firmware/SM-G950F/SFR/G950FXXUCDZE7) · [SamFW XEF](https://samfw.com/firmware/SM-G950F/XEF/G950FXXUCDZE7)

- Le MD5 du ZIP correspond à celui publié par SamFW, `405abb1af4abbe11e890b61ebe683de9`. Le SHA-256 calculé est `f02b4b0922f80a1f02e47fe3ce47c3ab08334ef6abc2d02f1442cd97838435de`. Le contenu TAR de l’AP correspond à son MD5 de fin d’archive, `62753b847e84356b4025fbc1919c4b3d`. Ces contrôles établissent l’intégrité par rapport aux sommes disponibles. La signature OEM n’a pas été vérifiée.
- L’image décompressée fait **37 351 712 octets**. Son SHA-256 est `63f322d6fb7eadc4eda0fb54b2dbdf7a941719518656536d5b5703bbb30dbb2e`. Le conteneur commence par `ANDROID!`, avec un en-tête de disposition Samsung historique, des pages de **2 048 octets** et un champ de taille DT de 1 142 784 octets à l’offset 40. La ligne de commande dans l’en-tête est vide. Le champ tags vaut `0x10000100`.

| Section | Offset dans boot.img | Taille | Format ou adresse |
| --- | ---: | ---: | --- |
| Noyau | 2 048 | 31 167 792 | Image Linux ARM64 brute, `0x10008000` |
| Ramdisk | 31 170 560 | 5 037 560 | CPIO gzip, `0x11000000` |
| Device trees | 36 208 640 | 1 142 784 | Table Samsung `DTBH` version 2 |
| Données finales | 37 351 424 | 288 | Commencent par `SEANDROIDENFORCE` |

- Les cinq DTB ont tous été lus avec `fdtget` et décompilés avec `dtc`. Ils identifient des cartes DREAMLTE EUR sous Exynos 8895, pour les révisions 01, 02, 03, 04/05 et 06. La révision de l’appareil et le critère de sélection du bootloader restent à établir.
- Le noyau embarqué s’identifie comme `4.4.111-21427293`, construit avec GCC 4.9.x. Sa configuration embarquée a été extraite. Elle confirme notamment `CONFIG_BLK_DEV_INITRD=y`, `CONFIG_RD_GZIP=y`, `CONFIG_DEVTMPFS=y`, `CONFIG_INPUT_EVDEV=y`, `CONFIG_FB=y`, `CONFIG_EXYNOS_DECON_FB=y` et `CONFIG_TOUCHSCREEN_SEC_TS=y`. Ces options renseignent sur le noyau stock. Les permissions des nœuds et le comportement réel des pilotes restent à valider.
- L’intégrité gzip du ramdisk et la lecture de son CPIO sont confirmées. Son manifeste contient l’`init` Android, `fstab.samsungexynos8895`, `dev`, `proc` et `sys`. Les répertoires racine `fonts` et `tmp` devront figurer dans un ramdisk Lumen. Aucun fichier de démarrage modifié n’a été produit.

### Essai de compilation du noyau et des DTB

Le catalogue Samsung Open Source a répondu HTTP 403 aux recherches `SM-G950F` et `G950FXXUCDZE7`. L’archive officielle exacte de DZE7 reste à identifier. L’essai utilise le miroir Exynos8895 au commit `654e8e3ad15988b6114c190f7568df9a7272fbc9`. Ce commit importe les sources du Galaxy S8 `G950FXXUCDUD1`, du S8+ `G955FXXUCDUD1` et du Note 8 `N950FXXSGDUG7`. Le firmware G950F DUD1 est répertorié avec la même version noyau `4.4.111-21427293`. Le Makefile du miroir indique `4.4.111`. Ces éléments justifient l’essai sur DUD1, tandis que l’identité des sources DZE7 reste à établir. [Catalogue Samsung](https://opensource.samsung.com/uploadSearch?searchValue=SM-G950F) · [Import DUD1](https://github.com/exynos8895/android_kernel_samsung_universal8895/commit/654e8e3ad15988b6114c190f7568df9a7272fbc9) · [Relevé du firmware DUD1](https://samfrew.com/download/Galaxy__S8__/f8cO/VDH/G950FXXUCDUD1/G950FOVFCDUD3)

La configuration de départ a été vérifiée par une nouvelle extraction du bloc gzip `IKCFG_ST` du noyau de `boot.img`. Son SHA-256 est `a6fdd036aea9da181db61a252283f24a90aa8da72d4af48ddaccdc8440efc19c`. Le build ARM64 a utilisé Ubuntu 20.04 Linux amd64 sous Docker et le GCC CFP/JOPP `4.9.x 20150123 (prerelease)` du miroir Neternels au commit `5eb1b4159bc466602e7634b1f7a4f471f4c027e2`. La chaîne a le même identifiant de version que celle annoncée par le noyau stock. Son identité binaire reste inconnue. [Miroir de la chaîne CFP](https://github.com/Neternels/exynos8895_kernel/tree/5eb1b4159bc466602e7634b1f7a4f471f4c027e2/toolchain/gcc-cfp/gcc-cfp-jopp-only/aarch64-linux-android-4.9)

Le premier essai a compilé les cinq DTB. La cible `Image` a échoué au lien du vDSO avec `-fuse-linker-plugin, but liblto_plugin.so not found`. La chaîne contient `liblto_plugin.so.0.0.0` mais omet le lien `liblto_plugin.so`. Le second essai a rétabli ce lien dans la chaîne locale et déclaré `PLATFORM_VERSION=9` avec `CONFIG_SHELL=/bin/bash` pour les scripts Samsung.

Le deuxième essai a passé cette étape, puis s’est arrêté sur `init/_vmm.S` avec `file not found: init/vmm.6g.elf`. `CONFIG_RKP=y` et `CONFIG_RKP_6G=y` sélectionnent ce binaire par `.incbin`. Le miroir ne le contient pas. L’essai suivant a réutilisé l’ELF ARM64 exécutable incorporé au noyau stock. Il commence à `0x13f9db8` dans la section noyau, soit `0x13fa5b8` dans `boot.img`. La fin de sa table de sections donne une longueur de 255 176 octets, avec tous les segments contenus dans cette plage. Son SHA-256 est `396b39367dc6a13741771dc92b372c8bcff764978e12c000d1f497c6a30b1d24`. Il est fourni localement à l’assembleur dans le répertoire de sortie.

Depuis la racine des sources dans le conteneur, avec `CROSS` pointant vers le préfixe absolu `aarch64-linux-android-` et `STOCK_CONFIG` vers la configuration extraite, les cibles ont été lancées ainsi. Le script `diffconfig`, le wrapper GCC et l’instrumentation CFP utilisent Python 2.7 dans ce conteneur. La variable `VMM_ELF` désigne le binaire extrait de la copie stock.

```bash
mkdir -p /build
cp "$STOCK_CONFIG" /build/.config
args=(O=/build ARCH=arm64 PLATFORM_VERSION=9 CONFIG_SHELL=/bin/bash CROSS_COMPILE="$CROSS")
make "${args[@]}" olddefconfig
scripts/diffconfig "$STOCK_CONFIG" /build/.config
make "${args[@]}" -j4 exynos/exynos8895-dreamlte_eur_open_{05,07,08,09,10}.dtb
mkdir -p /build/init
cp "$VMM_ELF" /build/init/vmm.6g.elf
make "${args[@]}" -j4 Image
```

`olddefconfig` a désactivé `TRACING_SUPPORT` et retiré les symboles associés, ainsi que `RBIN`, `ION_RBIN_HEAP` et les réglages de prélecture du hachage dm-verity absents de cet arbre. Aucune option de sécurité n’a été modifiée manuellement. `RKP`, `RKP_CFP` et `RKP_CFP_JOPP` sont restés activés dans la config effective.

#### Comparaison des DTB

Les cinq cibles DTB ont réussi lors des deux essais. Les binaires produits par les deux essais ont les mêmes empreintes. Pour chaque révision, `fdtget` renvoie les mêmes propriétés racine `model` et `compatible` que le DTB stock. Les cinq DTB compilés contiennent en plus le sous-arbre `/firmware/android/fstab` avec les entrées `SYSTEM` et `EFS`. Après suppression de `/firmware` sur des copies de comparaison uniquement, les sorties triées de `dtc -s -I dtb -O dts` sont identiques aux sorties stock pour les cinq paires.

| Révision DREAMLTE EUR | Suffixe de la cible DTB | Taille en octets | SHA-256 compilé |
| --- | --- | ---: | --- |
| 01 | `05` | 226 567 | `83702bf29d1a9eb265a6015e0d369a8014235b2ed6956b790c487dbcde5b4672` |
| 02 | `07` | 227 723 | `66d29788b1470b0baddabd85eda9bb45a04bba17a256337cdb1c5977fdfedc87` |
| 03 | `08` | 227 620 | `d18fd629fec3f94889341144987a936ce95a91f3b76f0e845cc90605dcf4eca5` |
| 04/05 | `09` | 228 893 | `c4605b04d9b886553e7b3b12283c58ef8bb0f62af867b5feae14633e0f28731c` |
| 06 | `10` | 228 809 | `fc90a66e2eab157709cc69a60d5522eb669d87ce28a8ff6724666bd7af8a87d1` |

#### Résultat de la cible Image

Avec le VMM fourni, `init/_vmm.o` s’est assemblé. La compilation a ensuite échoué avant l’édition des liens de `vmlinux`. GCC a signalé une erreur interne dans `cc1` sur `fs/dcache.o`. D’autres unités, dont `kernel/exec_domain.o` et `mm/mremap.o`, se sont arrêtées avec le signal 11 et le code 139. Le fichier core de l’échec sur `mm/mremap.o` est celui du processus AArch64 `/usr/bin/qemu-x86_64` qui exécute la chaîne x86_64. Le journal ne permet pas d’attribuer tous les plantages à une cause unique. Le code de sortie de `make Image` est 2. Aucun `vmlinux` ni `Image` n’a été obtenu. Un nouvel essai sous Linux x86_64 natif permettrait d’écarter la couche d’émulation avant d’évaluer les éventuels écarts de sources restants.

Les binaires compilés, le firmware et l’ELF VMM sont restés dans l’espace de travail local. L’essai s’est limité à l’hôte.

## Blocages matériels

- **Démarrage.** Le bootloader est verrouillé et aucune voie de démarrage autorisée pour Lumen n’a été établie. Le noyau QEMU cible une autre machine. Il reste à établir la provenance et la configuration du noyau Samsung, le DTB et les pilotes Exynos nécessaires, puis les contraintes de démarrage propres au firmware. Les fichiers de ce profil sont des artefacts userspace.
- **Écran.** Le rôle de chacun des trois fb, le framebuffer relié à la dalle, le format, le stride, les offsets et la capacité de mmap restent à vérifier. Android peut contrôler la dalle via sa pile graphique et restreindre les nœuds via les permissions et SELinux. La seule présence de fb0 ne prouve pas que les écritures seront visibles.
- **Entrées.** Le flux brut du tactile, les plages effectives, l’orientation, les permissions et les codes des boutons restent à confirmer. Le mapping suppose des axes directs et un flux à slots de type B. Les plages lues par ioctl sont prioritaires sur les valeurs fournies.
- **Système.** L’audio vise un nœud ALSA utilisé par virtio-sound. Le réseau dépend de la détection d’interface, de DHCP et, pour la vidéo, du pont QEMU à `10.0.2.2`. Audio, réseau, gestion d’énergie et usage sous Android restent hors validation.

Les opérations de cette branche se limitent à la compilation et aux essais sur l’hôte ou en VM. La contrainte du portage exclut tout flash, déverrouillage ou changement de partition.
