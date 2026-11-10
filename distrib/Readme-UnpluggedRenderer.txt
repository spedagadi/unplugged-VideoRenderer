Unplugged Video Renderer

A DirectShow video renderer for 64-bit players. It is the renderer used by
Unplugged, packaged for MPC-HC and MPC-BE: the same HDR tone mapping, the
Unplugged tone curve, the ML grade (Auto/Manual) and the renderer property
pages.

Package version 2.2.0 -- the same release as Unplugged 2.2.0. The renderer
binary inside it is build 0.10.9.2637; the filter's own properties show that
build number.


REQUIREMENTS
------------
- 64-bit Windows 10 or 11.
- A 64-bit DirectShow player. Tested with MPC-HC 2.8.x and MPC-BE 1.9.x.
  A 32-bit player cannot load this renderer; use mpc-hc64.exe / mpc-be64.exe.
- Microsoft Visual C++ 2015-2022 x64 runtime. The installer adds it when it
  is missing or older than the version the bundled inference engine needs.


INSTALL
-------
Run UnpluggedRenderer-setup-<version>.exe and accept the administrator prompt.
It installs to:

    C:\Program Files\Unplugged Video Renderer

and registers "Unplugged Video Renderer" (the filter and its four property
pages). The ML models and onnxruntime.dll must stay in that folder: the filter
loads them from the directory its own .ax was loaded from.

If MPC-HC or MPC-BE was running during the install, close it and start it
again before adding the filter, so the player re-reads the registered filters.


SELECT THE RENDERER IN THE PLAYER
---------------------------------
The renderer is registered but the players do not switch to third-party
renderers by themselves. Add it once as a preferred filter.

MPC-HC:
  1. Options -> External Filters.
  2. Add Filter... -> select "Unplugged Video Renderer" -> OK.
  3. With the entry selected, choose "Prefer" -> OK.

MPC-BE:
  1. Options -> Filters -> External Filters.
  2. Add Filter... -> select "Unplugged Video Renderer" -> OK.
  3. With the entry selected, choose "Prefer" -> OK.

Open a video. To confirm it is in the graph:
  MPC-HC: Play -> Filters -> Unplugged Video Renderer
  MPC-BE: right-click the picture -> Filters -> Unplugged Video Renderer


RENDERER SETTINGS
-----------------
Open the renderer's properties from the player's filter list (double-click the
Unplugged Video Renderer entry, or right-click it -> Properties). The pages
are the same as in Unplugged:

  Main  - video processor, scaling, dithering, exclusive fullscreen
  HDR   - HDR/SDR conversion, display nits, tone curve, trims
  Zion  - ML grade mode and its controls
  Info  - version and pipeline information

Settings are stored per user under:

    HKCU\Software\Unplugged\Renderer

"Reset settings" in the Start menu removes them; use it if the picture is
unexpectedly wrong after experimenting.


HDR / GAMMA NOTE
----------------
HDR-to-SDR tone mapping produces the house 2.2 reference code. The HDR page's
"Display transfer" control re-encodes the finished SDR picture for a display
calibrated to Gamma 2.2, Gamma 2.4, BT.1886, sRGB or Gamma 2.0; "Follow source"
leaves SDR untouched and keeps the 2.1.0 behaviour. Set it to the projector's
calibrated transfer. If a calibration 3D LUT is loaded, author it for the same
transfer.

The HDR page also carries the DBLE compensation rows (Enable, Strength,
Trigger picture %, Highlight start), the diagnostic View (Normal / Clipping
test / Split screen) and the OSD readout. DBLE is for projectors whose dynamic
laser dimming clips highlights in dark scenes; it works on the finished SDR
picture and does not require the ML grade.


UNINSTALL
---------
Settings -> Apps -> Unplugged Video Renderer, or the Start menu entry.

Renderer settings are deliberately kept, because the Unplugged application
uses the same registry key. Run "Reset settings" first if you want them gone.

If the Unplugged application is installed, uninstalling this package
re-registers the application's own copy of the renderer, so Unplugged keeps
working.

Close any running player before uninstalling; a loaded .ax cannot be deleted.


LICENSE
-------
This renderer is based on MPC Video Renderer and is distributed under the GNU
General Public License v3. See LICENSE.txt in the installation folder. The
corresponding source is available with the Unplugged project's source tree.