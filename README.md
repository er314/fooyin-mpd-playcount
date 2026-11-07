# Monitor MPD playback for play count & last played - Fooyin plugin

## Summary

This is a plugin for [fooyin](https://github.com/fooyin/fooyin).
<br>It monitors MPD playback, in order to update fooyin's database of tracks play count and last played.
<br>This makes sense when using both fooyin and MPD for audio playback.

## Pre-requisites

- Fooyin
- libmpdclient2

## Installation

- Build from source (see Fooyin build doc)
- Within fooyin, install & enable plugin "MPD playcount" (file 'fyplugin_mpdplaycount.so').

## Usage

The plugin runs in the background, monitoring MPD playback. Stats for a track are updated as soon as it's played for more than 1 minute.

## Notice

This (modest) software was (modestly) coded with the help of AI.

## Legal

This software is released under [GPLv3](https://www.gnu.org/licenses/gpl-3.0.en.html).
