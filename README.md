# LumaCam pour OBS Studio

Transformez votre iPhone en **webcam 4K** pour OBS Studio : jusqu'à 60 images par seconde, faible latence,
en **USB** sans aucun réglage ou **sans fil** avec un code à 4 chiffres.

**Site et téléchargements : https://lumacam-pi.vercel.app**

## Télécharger

| Fichier | Pour |
|---|---|
| [`lumacam-obs-macos.pkg`](https://github.com/dolva2000/lumacam-downloads/releases/latest/download/lumacam-obs-macos.pkg) | Plugin OBS Studio pour Mac (OBS 31 ou plus récent) |
| [`LumaCam-mac.dmg`](https://github.com/dolva2000/lumacam-downloads/releases/latest/download/LumaCam-mac.dmg) | App LumaCam pour Mac : aperçu et webcam virtuelle pour Zoom, Teams, Meet, FaceTime… |
| [`lumacam-obs-windows-x64.zip`](https://github.com/dolva2000/lumacam-downloads/releases/latest/download/lumacam-obs-windows-x64.zip) | Plugin OBS Studio pour Windows 10 et 11 (bêta) — copiez le dossier `lumacam-obs` dans `C:\ProgramData\obs-studio\plugins\` |
| App iPhone | Sur l'App Store |

Toutes les versions sont dans les [Releases](https://github.com/dolva2000/lumacam-downloads/releases).
Sous Windows, la connexion USB demande l'app « Appareils Apple » (Microsoft Store) ou iTunes.

## Utilisation

1. Installez l'app **LumaCam** sur l'iPhone et ouvrez-la.
2. Installez le plugin, puis redémarrez OBS.
3. Dans OBS : **Sources → + → LumaCam (caméra iPhone)**.
4. Branchez l'iPhone en USB, ou tapez dans la source le **code à 4 chiffres** affiché dans l'app.

La source trouve l'iPhone toute seule : USB, Bonjour, partage de connexion de l'iPhone, puis le réseau local.

## Fonctionnalités

- 4K jusqu'à 60 i/s, HEVC ou H.264, **décodage matériel** (VideoToolbox sur Mac, FFmpeg D3D11VA / VAAPI / CUDA ailleurs)
- Réglages de la caméra depuis OBS : objectif, zoom, mise au point, exposition, balance des blancs, lampe
- Styles de prise de vue (Naturel, Cinéma, Faible lumière, Action) et **rendus couleur** appliqués par LUT 3D sur le GPU
- Reconnexion automatique, changement de résolution sans coupure, état en direct (i/s, débit, batterie, température)

## Compiler depuis les sources

Le plugin est construit sur le modèle officiel [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate).

```bash
cmake --preset macos && cmake --build --preset macos          # macOS (Xcode)
cmake --preset windows-x64 && cmake --build --preset windows-x64
cmake --preset ubuntu-x86_64 && cmake --build --preset ubuntu-x86_64
```

| Dossier | Rôle |
|---|---|
| `src/lumacam-source.*` | La source OBS : connexion, décodage, propriétés, réglages de la caméra, rendus couleur |
| `src/net/connector.*` | Recherche de l'iPhone : USB, Bonjour, partage de connexion, réseau local, code de connexion |
| `src/net/usbmux.*`, `src/net/lan.*`, `src/net/discovery.*`, `src/net/socket.*` | USB (usbmuxd), réseau local, Bonjour, sockets |
| `src/lcp/protocol.hpp` | Protocole LumaCam (LCP) |
| `src/decoder/` | Décodeurs matériels |
| `data/` | Traductions (français, anglais) et LUT des rendus couleur |

## Assistance

Un problème ou une idée ? Ouvrez une [demande](https://github.com/dolva2000/lumacam-downloads/issues).

## Licence

Ce plugin est distribué sous licence **GNU GPL v2 ou ultérieure**, comme OBS Studio. Voir [LICENSE](LICENSE).
