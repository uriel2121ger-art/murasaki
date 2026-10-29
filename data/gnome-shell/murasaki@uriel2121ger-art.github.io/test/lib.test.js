// Run with: node --test data/gnome-shell/murasaki@uriel2121ger-art.github.io/test
import assert from 'node:assert/strict';
import test from 'node:test';

import {OVERLAY_APP_ID, SURFACE_APP_ID, isMurasakiWindow, isPetWindow, isTrustedTitle, isWallpaperWindow, petInfo, wallpaperOutput} from '../lib/classify.js';
import {MAX_WINDOWS, buildReport, serialize} from '../lib/report.js';
import {petActions, petPlacements, petSignature, sameRect, wallpaperPlacements} from '../lib/placement.js';

const monitors = [
    {name: 'DP-1', rect: {x: 0, y: 0, width: 1920, height: 1080}},
    {name: 'HDMI-A-1', rect: {x: 1920, y: 0, width: 1280, height: 720}},
];
const win = (over = {}) => ({
    title: 'A window', appId: 'org.gnome.Nautilus', pid: 100, output: 'DP-1', workspace: 0, fullscreen: false,
    minimized: false, maximized: false, isNormal: true, rect: {x: 100, y: 100, width: 800, height: 600}, ...over,
});

test('window titles', () => {
    assert.equal(wallpaperOutput('Murasaki Wallpaper | DP-1'), 'DP-1');
    assert.equal(wallpaperOutput('Murasaki Wallpaper | '), null);
    assert.equal(wallpaperOutput('Murasaki Wallpaper'), null);
    assert.equal(wallpaperOutput(null), null);
    assert.equal(wallpaperOutput(undefined), null);
    assert.deepEqual(petInfo('Murasaki Pet | HDMI-A-1 | abc-123'), {output: 'HDMI-A-1', petId: 'abc-123', below: false, stage: null});
    assert.deepEqual(petInfo('Murasaki Pet | DP-1'), {output: 'DP-1', petId: '', below: false, stage: null});
    assert.deepEqual(petInfo('Murasaki Pet | DP-1 | abc | below'), {output: 'DP-1', petId: 'abc', below: true, stage: null});
    assert.equal(petInfo('Murasaki Pet | DP-1 | abc | above').below, false); // only the marker counts
    assert.equal(petInfo('Murasaki Pet | DP-1 | abc | below | more').below, true);
    assert.equal(petInfo('Murasaki Pet | DP-1 | abc|below').below, false);
    assert.equal(petInfo('Murasaki Pet | '), null);
    assert.equal(petInfo('A window titled Murasaki Pet | DP-1'), null);
    assert.ok(isWallpaperWindow('Murasaki Wallpaper | DP-1') && !isPetWindow('Murasaki Wallpaper | DP-1'));
    assert.ok(isMurasakiWindow('Murasaki Pet | DP-1 | x') && !isMurasakiWindow('Murasaki'));
});

test('a page can not pose as a wallpaper by its title unless the prefix matches exactly', () => {
    assert.equal(wallpaperOutput('murasaki wallpaper | DP-1'), null);
    assert.equal(wallpaperOutput(' Murasaki Wallpaper | DP-1'), null);
});

test('the report has the format parseKWinReport reads', () => {
    const report = buildReport({
        windows: [win({fullscreen: true, appId: 'mpv'}), win({output: 'HDMI-A-1', workspace: -1, appId: 'obs'})],
        monitors, workspaces: ['One', 'Two'], currentWorkspace: 0,
        focus: win({appId: 'mpv', fullscreen: true}), ownPid: 1,
    });
    assert.equal(report.currentDesktop, '0');
    assert.deepEqual(report.desktops, [{id: '0', name: 'One'}, {id: '1', name: 'Two'}]);
    assert.deepEqual(report.activeWindow, {resourceClass: 'mpv', fullScreen: true});
    assert.equal(report.windows.length, 2);
    assert.deepEqual(report.windows[0], {
        output: 'DP-1', desktops: ['0'], onAllDesktops: false, fullScreen: true, minimized: false, coversOutput: false,
        resourceClass: 'mpv',
    });
    assert.equal(report.windows[1].onAllDesktops, true);
    assert.deepEqual(report.windows[1].desktops, []);
});

