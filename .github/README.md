# OBS Plugins by Vakovsky

English · [Русский](README.ru.md)

![OBS 31+](https://img.shields.io/badge/OBS-31%2B-302e31?logo=obsstudio&logoColor=white) ![Windows x64](https://img.shields.io/badge/Windows-x64-0078d4) ![macOS](https://img.shields.io/badge/macOS-universal-555555) ![License GPL-2.0](https://img.shields.io/badge/license-GPL--2.0--or--later-blue)

Two plugins for OBS Studio. Each one has its own releases.

## Ultimate Frame

A filter that rounds the corners of a source and adds a border, a shadow, a backing, crop, zoom and a fade into the scene at the edges. Each scene can have its own look, and when you switch scenes with [Move](https://obsproject.com/forum/resources/move.913/), Fade or Cut the filter animates from one look to the other.

Sizes are in canvas pixels. A 3 px border stays 3 px whether the camera fills the screen or sits small in a corner.

The outer border and shadow go past the edge of the source. For items that appear or disappear in Move, pick the Fade for Ultimate Frame transition that comes with the plugin: the plain Fade cuts them off.

<table>
<tr>
<td width="50%"><img src="../docs/features/img/profile-b.gif" alt=""><br>Per-scene profiles</td>
<td width="50%"><img src="../docs/features/img/crop.gif" alt=""><br>Crop per scene</td>
</tr>
<tr>
<td><img src="../docs/features/img/radius.gif" alt=""><br>Rounded corners</td>
<td><img src="../docs/features/img/corners.gif" alt=""><br>Radius per corner</td>
</tr>
<tr>
<td><img src="../docs/features/img/inner-stroke.gif" alt=""><br>Inner border</td>
<td><img src="../docs/features/img/outer-stroke.gif" alt=""><br>Outer border</td>
</tr>
<tr>
<td><img src="../docs/features/img/gradient.gif" alt=""><br>Gradient</td>
<td><img src="../docs/features/img/glow.gif" alt=""><br>Glow</td>
</tr>
<tr>
<td><img src="../docs/features/img/shadow.gif" alt=""><br>Drop shadow</td>
<td><img src="../docs/features/img/inner-shadow.gif" alt=""><br>Inner shadow</td>
</tr>
<tr>
<td><img src="../docs/features/img/zoom.gif" alt=""><br>Zoom and offset inside the frame</td>
<td><img src="../docs/features/img/backing.gif" alt=""><br>Backing under a transparent source</td>
</tr>
</table>

To start, add Ultimate Frame as the last filter of a source, turn on Different settings per scene, pick a scene in Edit profile and set its look.

## Browser ReSize (beta)

<img src="../docs/features/img/size-a.gif" alt="" width="50%">

Gives a browser source, such as a chat, its own page size on each scene. The page is laid out again at the new size, so the text doesn't stretch.

With Smooth resize (for Move) on, the page grows along with the Move animation. Turn it off for Fade and other transitions.

A static page, like a chat with no new messages, may not redraw after a resize. So the filter adds a tiny invisible animation to the end of the source's custom CSS. Your own CSS stays as it is. Uncheck Redraw the page all the time to take it out.

## Install

OBS Studio 31 or newer. Close OBS and download the file for your system from [Releases](https://github.com/VakovskyGT/vakovsky-obs-plugins/releases).

- Windows 10 or 11: run `windows-x64-installer.exe`. It finds the installed OBS or asks for the folder of a portable one, puts the plugin where that OBS version loads it, and can be removed in Installed apps. The installer isn't signed, so Windows SmartScreen may warn: click More info, then Run anyway. Or install by hand: extract the zip into `C:\ProgramData\obs-studio\plugins\` (`windows-x64.zip` for OBS 33 and newer, `windows-legacy-x64.zip` for OBS 31 and 32).
- macOS: put the `.plugin` folder from the zip into `~/Library/Application Support/obs-studio/plugins/`. The plugin isn't signed, so macOS may block it. Then run this in Terminal once: `xattr -r -d com.apple.quarantine ~/Library/Application\ Support/obs-studio/plugins/ultimate-frame.plugin` (or `browser-resize.plugin`).

Once a day the plugins check GitHub for a new version and show a link in the filter properties. They don't download anything.

## Limits

- Ultimate Frame has to be the last filter. Filters after it cut off the outer border and shadow.
- Items with Alt-crop, a scale filter or a blending mode get only the corners and inner effects: OBS cuts their picture at the edge. The filter tells you which scenes these are.
- On a scene used as a source the outer border and outer shadow are off, for the same reason. To get them, add the scene through [Source Clone](https://obsproject.com/forum/resources/source-clone.1632/) and put Ultimate Frame on the clone. Use the clone in every scene: Move treats a clone and its scene as one item and swaps them halfway.
- Per-scene settings don't work on a group: each scene has its own copy of it.
- Browser ReSize waits for a frame without black strips at the edges, so a page with a black border can show its new size late.

Found a bug? Open an [issue](https://github.com/VakovskyGT/vakovsky-obs-plugins/issues).

## Author

Made by [Vakovsky](https://www.twitch.tv/vakovsky). If the plugins help you, you can support me on [DonationAlerts](https://www.donationalerts.com/r/vakovsky) or [Boosty](https://boosty.to/vakovsky).

License: [GPL-2.0-or-later](../LICENSE)
