# Security

## Reporting a problem

If you find a security problem in QuickMon Forever, please report it privately rather than in a
public issue: use **Report a vulnerability** on this repository's **Security** tab (GitHub's
private vulnerability reporting). Please include the version (shown in the About box), what you
found, and how to reproduce it.

I'll reply as soon as I can, and credit you in the fix unless you'd rather not be named.

## Supported versions

Fixes go into the latest version. QuickMon Forever is a small free project, so older versions are
not patched separately.

## How QuickMon Forever is built to be safe

- **No third-party code.** It uses only Windows' own components and Microsoft's C runtime (built
  into the program), so there are no outside libraries to carry known vulnerabilities.
- **Hardened builds.** Stack checks (`/GS`), extra compile-time security checks (`/sdl`), Control
  Flow Guard, address randomization (ASLR, high-entropy on 64-bit), DEP, and shadow-stack
  compatibility (`/CETCOMPAT`).
- **System DLLs from System32 only.** The program never loads a Windows DLL from its own folder,
  so a planted file next to a downloaded `.scr` can't be loaded in its place.
- **The 32-bit helper** is started by its full path from the program's own folder, and talks to
  the screen saver over private anonymous pipes.
- **No temporary files.** Data passed between the parts of the program goes over those private
  pipes, or (for Set Default's elevated step) a volatile registry key that is deleted straight
  after and never touches the disk.
- **The sign-in screen saver.** **Set Default** only works when the program is installed in a
  folder that only administrators can change (the Windows folder or Program Files), so nobody can
  swap in their own program to run on the sign-in screen.
- **No network connections of its own.** It reads performance counters (from other computers only
  when you add a remote counter yourself) and looks up the PC's own name and IP address; it sends
  nothing anywhere.