test('covering: maximized, or geometry that fills the monitor', () => {
    const one = overrides => buildReport({windows: [win(overrides)], monitors, workspaces: ['1'], currentWorkspace: 0,
        focus: null, ownPid: 1}).windows[0].coversOutput;
    assert.equal(one({maximized: true}), true);
    assert.equal(one({rect: {x: 0, y: 0, width: 1920, height: 1080}}), true);
    assert.equal(one({rect: {x: -10, y: -10, width: 2000, height: 1200}}), true);
    assert.equal(one({rect: {x: 0, y: 0, width: 1919, height: 1080}}), false);
    assert.equal(one({}), false);
    assert.equal(one({output: 'UNKNOWN-1', maximized: false}), false);
});

test('Murasaki\'s own windows, abnormal windows and its own process are left out', () => {
    const report = buildReport({
        windows: [
            win({title: 'Murasaki Wallpaper | DP-1', appId: SURFACE_APP_ID}), win({title: 'Murasaki Pet | DP-1 | p', appId: OVERLAY_APP_ID}),
            win({isNormal: false}), win({pid: 1}), win({output: ''}), win(),
        ],
        monitors, workspaces: ['1'], currentWorkspace: 0, focus: win({title: 'Murasaki Wallpaper | DP-1', appId: SURFACE_APP_ID}), ownPid: 1,
    });
    assert.equal(report.windows.length, 1);
    assert.equal(report.activeWindow, null);
});

test('no focus, an abnormal focus or the surface\'s own focus report null', () => {
    const base = {windows: [], monitors, workspaces: ['1'], currentWorkspace: 0, ownPid: 1};
    assert.equal(buildReport({...base, focus: null}).activeWindow, null);
    assert.equal(buildReport({...base, focus: win({isNormal: false})}).activeWindow, null);
    assert.equal(buildReport({...base, focus: win({pid: 1})}).activeWindow, null);
});

test('missing application ids become empty strings', () => {
    const report = buildReport({windows: [win({appId: null})], monitors, workspaces: ['1'], currentWorkspace: 0, focus: null, ownPid: 1});
    assert.equal(report.windows[0].resourceClass, '');
});

test('serialization bounds the number of windows', () => {
    const many = Array.from({length: MAX_WINDOWS + 50}, () => win());
    const report = buildReport({windows: many, monitors, workspaces: ['1'], currentWorkspace: 0, focus: null, ownPid: 1});
    assert.equal(JSON.parse(serialize(report)).windows.length, MAX_WINDOWS);
    assert.equal(JSON.parse(serialize({...report, windows: []})).windows.length, 0);
});

test('wallpaper windows go to their output, windows of vanished outputs are left alone', () => {
    const windows = [
        {id: 1, title: 'Murasaki Wallpaper | DP-1', appId: SURFACE_APP_ID}, {id: 2, title: 'Murasaki Wallpaper | HDMI-A-1', appId: SURFACE_APP_ID},
        {id: 3, title: 'Murasaki Wallpaper | GONE-1', appId: SURFACE_APP_ID}, {id: 4, title: 'Firefox', appId: 'firefox'},
    ];
    const placements = wallpaperPlacements(windows, monitors);
    assert.deepEqual(placements.map(p => [p.id, p.output]), [[1, 'DP-1'], [2, 'HDMI-A-1']]);
    assert.deepEqual(placements[1].rect, monitors[1].rect);
});

test('pet windows go to the origin of their output', () => {
    const placements = petPlacements([{id: 7, title: 'Murasaki Pet | HDMI-A-1 | pet-1', appId: OVERLAY_APP_ID}, {id: 8, title: 'Murasaki Pet | X-1 | p', appId: OVERLAY_APP_ID}], monitors);
    assert.equal(placements.length, 1);
    assert.equal(placements[0].petId, 'pet-1');
    assert.deepEqual(placements[0].rect, monitors[1].rect);
});

