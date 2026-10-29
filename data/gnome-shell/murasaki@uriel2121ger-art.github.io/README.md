# Murasaki GNOME Shell extension

Pins Murasaki's windows (wallpapers under everything, pets above everything) and reports window state to `murasaki-surface`.
See `docs/gnome.md` of the repository. Install: `cmake --install` puts it in `share/gnome-shell/extensions/`; enable it with
`gnome-extensions enable murasaki@uriel2121ger-art.github.io` (log out and in on Wayland for a new install).

The logic is in `lib/` and tested with `node --test test/lib.test.js`.
