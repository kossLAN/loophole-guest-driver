loophole guest driver
=====================

Run loophole-setup.exe once Windows is installed. It installs:

- the GPU driver: display, Vulkan (RADV), Direct3D 8-12 and OpenGL, which
  every application and the desktop use;
- an agent that keeps the desktop at the size and refresh rate of the
  loophole window;
- DXVK, vkd3d-proton and Zink to use in place of the driver for a game:
  right-click its .exe and choose "Use loophole for Direct3D and OpenGL".

The driver is test-signed. Setup turns on test signing (Secure Boot is off in
loophole VMs), so Windows shows "Test Mode" in a corner of the desktop.
Restart Windows when setup asks.

If Windows refuses to run loophole-setup.exe ("Smart App Control blocked
an app"), turn Smart App Control off in Windows Security > App & browser
control.

Once the driver works, eject this disc from the loophole window.
