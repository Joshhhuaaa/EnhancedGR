# Enhanced GR

A patch for the original Ghost Recon, fixing bugs and adding gameplay improvements.

If you'd like to donate, all contributions are appreciated.
<div align="left">
  <a href="https://www.paypal.com/donate/?hosted_button_id=UB67N4GNTCEZ6">
    <img src="https://github.com/user-attachments/assets/6a8878e8-3ae8-48e5-8d2a-ae367c71df10" width="256" alt="PayPal"/>
  </a>
</div>

## Installation
The latest version of Enhanced GR can be found on the [Releases](https://github.com/Joshhhuaaa/EnhancedGR/releases) page.

### Game Setup
> [!IMPORTANT]
> Rename or delete `dbghelp.dll` in your Ghost Recon directory. This is required to prevent the patch from crashing on startup.

- After downloading Enhanced GR, extract the contents to your Ghost Recon directory and overwrite all existing files when prompted.
- You can adjust additional settings in `EnhancedGR.ini` located in the `plugins` folder.

## Uninstallation
- Navigate to the game folder, delete the `plugins` folder, `d3d8.dll`, and `dinput8.dll`.

## Features
### Widescreen Support
In the stock game, menus and cutscenes are hardcoded to render at 640x480, while the HUD stretches at widescreen aspect ratios. Enhanced GR renders menus and cutscenes at the in-game resolution and dynamically scales HUD elements to maintain their original proportions at any resolution.

Field of view is calculated automatically based on the aspect ratio, widening the horizontal FOV while preserving the vertical FOV from 4:3.

<div align="center">
  <table>
    <tr>
      <td width="50%"><img style="width:100%" src="https://github.com/user-attachments/assets/b2cbf094-bd8a-4683-a339-f7a1eebbef8f"></td>
      <td width="50%"><img style="width:100%" src="https://github.com/user-attachments/assets/28526a82-ceba-4869-81c8-3fd83f662cce"></td>
    </tr>
    <tr>
      <td align="center">Stock</td>
      <td align="center">Enhanced</td>
    </tr>
  </table>
</div>

### Raw Input
Mouse input is read directly, reducing overhead at high polling rates and helping prevent stuttering at extreme rates such as 8000 Hz.

### Mouse Sensitivity Multiplier
Separate sensitivity multipliers for in-game aiming and the menu cursor allow for more control than the in-game slider.

### Borderless Support
Adds an option to run the game in borderless windowed mode. Borderless always renders at the native resolution, regardless of the in-game resolution setting.

### Anisotropic Filtering
Forces anisotropic texture filtering.

<div align="center">
  <table>
    <tr>
      <td width="50%"><img style="width:100%" src="https://github.com/user-attachments/assets/6fd54166-494c-457f-b7b6-5fc7acfd332f"></td>
      <td width="50%"><img style="width:100%" src="https://github.com/user-attachments/assets/8eb1dd32-1dc9-4929-8f8b-570b35c18538"></td>
    </tr>
    <tr>
      <td align="center">Stock</td>
      <td align="center">Anisotropic 16x</td>
    </tr>
  </table>
</div>

### Multisample Antialiasing (MSAA)
Enables MSAA to smooth jagged edges while preserving a sharp image. MSAA does not smooth alpha-tested edges such as fences and foliage.

<div align="center">
  <table>
    <tr>
      <td width="50%"><img style="width:100%" src="https://github.com/user-attachments/assets/d02f3890-e611-4459-be0f-5dd07a557d0b"></td>
      <td width="50%"><img style="width:100%" src="https://github.com/user-attachments/assets/5349b4a0-a1df-4a46-9673-e7346a43574b"></td>
    </tr>
    <tr>
      <td align="center">Stock</td>
      <td align="center">MSAA 8x</td>
    </tr>
  </table>
</div>

### Subpixel Morphological Antialiasing (SMAA)
Enables SMAA to smooth jagged edges with a softer image. SMAA also smooths alpha-tested edges such as fences and foliage.

<div align="center">
  <table>
    <tr>
      <td width="50%"><img style="width:100%" src="https://github.com/user-attachments/assets/5e2c04a0-aa0d-404c-bea7-aa35f1c997b1"></td>
      <td width="50%"><img style="width:100%" src="https://github.com/user-attachments/assets/759f76a4-a707-437c-9ba2-230f4bc1f8ad"></td>
    </tr>
    <tr>
      <td align="center">Stock</td>
      <td align="center">SMAA</td>
    </tr>
  </table>
</div>

### Optic Aspect Ratio
`OpticMaskAspect` controls the aspect ratio of fullscreen optics such as night vision, binoculars, and sniper scopes.

| Value | Description                                                           |
| ----- | --------------------------------------------------------------------- |
| `0`   | Stretched to fill the screen, matching the stock game.                |
| `1`   | Maintains the original 4:3 aspect ratio with black bars on the sides. |
| `2`   | Maintains the original aspect ratio while zooming to fill the screen. |

<div align="center">
  <table>
    <tr>
      <td width="33.33%"><img style="width:100%" src="https://github.com/user-attachments/assets/77405715-e008-43c3-8a1a-329d000fc7da"></td>
      <td width="33.33%"><img style="width:100%" src="https://github.com/user-attachments/assets/c9ea9b74-073a-4fce-a3f5-0b1a6234299b"></td>
      <td width="33.33%"><img style="width:100%" src="https://github.com/user-attachments/assets/5bffcf30-8832-4a71-a571-d7f8c822264c"></td>
    </tr>
    <tr>
      <td align="center">Stock (stretched)</td>
      <td align="center">4:3 (black bars)</td>
      <td align="center">16:9 (zoomed)</td>
    </tr>
  </table>
</div>

### Framerate Limiter
Sets a maximum framerate. A value of `0` disables the limiter, `-1` matches the monitor's refresh rate, and any other value enables a hard cap at that value.