test('a pet window with the below marker is placed like any other and says so', () => {
    const placements = petPlacements([{id: 1, title: 'Murasaki Pet | DP-1 | a | below', appId: OVERLAY_APP_ID}, {id: 2, title: 'Murasaki Pet | DP-1 | b', appId: OVERLAY_APP_ID}], monitors);
    assert.deepEqual(placements.map(p => [p.petId, p.below]), [['a', true], ['b', false]]);
    assert.deepEqual(placements[0].rect, monitors[0].rect);
});

test('the stage marker is read from the title, with or without the below marker', () => {
    assert.deepEqual(petInfo('Murasaki Pet | DP-1 | p | stage output'), {output: 'DP-1', petId: 'p', below: false, stage: 'output'});
    assert.deepEqual(petInfo('Murasaki Pet | DP-1 | p | stage desktop'), {output: 'DP-1', petId: 'p', below: false, stage: 'desktop'});
    assert.deepEqual(petInfo('Murasaki Pet | DP-1 | p | below | stage output'), {output: 'DP-1', petId: 'p', below: true, stage: 'output'});
    assert.deepEqual(petInfo('Murasaki Pet | DP-1 | p | stage output | below'), {output: 'DP-1', petId: 'p', below: true, stage: 'output'});
    // a pet title as before has no stage; words that are not ours are ignored (a stage with a mode we do not know is no stage)
    assert.equal(petInfo('Murasaki Pet | DP-1 | p').stage, null);
    assert.equal(petInfo('Murasaki Pet | DP-1 | p | stage').stage, null);
    assert.equal(petInfo('Murasaki Pet | DP-1 | p | stage fullscreen').stage, null);
    assert.equal(petInfo('Murasaki Pet | DP-1 | p | stage output extra').stage, null);
    assert.equal(petInfo('Murasaki Pet | DP-1 | p|stage output').stage, null);
    assert.equal(petInfo('Murasaki Pet | DP-1 | p | Stage output').stage, null);
    assert.deepEqual(petInfo('Murasaki Pet | DP-1 | p | stage output | stage desktop').stage, 'desktop');
});

test('a stage is placed on its output and kept under the pets; a window that only has the title is never touched', () => {
    const stage = {id: 1, title: 'Murasaki Pet | HDMI-A-1 | s | stage output', appId: OVERLAY_APP_ID};
    const placements = petPlacements([stage, {id: 2, title: 'Murasaki Pet | HDMI-A-1 | s2 | stage desktop', appId: OVERLAY_APP_ID}], monitors);
    assert.deepEqual(placements.map(p => [p.petId, p.stage]), [['s', 'output'], ['s2', 'desktop']]);
    assert.deepEqual(placements[0].rect, monitors[1].rect);
    // the actions, in order: above and at the bottom of the layer above (pets stay in front), on every workspace, on the output
    assert.deepEqual(petActions(placements[0]), ['unmake_below', 'make_above', 'lower', 'stick', 'place']);
    // an ordinary pet is not lowered; a pet or a stage that must be below goes below
    assert.deepEqual(petActions({below: false, stage: null}), ['unmake_below', 'make_above', 'stick', 'place']);
    assert.deepEqual(petActions({below: true, stage: null}), ['unmake_above', 'make_below', 'lower', 'stick', 'place']);
    assert.deepEqual(petActions({below: true, stage: 'output'}), ['unmake_above', 'make_below', 'lower', 'stick', 'place']);
    // a spoofed stage title is not placed
    assert.deepEqual(petPlacements([{id: 3, title: 'Murasaki Pet | DP-1 | e | stage output', appId: 'org.mozilla.firefox'}], monitors), []);
    // and it is not in the report for the pause rules (it is Murasaki's own, so it does not count as a window that covers the output)
    const report = buildReport({windows: [win({title: 'Murasaki Pet | DP-1 | s | stage output', appId: OVERLAY_APP_ID, rect: monitors[0].rect})],
        monitors, workspaces: ['1'], currentWorkspace: 0, focus: null, ownPid: 1});
    assert.equal(report.windows.length, 0);
});

