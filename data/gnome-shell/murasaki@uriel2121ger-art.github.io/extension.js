// Murasaki for GNOME Shell 46-50 (ES modules). The decisions live in lib/*.js (pure, tested with node); this file only
// talks to the shell. See docs/gnome.md, and the spec docs/design/specs/2026-10-17-murasaki-gnome-design.md for why
// the wallpaper is a pinned window (there is no way to import the engine's DMA-BUFs from JS).
//
// NOT TESTED against a running shell yet (gnome-shell was not available when this was written). The calls are the ones
// documented for the extension API; every place that touches shell internals is guarded, so that a mismatch in one
// GNOME version disables that piece and logs, instead of breaking the shell.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';

import {Extension, InjectionManager} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as Workspace from 'resource:///org/gnome/shell/ui/workspace.js';

import {isPetWindow, isTrustedTitle, isWallpaperWindow, SURFACE_BUS_NAME, SURFACE_IFACE, SURFACE_PATH} from './lib/classify.js';
import {namedMonitors} from './lib/monitors.js';
import {petActions, petPlacements, petSignature, sameRect, wallpaperPlacements} from './lib/placement.js';
import {buildReport, serialize} from './lib/report.js';

const REPORT_DELAY_MS = 100; // one report per burst of changes

// Mutter 18 (GNOME 50) renamed get_maximized() to get_maximize_flags(); same flags.
function maximizeFlags(window) {
    return typeof window.get_maximize_flags === 'function' ? window.get_maximize_flags() : window.get_maximized();
}

export default class MurasakiExtension extends Extension {
    enable() {
        this._ids = []; // [object, signal id]
        this._timeouts = new Set();
        this._pinned = new Set(); // wallpaper window actors reparented into the background group
        this._petApplied = new WeakMap(); // pet window -> what it was last arranged for (petSignature)
        this._monitors = [];
        this._reportPending = false;
        this._injections = new InjectionManager();

        this._hideFromLists();
        this._watchMonitors();

        this._connect(global.display, 'window-created', (_display, window) => this._onWindowCreated(window));
        this._connect(global.display, 'notify::focus-window', () => this._scheduleReport());
        this._connect(global.workspace_manager, 'active-workspace-changed', () => this._scheduleReport());
        this._connect(global.workspace_manager, 'workspace-added', () => this._scheduleReport());
        this._connect(global.workspace_manager, 'workspace-removed', () => this._scheduleReport());
        this._connect(Main.layoutManager, 'monitors-changed', () => this._watchMonitors());

        for (const actor of global.get_window_actors())
            this._onWindowCreated(actor.meta_window);

        // The surface may start after the shell (or restart): report again whenever it appears on the bus.
        this._watch = Gio.bus_watch_name(Gio.BusType.SESSION, SURFACE_BUS_NAME, Gio.BusNameWatcherFlags.NONE,
            () => { this._surfaceUp = true; this._scheduleReport(); this._arrangeAll(); },
            () => { this._surfaceUp = false; });
    }

    disable() {
        if (this._watch)
            Gio.bus_unwatch_name(this._watch);
        this._watch = 0;
        for (const [object, id] of this._ids)
            object.disconnect(id);
        this._ids = [];
        for (const source of this._timeouts)
            GLib.source_remove(source);
        this._timeouts.clear();
        this._injections?.clear();
        this._injections = null;
        // Give the wallpaper windows back to the compositor's window group, where they were.
        for (const actor of this._pinned)
            this._unpin(actor);
        this._pinned.clear();
    }

    // ---- plumbing ----------------------------------------------------------------------------------------------

    _connect(object, signal, handler) {
        this._ids.push([object, object.connect(signal, handler)]);
    }

    _later(ms, callback) {
        const source = GLib.timeout_add(GLib.PRIORITY_DEFAULT, ms, () => {
            this._timeouts.delete(source);
            callback();
            return GLib.SOURCE_REMOVE;
        });
        this._timeouts.add(source);
    }

