# Security & Windows Defender / SmartScreen notes

`nightscreen.exe` is **unsigned** (no paid code-signing certificate yet). This is
normal for independent open-source tools, but Windows will treat the binary with
extra suspicion. This document explains what to expect, why the binary is
low-risk by construction, and how to get it allow-listed.

## What you may see on Windows (and what it means)

| Message | Meaning | Action |
|---|---|---|
| SmartScreen: "Unknown publisher" / "Windows protected your PC" | Expected for **any** unsigned download, even perfectly safe ones. Reputation builds over time/downloads; instant trust requires a paid cert. | Click *More info → Run anyway* (once per file). |
| Browser marks the zip "harmful" or deletes it | Usually a heuristic false positive on uncommon binaries. | Verify the hash (below), then restore/allow. |
| Defender quarantines it as Trojan:Win32/… | Would be a **false positive** — please report it (see below) so we can submit the hash to Microsoft. | Submit the file at <https://www.microsoft.com/en-us/wdsi/filesubmission> or open an issue. |

Right-click → *Properties* on the exe shows **Product name: night-screen**,
**Original filename: nightscreen.exe** — malware rarely bothers.

## Verify before you run

Each release ships `SHA256SUMS.txt`. On Windows (PowerShell):

```powershell
certutil -hashfile nightscreen-windows-x64.zip SHA256
# compare with the value in SHA256SUMS.txt
Expand-Archive nightscreen-windows-x64.zip .
Unblock-File nightscreen.exe   # clears the download "Mark of the Web" flag
```

You can also upload the zip to <https://www.virustotal.com> — 60+ scanners,
no account needed.

## Why this binary is low-risk by construction

- **100% open source, reproducible build**: the exact compiler invocations are
  in `CMakeLists.txt` + `cmake/toolchain-windows-zig.cmake`; anyone can rebuild
  byte-equivalent logic from this repo.
- **No packer / obfuscation**: not UPX-packed or encrypted (packers are a top
  malware-heuristic trigger). Imports are system DLLs only
  (`GDI32/USER32/WS2_32/ADVCTL…` — check with `strings` or Dependencies.exe).
- **Signed identity metadata**: embedded VERSIONINFO
  (CompanyName, FileDescription, OriginalFilename) + exe icon.
- **Loopback networking only**: the control panel binds `127.0.0.1` — no
  Windows Firewall prompt, no outbound connections, no telemetry of any kind.
- **No admin required**: gamma ramps, tray icon, and `HKCU` autostart all work
  as a standard user.
- **Static standard libraries**: no side-loaded DLLs to hijack.

## For the maintainer: reducing warnings further

1. **Microsoft file submission** (free, ~24–48 h): upload each release exe at
   <https://www.microsoft.com/en-us/wdsi/filesubmission> as a developer;
   confirmed-clean files get allow-listed by hash.
2. **Code signing** (the real fix for SmartScreen): an OV certificate
   (~$100–300/yr: SSL.com, DigiCert) plus accumulated reputation removes the
   warning; EV gives instant reputation. OSS projects can also use
   [SignPath.io](https://signpath.io) (free for open source).
3. Never "fix" warnings with a packer, crypter, or cert from a gray source —
   that makes detection *worse*.

## Reporting a security issue

Do not open a public issue for vulnerabilities — see the repo profile email.
For suspected false-positive detections, open a normal issue with the
Defender threat name, engine version, and the SHA-256 of your download.