test('a pet window already arranged for what it is only gets its place checked again', () => {
    const stage = {below: false, stage: 'output'};
    assert.equal(petSignature(stage), 'above|output');
    assert.equal(petSignature({below: true, stage: null}), 'below|');
    // the same signature: only the place (the stacking calls are what Mutter answers with signals)
    assert.deepEqual(petActions(stage, petSignature(stage)), ['place']);
    // another one, or none: all of it
    assert.deepEqual(petActions(stage, 'above|'), ['unmake_below', 'make_above', 'lower', 'stick', 'place']);
    assert.deepEqual(petActions(stage, null), ['unmake_below', 'make_above', 'lower', 'stick', 'place']);
    // a pet that becomes a stage, and a stage that goes below, are arranged again
    assert.notEqual(petSignature({below: false, stage: null}), petSignature(stage));
    assert.notEqual(petSignature(stage), petSignature({below: true, stage: 'output'}));
});

test('a window that only has the title is not Murasaki\'s: a web page can set its title to anything', () => {
    // trust needs the application id of the process that made the window: the overlay for pets, the surface for wallpapers
    assert.ok(isTrustedTitle('Murasaki Pet | DP-1 | x', OVERLAY_APP_ID));
    assert.ok(isTrustedTitle('Murasaki Wallpaper | DP-1', SURFACE_APP_ID));
    assert.ok(!isTrustedTitle('Murasaki Pet | DP-1 | x', 'firefox'));
    assert.ok(!isTrustedTitle('Murasaki Pet | DP-1 | x', SURFACE_APP_ID));        // the other process's id does not do
    assert.ok(!isTrustedTitle('Murasaki Wallpaper | DP-1', OVERLAY_APP_ID));
    assert.ok(!isTrustedTitle('Murasaki Pet | DP-1 | x', ''));
    assert.ok(!isTrustedTitle('Murasaki Pet | DP-1 | x', null));
    assert.ok(!isTrustedTitle('Some window', OVERLAY_APP_ID));
    const spoof = {id: 9, title: 'Murasaki Pet | DP-1 | evil | below', appId: 'org.mozilla.firefox'};
    assert.deepEqual(petPlacements([spoof], monitors), []);       // never placed, pinned or lowered
    assert.deepEqual(wallpaperPlacements([{id: 10, title: 'Murasaki Wallpaper | DP-1', appId: 'org.mozilla.firefox'}], monitors), []);
    // and in the report for the pause rules it is an ordinary window, not one to leave out
    const report = buildReport({windows: [win({title: 'Murasaki Pet | DP-1 | evil', appId: 'org.mozilla.firefox'})],
        monitors, workspaces: ['1'], currentWorkspace: 0, focus: null, ownPid: 1});
    assert.equal(report.windows.length, 1);
    assert.equal(report.windows[0].resourceClass, 'org.mozilla.firefox');
});

test('same rectangle', () => {
    assert.ok(sameRect({x: 1, y: 2, width: 3, height: 4}, {x: 1, y: 2, width: 3, height: 4}));
    assert.ok(!sameRect({x: 1, y: 2, width: 3, height: 4}, {x: 1, y: 2, width: 3, height: 5}));
});

test('monitors get their connector names from DisplayConfig', async () => {
    const {namedMonitors} = await import('../lib/monitors.js');
    const logical = [
        [0, 0, 1, 0, true, [['DP-1', 'DEL', 'U2723QE', '1']], {}],
        [1920, 0, 1, 0, false, [['HDMI-A-1', 'AOC', 'x', '2'], ['DP-2', 'AOC', 'x', '3']], {}], // mirrored pair
        [0, 1080, 1, 0, false, [], {}],
    ];
    const layout = [
        {x: 0, y: 0, width: 1920, height: 1080}, {x: 1920, y: 0, width: 1280, height: 720},
        {x: 5000, y: 0, width: 800, height: 600},
    ];
    const named = namedMonitors(logical, layout);
    assert.deepEqual(named.map(m => m.name), ['DP-1', 'HDMI-A-1']);
    assert.deepEqual(named[1].rect, {x: 1920, y: 0, width: 1280, height: 720});
    assert.deepEqual(namedMonitors([], layout), []);
    assert.deepEqual(namedMonitors([[0, 0, 1, 0, true, 'garbage', {}]], layout), []);
});