    _guard(what, callback) {
        try {
            return callback();
        } catch (error) {
            console.error(`Murasaki: ${what}: ${error}`);
            return undefined;
        }
    }

    // ---- monitors ----------------------------------------------------------------------------------------------

    _watchMonitors() {
        const layout = Main.layoutManager.monitors.map(({x, y, width, height}) => ({x, y, width, height}));
        // The connector names come from Mutter's DisplayConfig, asynchronously.
        Gio.DBus.session.call('org.gnome.Mutter.DisplayConfig', '/org/gnome/Mutter/DisplayConfig',
            'org.gnome.Mutter.DisplayConfig', 'GetCurrentState', null, null, Gio.DBusCallFlags.NONE, 2000, null,
            (connection, result) => this._guard('GetCurrentState', () => {
                const state = connection.call_finish(result).recursiveUnpack();
                this._monitors = namedMonitors(state[2], layout);
                this._arrangeAll();
                this._scheduleReport();
            }));
    }

    // ---- windows -----------------------------------------------------------------------------------------------

    // The application id of a window, as the report and the trust check use it.
    _appId(window) {
        return window.get_gtk_application_id?.() ?? window.get_wm_class?.() ?? '';
    }

    // Murasaki's own window: by title AND by the application id of the process that made it (a web page can only set a title).
    _isMurasaki(window) {
        return isTrustedTitle(window.get_title?.(), this._appId(window));
    }

    _describe(window, id) {
        const monitor = this._monitors.find(candidate => {
            const rect = window.get_frame_rect();
            return rect.x >= candidate.rect.x && rect.x < candidate.rect.x + candidate.rect.width &&
                rect.y >= candidate.rect.y && rect.y < candidate.rect.y + candidate.rect.height;
        });
        const rect = window.get_frame_rect();
        return {
            id, title: window.get_title(), appId: this._appId(window),
            pid: window.get_pid(), output: monitor?.name ?? '',
            workspace: window.is_on_all_workspaces() ? -1 : (window.get_workspace()?.index() ?? -1),
            fullscreen: window.is_fullscreen(), minimized: window.minimized,
            maximized: maximizeFlags(window) === Meta.MaximizeFlags.BOTH,
            isNormal: window.get_window_type() === Meta.WindowType.NORMAL && !this._isMurasaki(window),
            rect: {x: rect.x, y: rect.y, width: rect.width, height: rect.height},
        };
    }

    _onWindowCreated(window) {
        this._guard('window', () => {
            const track = () => {
                for (const signal of ['notify::title', 'notify::fullscreen', 'notify::minimized', 'notify::maximized-horizontally',
                    'notify::maximized-vertically', 'workspace-changed', 'position-changed', 'size-changed', 'notify::appears-focused'])
                    this._connect(window, signal, () => { this._arrange(window); this._scheduleReport(); });
                this._connect(window, 'unmanaged', () => this._scheduleReport());
            };
            track();
            this._arrange(window);
            this._scheduleReport();
        });
    }

    _arrangeAll() {
        for (const actor of global.get_window_actors())
            this._arrange(actor.meta_window);
    }

    // Pins what is Murasaki's. The title is only known a moment after the window exists, hence the re-arranging on
    // 'notify::title'.
    _arrange(window) {
        this._guard('arrange', () => {
            const title = window.get_title();
            if (!this._isMurasaki(window))
                return;
            const description = [{id: window, title, appId: this._appId(window)}];
            if (isWallpaperWindow(title)) {
                const [placement] = wallpaperPlacements(description, this._monitors);
                if (!placement)
                    return;
                this._pin(window, placement.rect);
            } else if (isPetWindow(title)) {
                const [placement] = petPlacements(description, this._monitors);
                if (!placement)
                    return;
                // keepAbove off (a "below" marker in the title): not above the windows. Mutter has no layer, so the pet becomes an ordinary stacked
                // window pushed down; a stage (the "stage" marker) is kept at the bottom of the layer above the windows so that the pets stay in front of it.
                // Experimental like the rest: the actions are decided in lib/placement.js (petActions, tested), every call is guarded by the optional chaining
                // and `_guard`.
                // (only what differs from the last arrangement of this window is done again: a stacking call answered by Mutter's own signals must not start a loop)
                for (const action of petActions(placement, this._petApplied.get(window) ?? null)) {
                    if (action === 'place')
                        this._place(window, placement.rect);
                    else
                        window[action]?.();
                }
                this._petApplied.set(window, petSignature(placement));
            }
        });
    }

