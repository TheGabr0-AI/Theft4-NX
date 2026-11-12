# Renut at a later XDK: notes from other ports

The base's Renut stands at Beautiful Katamari's XDK 2.0.5632. These are what ports at a later XDK met, for the next
port's remap of the hooks and the device layout. Each is that port's finding on its own image; check it on yours.

## XDK 2.0.11164 (blur-nx, 2026-11-10)

Evidence: blur-nx's `docs/blur-notes.md`, "Renut at XDK 2.0.11164" (D:\blur\blur-nx), with the tools named there.

- **The first release's unused second argument.** D3D_Lock2DSurface, D3D_LockSurface, D3D_LockResource and
  D3DDevice_BeginCommandBuffer take one more argument than at 5632, unused. pacman-nx met three of them at XDK 21250;
  they are already there at 11164.
- **SetGammaRamp and SetPWLGamma** take (device, flags, ramp).
- **D3DVertexShader_Bind** is (shader r3, flags r4, declaration r5, stream strides r6, pixel shader r7). Katamari never
  links it, so it is one of the base's `BANJO_RENUT_UNHOOKED_*` bodies: left unhooked, Blur's ran on Renut's declaration
  record and walked off the guest stack. blur-nx hooks it through `D3DVertexShader_Bind11164_hook` (the flags and the
  pixel shader dropped: Renut keeps the declaration and the strides).
- **XGRegisterPixelShader** is linked and makes the pixel shaders where CreatePixelShader is not linked (Blur); its
  instructions write what Renut's hook writes.
- **The tiling Flags word.** pacman-nx's N&B -> 11164 device-offset table moves N&B's 13196 by +176; Blur's BeginTiling
  stores it at 13364, +168. Check that word against your BeginTiling.
- **The state tables.** `tools/renut_state_tables.py --ref-render 82AD8E48 --ref-sampler 82AD9308` pairs a game's render
  and sampler state tables with Katamari's (port 69's options); Blur's are 0x82B9A888 and 0x82B9AD48, all 121 records
  paired by their defaults.
- **Vertex shaders with two bindings.** Blur makes about 1,000 vertex shaders at run time whose container has flags bit
  0x20 (0x102A1121). XenosRecomp's dump reader skipped them before port 68.
