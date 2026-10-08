# Installing

[Docs home](../README.md) › [User guide](README.md) › Installing

U-Stu ships as a Flatpak bundle. It runs on any Linux distribution with
Flatpak. It's tested on Fedora 44 and made for Linux Mint 22.

## Install

```sh
# once: Flathub provides the GNOME runtime the app runs on
flatpak remote-add --if-not-exists --user flathub https://dl.flathub.org/repo/flathub.flatpakrepo

# download and install
wget https://software.unicornviz.com/u-studio-video-editor-latest.flatpak
flatpak install --user ./u-studio-video-editor-latest.flatpak

# run
flatpak run com.ustudio.VideoEditor
```

The first install also downloads the GNOME runtime, about 450 MB. After
that, U-Stu shows up in your app menu.

On **Linux Mint** you can double-click the downloaded file instead, and it
opens in Software Manager.

## Update

Download the latest file again and install it the same way. If Flatpak says
"already installed", you already have that version. Add `--reinstall` to
install it anyway.

## Add-ons

Some features come as add-ons you install next to the app: **Titles**
([Titles](titles.md)) and **Effects** ([Effects](effects.md)). Install the
ones you want. An add-on must be the same version as the app, so update
them together.

```sh
# Titles
wget https://software.unicornviz.com/u-studio-video-editor-dropin-titles-latest.flatpak
flatpak install --user ./u-studio-video-editor-dropin-titles-latest.flatpak

# Effects
wget https://software.unicornviz.com/u-studio-video-editor-dropin-effects-latest.flatpak
flatpak install --user ./u-studio-video-editor-dropin-effects-latest.flatpak
```

Restart U-Stu after installing. **Settings › Drop-ins** lists the installed
add-ons; you can switch one off there. If an add-on is for another version
of the app, it's listed under "Couldn't load" and says which version it
needs. To remove it:

```sh
flatpak uninstall --user com.ustudio.VideoEditor.DropIn.Titles
flatpak uninstall --user com.ustudio.VideoEditor.DropIn.Effects
```

Uninstalling the app removes its add-ons too.

## Uninstall

```sh
flatpak uninstall --user com.ustudio.VideoEditor
```

Your projects and media are never touched. Settings, autosaves and proxies
are under `~/.var/app/com.ustudio.VideoEditor/`. Delete that folder too if
you want nothing left behind.

## What it can access

- The Flatpak can read and write your home folder, `/media`, `/run/media`
  and `/mnt`, so it can open footage on external drives.
- It has **no network access**. The editor never phones home, checks for
  updates or downloads anything.

## Building from source

Developers can build and run it natively; see
[Building](../developer/building.md).

Next: [Getting started](getting-started.md)