    _place(window, rect) {
        const current = window.get_frame_rect();
        if (!sameRect(current, rect))
            window.move_resize_frame(false, rect.x, rect.y, rect.width, rect.height);
    }

    // The wallpaper window becomes a child of the background group: under every window and the desktop icons.
    _pin(window, rect) {
        const actor = window.get_compositor_private();
        if (!actor)
            return;
        window.stick();
        window.lower?.();
        this._place(window, rect);
        const group = Main.layoutManager._backgroundGroup;
        if (!group || actor.get_parent() === group)
            return;
        actor.get_parent()?.remove_child(actor);
        group.add_child(actor);
        group.set_child_above_sibling(actor, null);
        this._pinned.add(actor);
        this._connect(actor, 'destroy', () => this._pinned.delete(actor));
    }

    _unpin(actor) {
        this._guard('unpin', () => {
            const parent = actor.get_parent();
            if (parent && parent === Main.layoutManager._backgroundGroup) {
                parent.remove_child(actor);
                global.window_group.add_child(actor);
            }
        });
    }

    // Murasaki's windows are not windows the user manages: out of the overview and Alt-Tab, like a desktop background.
    _hideFromLists() {
        this._guard('overview', () => {
            this._injections.overrideMethod(Workspace.Workspace.prototype, '_isOverviewWindow', original => function (window) {
                const meta = window.get_meta_window?.() ?? window;
                if (isTrustedTitle(meta.get_title?.(), meta.get_gtk_application_id?.() ?? meta.get_wm_class?.() ?? ''))
                    return false;
                return original.call(this, window);
            });
        });
        this._guard('tab list', () => {
            this._injections.overrideMethod(Meta.Display.prototype, 'get_tab_list', original => function (...args) {
                return original.call(this, ...args).filter(window => !isTrustedTitle(window.get_title(), window.get_gtk_application_id?.() ?? window.get_wm_class?.() ?? ''));
            });
        });
        this._guard('window tracker', () => {
            this._injections.overrideMethod(Shell.WindowTracker.prototype, 'is_window_interesting', original => function (window) {
                return !isTrustedTitle(window.get_title(), window.get_gtk_application_id?.() ?? window.get_wm_class?.() ?? '') && original.call(this, window);
            });
        });
    }

    // ---- the report for the pause rules ------------------------------------------------------------------------

    _scheduleReport() {
        if (this._reportPending)
            return;
        this._reportPending = true;
        this._later(REPORT_DELAY_MS, () => {
            this._reportPending = false;
            this._sendReport();
        });
    }

    _sendReport() {
        this._guard('report', () => {
            const windows = global.get_window_actors().map((actor, index) => this._describe(actor.meta_window, index));
            const focus = global.display.focus_window;
            const workspaceManager = global.workspace_manager;
            const names = [];
            for (let i = 0; i < workspaceManager.get_n_workspaces(); i++)
                names.push(Meta.prefs_get_workspace_name?.(i) ?? `Workspace ${i + 1}`);
            const report = buildReport({
                windows, monitors: this._monitors, workspaces: names,
                currentWorkspace: workspaceManager.get_active_workspace_index(),
                focus: focus ? this._describe(focus, -1) : null,
                ownPid: -1, // Murasaki's windows are told apart by title; the surface ignores its own anyway
            });
            Gio.DBus.session.call(SURFACE_BUS_NAME, SURFACE_PATH, SURFACE_IFACE, 'ReportShellState',
                new GLib.Variant('(s)', [serialize(report)]), null, Gio.DBusCallFlags.NO_AUTO_START, 1000, null, null);
        });
    }
}
